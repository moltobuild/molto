#include <molto/services/paths_service.h>

#include <molto/services/fs_service.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Pointed somewhere else entirely, when the default will not do. */
#define MOLTO_HOME_ENV "MOLTO_HOME"

/* What the directory is called inside each base directory. */
#define MOLTO_DIRNAME "molto"

/* What it used to be called inside a home, before it followed the platform. */
#define LEGACY_DIRNAME ".molto"

/* The one file of configuration, and the one name of the cache inside a home
   laid out the old way. */
#define CREDENTIALS_FILE "credentials.toml"
#define CACHE_DIRNAME "cache"

static const char *nonempty_env(const char *name) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : NULL;
}

/* `$MOLTO_HOME`, when someone named one. */
static const char *override_home(void) { return nonempty_env(MOLTO_HOME_ENV); }

/* The person's home directory. */
static const char *user_home(void) {
    const char *home = nonempty_env("HOME");
#ifdef _WIN32
    /* Windows names it differently, and the shells it ships with set only the
       other one. Asked in this order rather than the reverse because a shell
       that defines HOME on Windows means it: MSYS2 and git-bash point it at a
       home of their own choosing, and a tool run from there should agree with
       everything else run from there. */
    if(home == NULL)
        home = nonempty_env("USERPROFILE");
#endif
    return home;
}

#ifdef _WIN32
/*
 * Everything under %APPDATA%\molto, by choice: one place a Windows user can
 * find. %APPDATA% roams with a domain profile, which suits the credential and
 * the plugins and is wasteful for the cache, so the data root and the cache
 * root are each answered by one function below: moving either to
 * %LOCALAPPDATA% is a change to the variable it names, and nothing else.
 */
static bool appdata_dir(const char *variable, char *out, size_t size) {
    const char *base = nonempty_env(variable);
    return base != NULL && fs_format_path(out, size, "%s/%s", base, MOLTO_DIRNAME);
}

static bool platform_config_dir(char *out, size_t size) {
    return appdata_dir("APPDATA", out, size);
}

static bool platform_data_dir(char *out, size_t size) { return appdata_dir("APPDATA", out, size); }

static bool platform_cache_dir(char *out, size_t size) {
    char root[MOLTO_HOME_PATH_MAX];
    return appdata_dir("APPDATA", root, sizeof root) &&
           fs_format_path(out, size, "%s/%s", root, CACHE_DIRNAME);
}
#else
/* `$<variable>/molto` when the variable holds an absolute path, and
   `<home>/<fallback>/molto` otherwise — the XDG Base Directory rule, which
   says a relative value is to be ignored. */
static bool xdg_dir(const char *variable, const char *fallback, char *out, size_t size) {
    const char *base = nonempty_env(variable);
    if(base != NULL && base[0] == '/')
        return fs_format_path(out, size, "%s/%s", base, MOLTO_DIRNAME);

    const char *home = user_home();
    return home != NULL && fs_format_path(out, size, "%s/%s/%s", home, fallback, MOLTO_DIRNAME);
}

static bool platform_config_dir(char *out, size_t size) {
    return xdg_dir("XDG_CONFIG_HOME", ".config", out, size);
}

static bool platform_data_dir(char *out, size_t size) {
    return xdg_dir("XDG_DATA_HOME", ".local/share", out, size);
}

static bool platform_cache_dir(char *out, size_t size) {
    return xdg_dir("XDG_CACHE_HOME", ".cache", out, size);
}
#endif

bool paths_molto_config_dir(char *out, size_t size) {
    const char *home = override_home();
    if(home != NULL)
        return fs_format_path(out, size, "%s", home);
    return platform_config_dir(out, size);
}

bool paths_molto_data_dir(char *out, size_t size) {
    const char *home = override_home();
    if(home != NULL)
        return fs_format_path(out, size, "%s", home);
    return platform_data_dir(out, size);
}

bool paths_molto_cache_dir(char *out, size_t size) {
    const char *home = override_home();
    if(home != NULL)
        return fs_format_path(out, size, "%s/%s", home, CACHE_DIRNAME);
    return platform_cache_dir(out, size);
}

bool paths_molto_data_subdir(const char *subdirectory, char *out, size_t size) {
    char data[MOLTO_HOME_PATH_MAX];
    if(!paths_molto_data_dir(data, sizeof data))
        return false;
    return fs_format_path(out, size, "%s/%s", data, subdirectory);
}

bool paths_molto_legacy_home(char *out, size_t size) {
    const char *home = user_home();
    return home != NULL && fs_format_path(out, size, "%s/%s", home, LEGACY_DIRNAME);
}

/* --- the migration from ~/.molto --- */

/* What one run of the migration did, for the note it leaves. */
typedef struct {
    bool moved_credentials;
    bool moved_plugins;
    bool discarded_cache;
    bool left_something; /* a file not moved, so ~/.molto stays */
} migration_outcome;

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* Copy a credential into place without it ever being readable by anybody else:
   created 0600 under a private temporary name, then renamed over nothing. Only
   for when a rename cannot do it, which is across filesystems. */
static bool copy_private(const char *from, const char *to) {
    char *content = fs_read_file(from);
    if(content == NULL)
        return false;

    char temporary[MOLTO_HOME_PATH_MAX + 16];
    bool ok = fs_format_path(temporary, sizeof temporary, "%s.new", to);
    if(ok) {
        const int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, S_IRUSR | S_IWUSR);
        const size_t length = strlen(content);
        ok = fd >= 0;
        if(ok) {
            const ssize_t written = write(fd, content, length);
            ok = close(fd) == 0 && written >= 0 && (size_t)written == length &&
                 !fs_path_exists(to) && rename(temporary, to) == 0;
            if(!ok)
                (void)unlink(temporary);
        }
    }
    free(content);
    return ok && unlink(from) == 0;
}

static void move_credentials(const char *legacy, const char *config, migration_outcome *outcome) {
    char from[MOLTO_HOME_PATH_MAX];
    char to[MOLTO_HOME_PATH_MAX];
    if(!fs_format_path(from, sizeof from, "%s/%s", legacy, CREDENTIALS_FILE) ||
       !fs_format_path(to, sizeof to, "%s/%s", config, CREDENTIALS_FILE)) {
        outcome->left_something = true;
        return;
    }
    if(!fs_path_exists(from))
        return;
    /* One somebody logged in with since is the one that counts. */
    if(fs_path_exists(to) || !fs_make_dirs(config)) {
        outcome->left_something = true;
        return;
    }

    /* A rename keeps the file and its mode, so there is no moment at which a
       copy of the token sits somewhere with different permissions; and the
       mode is made private before the move rather than after it, so a file an
       older molto left readable is never readable at its new address. */
    (void)chmod(from, S_IRUSR | S_IWUSR);
    if(rename(from, to) == 0 || copy_private(from, to)) {
        (void)chmod(to, S_IRUSR | S_IWUSR);
        outcome->moved_credentials = true;
    } else {
        outcome->left_something = true;
    }
}

/* Move one entry of a plugin directory, by rename where it can be done, and by
   copying a file where the two directories are on different filesystems. */
static bool move_entry(const char *from, const char *to) {
    if(rename(from, to) == 0)
        return true;
    if(fs_is_dir_no_follow(from))
        return false;

    struct stat info;
    if(stat(from, &info) != 0 || !fs_copy_file(from, to))
        return false;
    (void)chmod(to, info.st_mode & 0777);
    return unlink(from) == 0;
}

/* Move `<legacy>/plugins/<which>` into `<data>/plugins/<which>`: whole when
   there is nothing at the destination, entry by entry when there is. */
static void move_plugin_dir(const char *legacy, const char *data, const char *which,
                            migration_outcome *outcome) {
    char from[MOLTO_HOME_PATH_MAX];
    char to[MOLTO_HOME_PATH_MAX];
    char parent[MOLTO_HOME_PATH_MAX];
    if(!fs_format_path(from, sizeof from, "%s/plugins/%s", legacy, which) ||
       !fs_format_path(to, sizeof to, "%s/plugins/%s", data, which) ||
       !fs_format_path(parent, sizeof parent, "%s/plugins", data)) {
        outcome->left_something = true;
        return;
    }
    if(!fs_is_dir_no_follow(from))
        return;
    if(!fs_make_dirs(parent)) {
        outcome->left_something = true;
        return;
    }
    if(!fs_path_exists(to) && rename(from, to) == 0) {
        outcome->moved_plugins = true;
        return;
    }
    if(!fs_make_dirs(to)) {
        outcome->left_something = true;
        return;
    }

    DIR *handle = opendir(from);
    if(handle == NULL) {
        outcome->left_something = true;
        return;
    }
    for(const struct dirent *entry = readdir(handle); entry != NULL; entry = readdir(handle)) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char source[MOLTO_HOME_PATH_MAX];
        char target[MOLTO_HOME_PATH_MAX];
        if(fs_format_path(source, sizeof source, "%s/%s", from, entry->d_name) &&
           fs_format_path(target, sizeof target, "%s/%s", to, entry->d_name) &&
           !fs_path_exists(target) && move_entry(source, target))
            outcome->moved_plugins = true;
        else
            outcome->left_something = true;
    }
    closedir(handle);
    (void)rmdir(from);
}

static void move_plugins(const char *legacy, const char *data, migration_outcome *outcome) {
    move_plugin_dir(legacy, data, "bin", outcome);
    move_plugin_dir(legacy, data, "recipes", outcome);

    char plugins[MOLTO_HOME_PATH_MAX];
    if(fs_format_path(plugins, sizeof plugins, "%s/plugins", legacy))
        (void)rmdir(plugins);
}

/* The cache is not carried: it refills on demand, and the objects in it are
   keyed by compile commands naming paths inside the old directory, so a moved
   object cache would only ever miss. */
static void discard_cache(const char *legacy, migration_outcome *outcome) {
    char cache[MOLTO_HOME_PATH_MAX];
    if(!fs_format_path(cache, sizeof cache, "%s/%s", legacy, CACHE_DIRNAME) ||
       !fs_path_exists(cache))
        return;
    if(fs_remove_tree(cache))
        outcome->discarded_cache = true;
    else
        outcome->left_something = true;
}

bool paths_migrate_legacy_home(char *note, size_t note_size) {
    if(note == NULL || note_size == 0)
        return false;
    note[0] = '\0';

    /* The override is the directory in use, laid out the old way. */
    if(override_home() != NULL)
        return false;

    char legacy[MOLTO_HOME_PATH_MAX];
    char config[MOLTO_HOME_PATH_MAX];
    char data[MOLTO_HOME_PATH_MAX];
    if(!paths_molto_legacy_home(legacy, sizeof legacy) || !fs_is_dir_no_follow(legacy) ||
       !paths_molto_config_dir(config, sizeof config) || !paths_molto_data_dir(data, sizeof data))
        return false;

    migration_outcome outcome = {0};
    move_credentials(legacy, config, &outcome);
    move_plugins(legacy, data, &outcome);
    discard_cache(legacy, &outcome);

    if(!outcome.moved_credentials && !outcome.moved_plugins && !outcome.discarded_cache)
        return false;

    /* Empty only if everything moved: rmdir refuses anything else. */
    const bool removed = rmdir(legacy) == 0;

    const char *what = outcome.moved_credentials && outcome.moved_plugins
                           ? "the registry credential and installed plugins"
                       : outcome.moved_credentials ? "the registry credential"
                       : outcome.moved_plugins     ? "installed plugins"
                                                   : NULL;
    char where[2 * MOLTO_HOME_PATH_MAX + 8];
    if(!outcome.moved_plugins || strcmp(config, data) == 0)
        snprintf(where, sizeof where, "%s", outcome.moved_plugins ? data : config);
    else if(!outcome.moved_credentials)
        snprintf(where, sizeof where, "%s", data);
    else
        snprintf(where, sizeof where, "%s and %s", config, data);

    int used = what != NULL
                   ? snprintf(note, note_size, "moved %s from %s to %s\n", what, legacy, where)
                   : 0;
    if(used < 0 || (size_t)used >= note_size)
        return true;
    if(removed)
        (void)snprintf(note + used, note_size - (size_t)used, "removed %s%s", legacy,
                       outcome.discarded_cache
                           ? "; its download cache was discarded and refills on demand"
                           : "");
    else
        (void)snprintf(note + used, note_size - (size_t)used,
                       "left %s in place: it still holds files molto did not move", legacy);
    return true;
}
