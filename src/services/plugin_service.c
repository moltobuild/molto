#include <molto/services/plugin_service.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/util/doc.h>
#include <molto/util/toml.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The prefix every plugin executable carries, so that a plugin cannot be
   mistaken for an unrelated program with the same short name. */
#define PLUGIN_PREFIX "molto-"

/* `molto-<name>` is composed into a buffer this wide. */
#define PLUGIN_PROGRAM_MAX (sizeof PLUGIN_PREFIX + PLUGIN_NAME_MAX)

/* --- naming --- */

static bool name_char_valid(char c, bool first) {
    if(c >= 'a' && c <= 'z')
        return true;
    if(c >= '0' && c <= '9')
        return true;
    return !first && (c == '-' || c == '_');
}

bool plugin_name_valid(const char *name) {
    if(name == NULL || name[0] == '\0')
        return false;
    size_t length = strlen(name);
    if(length >= PLUGIN_NAME_MAX)
        return false;
    for(size_t i = 0; i < length; i++) {
        if(!name_char_valid(name[i], i == 0))
            return false;
    }
    return true;
}

/* Compose the executable name a plugin is looked for under. */
static bool plugin_program(const char *name, char *out, size_t size) {
    return fs_format_path(out, size, PLUGIN_PREFIX "%s", name);
}

/* --- where to look --- */

bool plugin_dir(char *out, size_t size) {
    const char *home = getenv("HOME");
    if(home == NULL || home[0] == '\0')
        return false;
    return fs_format_path(out, size, "%s/.molto/plugins/bin", home);
}

static bool is_executable(const char *path) { return access(path, X_OK) == 0; }

/* Whether `dir` holds an executable `program`, writing its path into `out`. */
static bool in_directory(const char *dir, const char *program, char *out, size_t size) {
    if(dir[0] == '\0')
        return false;
    if(!fs_format_path(out, size, "%s/%s", dir, program))
        return false;
    return is_executable(out);
}

/* Walk PATH looking for `program`.

   An empty entry is skipped rather than read as the current directory, which is
   what POSIX says it means. Molto would be running a binary from whatever
   directory the user happens to be standing in, chosen by nobody, and a plugin
   is a program this tool executes on their behalf. */
static bool on_path(const char *program, char *out, size_t size) {
    const char *path = getenv("PATH");
    if(path == NULL || path[0] == '\0')
        return false;

    for(const char *entry = path; entry != NULL;) {
        const char *separator = strchr(entry, ':');
        size_t length = separator != NULL ? (size_t)(separator - entry) : strlen(entry);

        char dir[PLUGIN_PATH_MAX];
        if(length > 0 && length < sizeof dir) {
            memcpy(dir, entry, length);
            dir[length] = '\0';
            if(in_directory(dir, program, out, size))
                return true;
        }
        entry = separator != NULL ? separator + 1 : NULL;
    }
    return false;
}

bool plugin_resolve(const char *name, char *out, size_t size) {
    if(!plugin_name_valid(name))
        return false;

    char program[PLUGIN_PROGRAM_MAX];
    if(!plugin_program(name, program, sizeof program))
        return false;

    char installed[PLUGIN_PATH_MAX];
    if(plugin_dir(installed, sizeof installed) && in_directory(installed, program, out, size))
        return true;

    return on_path(program, out, size);
}

/* --- running one --- */

/* Build the NULL-terminated vector handed to exec: the resolved path, then the
   arguments as they were typed. Caller frees. */
static const char **compose_argv(const char *path, int argc, char **argv) {
    const char **out = (const char **)calloc((size_t)argc + 2, sizeof *out);
    if(out == NULL)
        return NULL;
    out[0] = path;
    for(int i = 0; i < argc; i++)
        out[i + 1] = argv[i];
    return out;
}

int plugin_run(const char *path, int argc, char **argv) {
    const char **child = compose_argv(path, argc, argv);
    if(child == NULL)
        return exit_plugin_failure;

    int code = process_run(child);
    free((void *)child);

    /* -1 is a fork or wait that failed and 127 is a file that could not be
       executed after all — the plugin never ran, so what came back is not an
       answer to report as one. Everything else is the plugin speaking. */
    if(code < 0 || code == 127)
        return exit_plugin_failure;
    return code;
}

/* --- the recipe kept beside a plugin --- */

bool plugin_recipe_path(const char *name, char *out, size_t size) {
    if(!plugin_name_valid(name))
        return false;

    const char *home = getenv("HOME");
    if(home == NULL || home[0] == '\0')
        return false;
    return fs_format_path(out, size, "%s/.molto/plugins/recipes/%s.toml", home, name);
}

static bool recipe_error(char *err, size_t err_size, const char *format, const char *detail) {
    if(err != NULL && err_size > 0)
        snprintf(err, err_size, format, detail);
    return false;
}

bool plugin_read_recipe(const char *name, recipe_coordinate *coordinate, recipe_plugin *plugin,
                        char *err, size_t err_size) {
    char path[PLUGIN_PATH_MAX];
    if(!plugin_recipe_path(name, path, sizeof path))
        return recipe_error(err, err_size, "no recipe path for '%s'", name);

    char *text = fs_read_file(path);
    if(text == NULL)
        return recipe_error(err, err_size, "no recipe beside '%s'", name);

    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    free(text);
    if(doc == NULL)
        return recipe_error(err, err_size, "the recipe is not valid TOML: %s", parse_err);

    const doc_view view = doc_from_toml(doc);
    recipe_coordinate discarded_coordinate;
    recipe_plugin discarded_plugin;
    const bool ok =
        recipe_read_coordinate(view, coordinate != NULL ? coordinate : &discarded_coordinate, err,
                               err_size) &&
        recipe_read_plugin(view, plugin != NULL ? plugin : &discarded_plugin, err, err_size);
    toml_free(doc);
    return ok;
}

/* --- listing what this machine offers --- */

/* The name behind `molto-<name>`, or false when the entry is not one of ours.
   The name is validated too, so a file somebody dropped in the directory
   cannot be listed as a plugin Molto would agree to run. */
static bool name_from_entry(const char *entry, char *out, size_t size) {
    const size_t prefix = sizeof PLUGIN_PREFIX - 1;
    if(strncmp(entry, PLUGIN_PREFIX, prefix) != 0)
        return false;
    if(!fs_format_path(out, size, "%s", entry + prefix))
        return false;
    return plugin_name_valid(out);
}

static bool already_listed(const plugin_entry *list, size_t count, const char *name) {
    for(size_t i = 0; i < count; i++) {
        if(strcmp(list[i].name, name) == 0)
            return true;
    }
    return false;
}

/* Record one plugin, unless a higher-precedence origin already claimed the
   name. False only when the listing is full. */
static bool add_entry(plugin_entry *out, size_t capacity, size_t *count, const char *dir,
                      const char *name, plugin_origin origin) {
    if(already_listed(out, *count, name))
        return true;
    if(*count == capacity)
        return false;

    plugin_entry *entry = &out[*count];
    memset(entry, 0, sizeof *entry);
    snprintf(entry->name, sizeof entry->name, "%s", name);
    if(!fs_format_path(entry->path, sizeof entry->path, "%s/" PLUGIN_PREFIX "%s", dir, name))
        return true; /* a path that does not fit is a plugin nothing could run */

    entry->origin = origin;

    char recipe[PLUGIN_PATH_MAX];
    entry->has_recipe = plugin_recipe_path(name, recipe, sizeof recipe) && fs_path_exists(recipe);

    (*count)++;
    return true;
}

static int compare_entries(const void *left, const void *right) {
    const plugin_entry *a = left;
    const plugin_entry *b = right;
    return strcmp(a->name, b->name);
}

/* Collect every executable `molto-*` in one directory. Returns false only when
   the listing filled up; a directory that does not exist is not an error, since
   neither the install directory nor every PATH entry has to. */
static bool scan_directory(const char *dir, plugin_origin origin, plugin_entry *out,
                           size_t capacity, size_t *count) {
    DIR *handle = opendir(dir);
    if(handle == NULL)
        return true;

    const size_t before = *count;
    bool room = true;
    for(const struct dirent *entry = readdir(handle); entry != NULL && room;
        entry = readdir(handle)) {
        char name[PLUGIN_NAME_MAX];
        if(!name_from_entry(entry->d_name, name, sizeof name))
            continue;

        char path[PLUGIN_PATH_MAX];
        if(!in_directory(dir, entry->d_name, path, sizeof path))
            continue; /* there, and not executable */

        room = add_entry(out, capacity, count, dir, name, origin);
    }
    closedir(handle);

    /* Sorted within the directory, because readdir's order is the filesystem's
       and two runs of `molto plugin list` should agree. */
    if(*count > before)
        qsort(out + before, *count - before, sizeof *out, compare_entries);
    return room;
}

bool plugin_list(plugin_entry *out, size_t capacity, size_t *count) {
    *count = 0;

    char installed[PLUGIN_PATH_MAX];
    if(plugin_dir(installed, sizeof installed) &&
       !scan_directory(installed, plugin_origin_installed, out, capacity, count))
        return false;

    const char *path = getenv("PATH");
    if(path == NULL || path[0] == '\0')
        return true;

    for(const char *entry = path; entry != NULL;) {
        const char *separator = strchr(entry, ':');
        const size_t length = separator != NULL ? (size_t)(separator - entry) : strlen(entry);

        char dir[PLUGIN_PATH_MAX];
        if(length > 0 && length < sizeof dir) {
            memcpy(dir, entry, length);
            dir[length] = '\0';
            if(!scan_directory(dir, plugin_origin_path, out, capacity, count))
                return false;
        }
        entry = separator != NULL ? separator + 1 : NULL;
    }
    return true;
}
