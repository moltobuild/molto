#include <moltest.h>

#include "private_home.h"

#include <molto/services/fs_service.h>
#include <molto/services/paths_service.h>

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Where molto keeps its own things.
 *
 * One service answers this for the four places that used to read `HOME`
 * apiece, and the order it asks in is the whole contract: `$MOLTO_HOME` wins,
 * then the platform's own places — the XDG directories on Linux and macOS,
 * `%APPDATA%` on Windows. Getting the order wrong is not a crash; it is a
 * credential written somewhere the next command does not look, which is why it
 * is pinned here rather than left to the four callers.
 */

typedef struct {
    char home[PRIVATE_HOME_MAX];
    bool home_was_set;
    molto_home_override override;
#ifdef _WIN32
    char user_profile[PRIVATE_HOME_MAX];
    bool user_profile_was_set;
#endif
} environment;

static void remember(const char *name, char *out, size_t size, bool *was_set) {
    const char *value = getenv(name);
    *was_set = value != NULL;
    snprintf(out, size, "%s", value != NULL ? value : "");
}

static void put_back(const char *name, const char *value, bool was_set) {
    if(was_set)
        (void)setenv(name, value, 1);
    else
        (void)unsetenv(name);
}

static void save(environment *saved) {
    remember("HOME", saved->home, sizeof saved->home, &saved->home_was_set);
    molto_home_override_clear(&saved->override);
#ifdef _WIN32
    remember("USERPROFILE", saved->user_profile, sizeof saved->user_profile,
             &saved->user_profile_was_set);
#endif
}

static void restore(const environment *saved) {
    put_back("HOME", saved->home, saved->home_was_set);
    molto_home_override_restore(&saved->override);
#ifdef _WIN32
    put_back("USERPROFILE", saved->user_profile, saved->user_profile_was_set);
#endif
}

typedef bool (*directory_fn)(char *out, size_t size);

/* What `fn` answers, or "<none>" when it declines. */
static const char *answer(directory_fn fn, char *out, size_t size) {
    if(!fn(out, size))
        snprintf(out, size, "<none>");
    return out;
}

MOLTEST(the_molto_home_override_answers_whole) {
    environment saved;
    save(&saved);

    /* Someone who names a directory means that directory: nothing is appended,
       and the layout inside it is the one `~/.molto` always had. */
    ASSERT_EQ(0, setenv("HOME", "/somewhere", 1));
    ASSERT_EQ(0, setenv("MOLTO_HOME", "/elsewhere/molto", 1));
    ASSERT_EQ(0, setenv("XDG_CONFIG_HOME", "/xdg/config", 1));

    char out[MOLTO_HOME_PATH_MAX];
    EXPECT_STREQ("/elsewhere/molto", answer(paths_molto_config_dir, out, sizeof out));
    EXPECT_STREQ("/elsewhere/molto", answer(paths_molto_data_dir, out, sizeof out));
    EXPECT_STREQ("/elsewhere/molto/cache", answer(paths_molto_cache_dir, out, sizeof out));

    restore(&saved);
}

MOLTEST(a_subdirectory_hangs_off_the_data_directory) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("MOLTO_HOME", "/elsewhere/molto", 1));

    char path[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_data_subdir("plugins/bin", path, sizeof path));
    EXPECT_STREQ("/elsewhere/molto/plugins/bin", path);

    restore(&saved);
}

/* The migration's source, which does not move with the override: the override
   is where molto works now, and `~/.molto` is where an older one worked. */
MOLTEST(the_legacy_home_hangs_off_home) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/somewhere", 1));
    ASSERT_EQ(0, setenv("MOLTO_HOME", "/elsewhere/molto", 1));

    char home[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_legacy_home(home, sizeof home));
    EXPECT_STREQ("/somewhere/.molto", home);

    restore(&saved);
}

#ifdef _WIN32
/* One place on Windows, %APPDATA%\molto, whatever the XDG variables say: they
   are a Unix convention, and a Windows user who happens to export one did not
   mean molto's credential to follow it. */
MOLTEST(windows_keeps_everything_under_appdata) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("APPDATA", "C:/Users/somebody/AppData/Roaming", 1));
    ASSERT_EQ(0, setenv("XDG_CONFIG_HOME", "C:/xdg", 1));

    char out[MOLTO_HOME_PATH_MAX];
    EXPECT_STREQ("C:/Users/somebody/AppData/Roaming/molto",
                 answer(paths_molto_config_dir, out, sizeof out));
    EXPECT_STREQ("C:/Users/somebody/AppData/Roaming/molto",
                 answer(paths_molto_data_dir, out, sizeof out));
    EXPECT_STREQ("C:/Users/somebody/AppData/Roaming/molto/cache",
                 answer(paths_molto_cache_dir, out, sizeof out));

    restore(&saved);
}

MOLTEST(windows_without_appdata_has_no_answer) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/c/Users/somebody", 1));

    char out[MOLTO_HOME_PATH_MAX];
    EXPECT_FALSE(paths_molto_config_dir(out, sizeof out));
    EXPECT_FALSE(paths_molto_data_dir(out, sizeof out));
    EXPECT_FALSE(paths_molto_cache_dir(out, sizeof out));

    restore(&saved);
}

/*
 * The failure the paths service was extracted for: `molto login` on an
 * ordinary Windows machine said "HOME is not set, so there is nowhere to store
 * credentials". cmd and PowerShell set USERPROFILE and leave HOME alone; only
 * MSYS2 and git-bash define the other one. It still decides where the old
 * `.molto` is looked for.
 */
MOLTEST(windows_falls_back_to_the_user_profile) {
    environment saved;
    save(&saved);

    (void)unsetenv("HOME");
    ASSERT_EQ(0, setenv("USERPROFILE", "C:/Users/somebody", 1));

    char home[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_legacy_home(home, sizeof home));
    EXPECT_STREQ("C:/Users/somebody/.molto", home);

    restore(&saved);
}

/* What Windows actually hands over is spelled with backslashes, and every answer
   comes back in Molto's one separator: composed onto with '/', a backslash path
   becomes a directory a cached dependency's objects cannot be mirrored under
   (`obj/C/\\Users\\...`), which is how a git dependency failed to build. */
MOLTEST(windows_answers_in_one_separator) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("APPDATA", "C:\\Users\\somebody\\AppData\\Roaming", 1));
    (void)unsetenv("HOME");
    ASSERT_EQ(0, setenv("USERPROFILE", "C:\\Users\\somebody", 1));

    char out[MOLTO_HOME_PATH_MAX];
    EXPECT_STREQ("C:/Users/somebody/AppData/Roaming/molto",
                 answer(paths_molto_config_dir, out, sizeof out));
    EXPECT_STREQ("C:/Users/somebody/AppData/Roaming/molto/cache",
                 answer(paths_molto_cache_dir, out, sizeof out));
    EXPECT_STREQ("C:/Users/somebody/.molto", answer(paths_molto_legacy_home, out, sizeof out));

    restore(&saved);
}

/* HOME still wins where a shell went to the trouble of setting it: someone in
   MSYS2 means the home that shell gave them, and molto should agree with
   everything else run from there. */
MOLTEST(home_beats_the_user_profile_when_both_are_set) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/c/Users/somebody", 1));
    ASSERT_EQ(0, setenv("USERPROFILE", "D:/elsewhere", 1));

    char home[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_legacy_home(home, sizeof home));
    EXPECT_STREQ("/c/Users/somebody/.molto", home);

    restore(&saved);
}
#else
MOLTEST(the_directories_follow_xdg_defaults_under_home) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/somewhere", 1));

    char out[MOLTO_HOME_PATH_MAX];
    EXPECT_STREQ("/somewhere/.config/molto", answer(paths_molto_config_dir, out, sizeof out));
    EXPECT_STREQ("/somewhere/.local/share/molto", answer(paths_molto_data_dir, out, sizeof out));
    EXPECT_STREQ("/somewhere/.cache/molto", answer(paths_molto_cache_dir, out, sizeof out));

    restore(&saved);
}

MOLTEST(the_xdg_variables_move_each_directory) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/somewhere", 1));
    ASSERT_EQ(0, setenv("XDG_CONFIG_HOME", "/xdg/config", 1));
    ASSERT_EQ(0, setenv("XDG_DATA_HOME", "/xdg/data", 1));
    ASSERT_EQ(0, setenv("XDG_CACHE_HOME", "/xdg/cache", 1));

    char out[MOLTO_HOME_PATH_MAX];
    EXPECT_STREQ("/xdg/config/molto", answer(paths_molto_config_dir, out, sizeof out));
    EXPECT_STREQ("/xdg/data/molto", answer(paths_molto_data_dir, out, sizeof out));
    EXPECT_STREQ("/xdg/cache/molto", answer(paths_molto_cache_dir, out, sizeof out));

    restore(&saved);
}

/* The specification says to ignore a relative one, and an empty one is the
   same mistake: either would put a credential wherever the command ran. */
MOLTEST(an_empty_or_relative_xdg_variable_is_ignored) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/somewhere", 1));
    ASSERT_EQ(0, setenv("XDG_CONFIG_HOME", "", 1));
    ASSERT_EQ(0, setenv("XDG_DATA_HOME", "relative/data", 1));
    ASSERT_EQ(0, setenv("XDG_CACHE_HOME", "./cache", 1));

    char out[MOLTO_HOME_PATH_MAX];
    EXPECT_STREQ("/somewhere/.config/molto", answer(paths_molto_config_dir, out, sizeof out));
    EXPECT_STREQ("/somewhere/.local/share/molto", answer(paths_molto_data_dir, out, sizeof out));
    EXPECT_STREQ("/somewhere/.cache/molto", answer(paths_molto_cache_dir, out, sizeof out));

    restore(&saved);
}

/* An override set to nothing is not an override. Without this an exported but
   empty variable would answer the empty path, and every molto file would be
   written at the root of the current drive. */
MOLTEST(an_empty_override_is_no_override) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/somewhere", 1));
    ASSERT_EQ(0, setenv("MOLTO_HOME", "", 1));

    char out[MOLTO_HOME_PATH_MAX];
    EXPECT_STREQ("/somewhere/.config/molto", answer(paths_molto_config_dir, out, sizeof out));

    restore(&saved);
}

/* Nothing to fall back to, and saying so is the point: a Unix with no HOME has
   no home, and inventing one would put a credential somewhere nobody looks. */
MOLTEST(no_home_is_no_answer) {
    environment saved;
    save(&saved);

    (void)unsetenv("HOME");

    char out[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_FALSE(paths_molto_config_dir(out, sizeof out));
    EXPECT_FALSE(paths_molto_data_dir(out, sizeof out));
    EXPECT_FALSE(paths_molto_cache_dir(out, sizeof out));
    EXPECT_FALSE(paths_molto_legacy_home(out, sizeof out));

    restore(&saved);
}
#endif

/* --- moving what an older molto left in ~/.molto --- */

typedef struct {
    environment saved;
    char home[PRIVATE_HOME_MAX];
    char legacy[PRIVATE_HOME_MAX];
    char config[PRIVATE_HOME_MAX];
    char data[PRIVATE_HOME_MAX];
} migration;

static bool plant_file(const char *path, const char *content, int mode) {
    char dir[PRIVATE_HOME_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if(slash == NULL)
        return false;
    *slash = '\0';
    return fs_make_dirs(dir) && fs_write_file(path, content) && chmod(path, (mode_t)mode) == 0;
}

static bool in(const char *root, const char *relative, char *out, size_t size) {
    return fs_format_path(out, size, "%s/%s", root, relative);
}

/* A home holding what an older molto would have left in it. */
static bool migration_open(migration *at) {
    save(&at->saved);
    if(!moltest_temp_dir("molto_migrate", at->home, sizeof at->home) ||
       !private_home_point(at->home))
        return false;
    snprintf(at->legacy, sizeof at->legacy, "%s/.molto", at->home);
    snprintf(at->config, sizeof at->config, "%s" PRIVATE_HOME_CONFIG, at->home);
    snprintf(at->data, sizeof at->data, "%s" PRIVATE_HOME_DATA, at->home);

    char path[PRIVATE_HOME_MAX];
    return in(at->legacy, "credentials.toml", path, sizeof path) &&
           plant_file(path, "[registry]\ntoken = \"secret\"\n", 0600) &&
           in(at->legacy, "plugins/bin/molto-deb", path, sizeof path) &&
           plant_file(path, "#!/bin/sh\n", 0755) &&
           in(at->legacy, "plugins/recipes/deb.json", path, sizeof path) &&
           plant_file(path, "{}", 0644) &&
           in(at->legacy, "cache/sources/zlib/1.3.1/any/zlib.h", path, sizeof path) &&
           plant_file(path, "/* zlib */", 0644);
}

static void migration_close(migration *at) {
    (void)fs_remove_tree(at->home);
    restore(&at->saved);
}

static char *read_in(const char *root, const char *relative) {
    char path[PRIVATE_HOME_MAX];
    return in(root, relative, path, sizeof path) ? fs_read_file(path) : NULL;
}

static bool exists_in(const char *root, const char *relative) {
    char path[PRIVATE_HOME_MAX];
    return in(root, relative, path, sizeof path) && fs_path_exists(path);
}

MOLTEST(the_migration_moves_the_credential_and_the_plugins) {
    migration at;
    ASSERT_TRUE(migration_open(&at));

    char note[1024] = "";
    EXPECT_TRUE(paths_migrate_legacy_home(note, sizeof note));

    char *token = read_in(at.config, "credentials.toml");
    ASSERT_NOT_NULL(token);
    EXPECT_NOT_NULL(strstr(token, "secret"));
    free(token);
    EXPECT_TRUE(exists_in(at.data, "plugins/bin/molto-deb"));
    EXPECT_TRUE(exists_in(at.data, "plugins/recipes/deb.json"));

    /* Moved, not copied, and the cache discarded rather than carried: so the
       old directory is empty, and gone. */
    EXPECT_FALSE(fs_path_exists(at.legacy));
    EXPECT_NOT_NULL(strstr(note, at.legacy));

    migration_close(&at);
}

MOLTEST(the_migrated_credential_stays_readable_only_by_its_owner) {
    migration at;
    ASSERT_TRUE(migration_open(&at));

    char note[1024] = "";
    ASSERT_TRUE(paths_migrate_legacy_home(note, sizeof note));

    char path[PRIVATE_HOME_MAX];
    ASSERT_TRUE(in(at.config, "credentials.toml", path, sizeof path));
    struct stat info;
    ASSERT_EQ(0, stat(path, &info));
#ifndef _WIN32
    EXPECT_EQ(0, (int)(info.st_mode & (S_IRWXG | S_IRWXO)));
#endif

    migration_close(&at);
}

MOLTEST(the_migration_happens_once) {
    migration at;
    ASSERT_TRUE(migration_open(&at));

    char note[1024] = "";
    ASSERT_TRUE(paths_migrate_legacy_home(note, sizeof note));
    EXPECT_FALSE(paths_migrate_legacy_home(note, sizeof note));
    EXPECT_STREQ("", note);

    migration_close(&at);
}

/* `$MOLTO_HOME` is the directory in use, laid out the old way: nothing in
   `~/.molto` is molto's to move while it is set. */
MOLTEST(the_migration_leaves_alone_a_machine_with_an_override) {
    migration at;
    ASSERT_TRUE(migration_open(&at));
    ASSERT_EQ(0, setenv("MOLTO_HOME", at.legacy, 1));

    char note[1024] = "";
    EXPECT_FALSE(paths_migrate_legacy_home(note, sizeof note));
    EXPECT_TRUE(exists_in(at.legacy, "credentials.toml"));
    EXPECT_FALSE(exists_in(at.config, "credentials.toml"));

    migration_close(&at);
}

/* A newer credential is the one somebody logged in with since: the old one
   does not get to replace it, and `~/.molto` stays rather than losing it. */
MOLTEST(the_migration_never_overwrites_what_is_already_there) {
    migration at;
    ASSERT_TRUE(migration_open(&at));
    char path[PRIVATE_HOME_MAX];
    ASSERT_TRUE(in(at.config, "credentials.toml", path, sizeof path));
    ASSERT_TRUE(plant_file(path, "[registry]\ntoken = \"newer\"\n", 0600));

    char note[1024] = "";
    EXPECT_TRUE(paths_migrate_legacy_home(note, sizeof note));

    char *token = read_in(at.config, "credentials.toml");
    ASSERT_NOT_NULL(token);
    EXPECT_NOT_NULL(strstr(token, "newer"));
    free(token);
    EXPECT_TRUE(exists_in(at.legacy, "credentials.toml"));
    EXPECT_TRUE(exists_in(at.data, "plugins/bin/molto-deb"));

    migration_close(&at);
}

/* Plugins installed by the new molto before the old ones were moved: both
   sets end up in one directory. */
MOLTEST(the_migration_merges_plugins_into_an_existing_directory) {
    migration at;
    ASSERT_TRUE(migration_open(&at));
    char path[PRIVATE_HOME_MAX];
    ASSERT_TRUE(in(at.data, "plugins/bin/molto-rpm", path, sizeof path));
    ASSERT_TRUE(plant_file(path, "#!/bin/sh\n", 0755));

    char note[1024] = "";
    EXPECT_TRUE(paths_migrate_legacy_home(note, sizeof note));
    EXPECT_TRUE(exists_in(at.data, "plugins/bin/molto-deb"));
    EXPECT_TRUE(exists_in(at.data, "plugins/bin/molto-rpm"));
    EXPECT_FALSE(fs_path_exists(at.legacy));

    migration_close(&at);
}

/* Something molto did not put there is not molto's to delete. */
MOLTEST(the_migration_keeps_the_old_directory_while_it_holds_anything_else) {
    migration at;
    ASSERT_TRUE(migration_open(&at));
    char path[PRIVATE_HOME_MAX];
    ASSERT_TRUE(in(at.legacy, "notes.txt", path, sizeof path));
    ASSERT_TRUE(plant_file(path, "mine", 0644));

    char note[1024] = "";
    EXPECT_TRUE(paths_migrate_legacy_home(note, sizeof note));
    EXPECT_TRUE(exists_in(at.legacy, "notes.txt"));
    EXPECT_TRUE(exists_in(at.config, "credentials.toml"));

    migration_close(&at);
}

MOLTEST(the_migration_has_nothing_to_do_without_an_old_directory) {
    environment saved;
    save(&saved);
    char home[PRIVATE_HOME_MAX];
    ASSERT_TRUE(moltest_temp_dir("molto_migrate_none", home, sizeof home));
    ASSERT_TRUE(private_home_point(home));

    char note[1024] = "";
    EXPECT_FALSE(paths_migrate_legacy_home(note, sizeof note));
    EXPECT_STREQ("", note);

    (void)fs_remove_tree(home);
    restore(&saved);
}
