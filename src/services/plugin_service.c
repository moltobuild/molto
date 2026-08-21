#include <molto/services/plugin_service.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>

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
