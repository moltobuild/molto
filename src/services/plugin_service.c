#include <molto/services/plugin_service.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/resolve_service.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/doc.h>
#include <molto/util/json.h>
#include <molto/util/semver.h>
#include <molto/util/toml.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The prefix every plugin executable carries, so that a plugin cannot be
   mistaken for an unrelated program with the same short name. */
#define PLUGIN_PREFIX "molto-"

/* `molto-<name>` is composed into a buffer this wide. The filename it is
   stored in may be wider by whatever the platform appends, which is why
   `in_directory` sizes its own. */
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

/* Whether `dir` holds an executable `program`, writing its path into `out`.

   `program` is a name and what the directory holds is a filename, and on
   Windows they differ: a plugin called `meson` is `molto-meson.exe` there, and
   looking for `molto-meson` finds nothing at all. `fs_executable_file` is the
   conversion, and it is also the permission — `access(X_OK)` succeeds for any
   file that exists on Windows, because there is no execute bit for it to read,
   so requiring the suffix is what filtering there consists of. */
static bool in_directory(const char *dir, const char *program, char *out, size_t size) {
    if(dir[0] == '\0')
        return false;
    char file[PLUGIN_PROGRAM_MAX + sizeof ".exe"];
    if(!fs_executable_file(program, file, sizeof file))
        return false;
    if(!fs_format_path(out, size, "%s/%s", dir, file))
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

/* The recipe directory itself, which the install has to create. */
static bool recipe_dir(char *out, size_t size) {
    const char *home = getenv("HOME");
    if(home == NULL || home[0] == '\0')
        return false;
    return fs_format_path(out, size, "%s/.molto/plugins/recipes", home);
}

/* One of the two encodings a recipe may be stored in. */
static bool recipe_path_with(const char *name, const char *extension, char *out, size_t size) {
    if(!plugin_name_valid(name))
        return false;

    char dir[PLUGIN_PATH_MAX];
    if(!recipe_dir(dir, sizeof dir))
        return false;
    return fs_format_path(out, size, "%s/%s.%s", dir, name, extension);
}

bool plugin_recipe_path(const char *name, char *out, size_t size) {
    /* The hand-written one when it is there, so a developer can override what
       an install put down without uninstalling it. */
    if(recipe_path_with(name, "toml", out, size) && fs_path_exists(out))
        return true;
    return recipe_path_with(name, "json", out, size);
}

static bool ends_with(const char *text, const char *suffix) {
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length && strcmp(text + text_length - suffix_length, suffix) == 0;
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

    recipe_coordinate discarded_coordinate;
    recipe_plugin discarded_plugin;
    recipe_coordinate *into_coordinate = coordinate != NULL ? coordinate : &discarded_coordinate;
    recipe_plugin *into_plugin = plugin != NULL ? plugin : &discarded_plugin;
    bool ok = false;

    if(ends_with(path, ".json")) {
        /* The registry's answer for one artifact. The recipe is its `metadata`
           member: the artifact wraps it with the checksum and the size the
           registry measured, which are facts about the blob rather than part of
           what the publisher wrote. */
        json_document *doc = json_parse(text);
        if(doc == NULL) {
            free(text);
            return recipe_error(err, err_size, "the stored answer for '%s' is not JSON", name);
        }
        const doc_view view = doc_from_json(json_get(json_root(doc), "metadata"));
        ok = recipe_read_coordinate(view, into_coordinate, err, err_size) &&
             recipe_read_plugin(view, into_plugin, err, err_size);
        json_free(doc);
    } else {
        char parse_err[256] = "";
        toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
        if(doc == NULL) {
            free(text);
            return recipe_error(err, err_size, "the recipe is not valid TOML: %s", parse_err);
        }
        const doc_view view = doc_from_toml(doc);
        ok = recipe_read_coordinate(view, into_coordinate, err, err_size) &&
             recipe_read_plugin(view, into_plugin, err, err_size);
        toml_free(doc);
    }

    free(text);
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
    /* The name, not the filename: `molto-meson.exe` is a plugin called `meson`
       and not one called `meson.exe`, and everything downstream — the listing,
       the recipe lookup, the subcommand — reads the name. Refusing a file that
       carries no suffix is the same filter `in_directory` applies, said for a
       directory entry instead of a composed path. */
    if(!fs_executable_name(entry + prefix, out, size))
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

        /* Back through the name rather than reusing the entry: what readdir
           hands over is a filename and `in_directory` takes a name, and on
           Windows the two differ by the suffix `name_from_entry` just took
           off. Handing the entry straight back would ask for
           `molto-meson.exe.exe`. The conversion is inverse in both directions,
           which is what makes the round trip safe. */
        char program[PLUGIN_PROGRAM_MAX];
        if(!plugin_program(name, program, sizeof program))
            continue;

        char path[PLUGIN_PATH_MAX];
        if(!in_directory(dir, program, path, sizeof path))
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

/* --- installing one --- */

/* The registry path of a tool's release, and of one artifact of it. */
#define TOOL_RELEASE_PATH "/v1/tools/%s/%s"
#define TOOL_ARTIFACT_PATH "/v1/tools/%s/%s/%s"
#define TOOL_VERSIONS_PATH "/v1/tools/%s"

static bool ask(const char *base_url, const char *path, registry_response *out, char *err,
                size_t err_size) {
    if(!registry_get(base_url, path, out, err, err_size))
        return false;
    if(out->status != 200) {
        registry_explain(out, err, err_size);
        return false;
    }
    return true;
}

/* The newest version published under `name`, when the caller named none. */
static bool newest_version(const char *base_url, const char *name, char *out, size_t size,
                           char *err, size_t err_size) {
    char path[PLUGIN_PATH_MAX];
    if(!fs_format_path(path, sizeof path, TOOL_VERSIONS_PATH, name))
        return recipe_error(err, err_size, "the path for '%s' is too long", name);

    registry_response response;
    if(!ask(base_url, path, &response, err, err_size))
        return false;

    str_list versions;
    str_list_init(&versions);
    bool ok = resolve_read_versions(response.body, &versions, err, err_size);
    if(ok) {
        /* Ordered by precedence rather than by publication, so "the newest" is
           the same answer whatever order the catalogue happened to return. */
        const size_t ordered = semver_sort_desc(&versions);
        ok = ordered > 0 && fs_format_path(out, size, "%s", str_list_get(&versions, 0));
        if(!ok)
            (void)recipe_error(err, err_size, "'%s' has no release molto can order", name);
    }
    str_list_free(&versions);
    return ok;
}

bool plugin_prepare(const char *base_url, const char *name, const char *version,
                    plugin_candidate *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);

    if(!plugin_name_valid(name))
        return recipe_error(err, err_size, "'%s' is not a plugin name", name);

    char target[PLUGIN_NAME_MAX];
    if(!toolchain_host_target(target, sizeof target))
        return recipe_error(err, err_size,
                            "could not ask pickup what this machine is (needs pickup 0.6.0)%s", "");

    char resolved[RECIPE_COORDINATE_MAX];
    if(version != NULL) {
        if(!fs_format_path(resolved, sizeof resolved, "%s", version))
            return recipe_error(err, err_size, "the version of '%s' is too long", name);
    } else if(!newest_version(base_url, name, resolved, sizeof resolved, err, err_size)) {
        return false;
    }

    /* One artifact, not the whole release: the target is already decided, and
       asking for it by name is what makes "published, but not for you" a
       distinct answer from "no such plugin". */
    char path[PLUGIN_PATH_MAX];
    if(!fs_format_path(path, sizeof path, TOOL_ARTIFACT_PATH, name, resolved, target))
        return recipe_error(err, err_size, "the path for '%s' is too long", name);

    registry_response response;
    if(!ask(base_url, path, &response, err, err_size))
        return false;

    json_document *doc = json_parse(response.body);
    if(doc == NULL)
        return recipe_error(err, err_size, "the registry's answer about '%s' was not JSON", name);

    const json_value root = json_root(doc);
    const doc_view view = doc_from_json(json_get(root, "metadata"));
    const char *url = json_string(json_get(root, "download_url"));
    const char *checksum = json_string(json_get(root, "checksum"));

    bool ok = recipe_read_coordinate(view, &out->coordinate, err, err_size) &&
              recipe_read_plugin(view, &out->plugin, err, err_size);
    if(ok && (url == NULL || checksum == NULL))
        ok = recipe_error(err, err_size, "the registry offered '%s' with no bytes to download",
                          name);
    if(ok)
        ok = fs_format_path(out->download_url, sizeof out->download_url, "%s", url) &&
             fs_format_path(out->checksum, sizeof out->checksum, "%s", checksum) &&
             fs_format_path(out->body, sizeof out->body, "%s", response.body);
    json_free(doc);
    return ok;
}

/* Copy the binary out of the unpacked tree and into ~/.molto/plugins/bin. */
static bool place_binary(const char *root, const char *name, char *err, size_t err_size) {
    char dir[PLUGIN_PATH_MAX];
    if(!plugin_dir(dir, sizeof dir) || !fs_make_dirs(dir))
        return recipe_error(err, err_size, "could not create the plugin directory for '%s'", name);

    /* The archive carries `bin/molto-<name>`, the same shape every other
       artifact in this ecosystem is packed with. */
    char source[PLUGIN_PATH_MAX];
    char destination[PLUGIN_PATH_MAX];
    if(!fs_format_path(source, sizeof source, "%s/bin/" PLUGIN_PREFIX "%s", root, name) ||
       !fs_format_path(destination, sizeof destination, "%s/" PLUGIN_PREFIX "%s", dir, name))
        return recipe_error(err, err_size, "the path to '%s' is too long", name);

    if(!fs_path_exists(source))
        return recipe_error(err, err_size, "the archive holds no bin/" PLUGIN_PREFIX "%s", name);

    const char *argv[] = {"cp", "-f", source, destination, NULL};
    if(process_run(argv) != 0)
        return recipe_error(err, err_size, "could not install the binary of '%s'", name);

    return chmod(destination, 0755) == 0 ||
           recipe_error(err, err_size, "could not make '%s' executable", name);
}

/* Keep the registry's answer, so what it said stays readable offline. */
static bool place_recipe(const plugin_candidate *candidate, char *err, size_t err_size) {
    char dir[PLUGIN_PATH_MAX];
    char path[PLUGIN_PATH_MAX];
    if(!recipe_dir(dir, sizeof dir) || !fs_make_dirs(dir) ||
       !recipe_path_with(candidate->coordinate.name, "json", path, sizeof path))
        return recipe_error(err, err_size, "could not write the recipe of '%s'",
                            candidate->coordinate.name);

    return fs_write_file(path, candidate->body) ||
           recipe_error(err, err_size, "could not write the recipe of '%s'",
                        candidate->coordinate.name);
}

bool plugin_install(const plugin_candidate *candidate, char *err, size_t err_size) {
    const char *name = candidate->coordinate.name;

    /* The fetch is the one that already downloads, verifies the digest and
       unpacks atomically. Reimplementing it here would be a second integrity
       story, and the wrong number of them is two. */
    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_archive;
    spec.compression = source_compression_tar_zst;
    if(!fs_format_path(spec.location, sizeof spec.location, "%s", candidate->download_url) ||
       !fs_format_path(spec.sha256, sizeof spec.sha256, "%s", candidate->checksum))
        return recipe_error(err, err_size, "the download of '%s' does not fit", name);

    char root[PLUGIN_PATH_MAX];
    if(!source_fetch(&spec, name, candidate->coordinate.version, candidate->coordinate.target, root,
                     sizeof root, err, err_size))
        return false;

    return place_binary(root, name, err, err_size) && place_recipe(candidate, err, err_size);
}

/* --- removing one --- */

bool plugin_remove(const char *name, char *err, size_t err_size) {
    if(!plugin_name_valid(name))
        return recipe_error(err, err_size, "'%s' is not a plugin name", name);

    char dir[PLUGIN_PATH_MAX];
    char binary[PLUGIN_PATH_MAX];
    if(!plugin_dir(dir, sizeof dir) ||
       !fs_format_path(binary, sizeof binary, "%s/" PLUGIN_PREFIX "%s", dir, name))
        return recipe_error(err, err_size, "the path to '%s' is too long", name);

    /* A plugin on PATH was put there by someone else and is not molto's to
       delete. Saying so is more useful than removing nothing and reporting
       success. */
    if(!fs_path_exists(binary))
        return recipe_error(err, err_size, "'%s' was not installed by molto", name);

    if(remove(binary) != 0)
        return recipe_error(err, err_size, "could not remove the binary of '%s'", name);

    /* Both encodings, because either could be there and leaving one behind
       would make `molto plugin list` report a plugin that is gone. */
    char recipe[PLUGIN_PATH_MAX];
    for(const char *extension = "json";; extension = "toml") {
        if(recipe_path_with(name, extension, recipe, sizeof recipe) && fs_path_exists(recipe))
            (void)remove(recipe);
        if(extension[0] == 't')
            break;
    }
    return true;
}
