#include <molto/services/build_service.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/manifest_service.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Map a source path to its object path, mirroring the source tree under
   `root/build/<profile_dir>/obj`. */
static void object_path_for(const char *root, const char *profile_dir,
                            const char *source, char *out, size_t out_size) {
    size_t root_len = strlen(root);
    const char *relative = source;
    if (strncmp(source, root, root_len) == 0 && source[root_len] == '/')
        relative = source + root_len + 1;
    snprintf(out, out_size, "%s/build/%s/obj/%s.o", root, profile_dir, relative);
}

/* Create the parent directory chain for `path`. */
static bool make_parent_dirs(const char *path) {
    char directory[4096];
    snprintf(directory, sizeof directory, "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash == NULL)
        return true;
    *slash = '\0';
    return fs_make_dirs(directory);
}

/* Compile a single translation unit to `object`. */
static bool compile_one(const char *source, const char *object,
                        const manifest_profile *settings, const char *include_flag) {
    const char *compiler = source_is_cpp(source) ? "g++" : "gcc";
    char opt_flag[16];
    snprintf(opt_flag, sizeof opt_flag, "-O%d", settings->opt_level);
    const char *argv[10];
    size_t i = 0;
    argv[i++] = compiler;
    argv[i++] = "-c";
    argv[i++] = source;
    argv[i++] = "-o";
    argv[i++] = object;
    argv[i++] = opt_flag;
    if (settings->debug_info)
        argv[i++] = "-g";
    argv[i++] = include_flag;
    argv[i++] = NULL;
    return process_run(argv) == 0;
}

/* Link every object into the final executable. */
static bool link_all(bool any_cpp, const str_list *objects, const char *binary) {
    const char *linker = any_cpp ? "g++" : "gcc";
    size_t count = str_list_count(objects);
    const char **argv = malloc((count + 4) * sizeof(char *));
    if (argv == NULL)
        return false;
    size_t i = 0;
    argv[i++] = linker;
    for (size_t j = 0; j < count; j++)
        argv[i++] = str_list_get(objects, j);
    argv[i++] = "-o";
    argv[i++] = binary;
    argv[i++] = NULL;
    bool ok = process_run(argv) == 0;
    free(argv);
    return ok;
}

/* Read the package name and effective profile settings from the manifest. */
static int load_manifest(const char *root, build_profile profile,
                         char *name, size_t name_size, manifest_profile *settings) {
    char manifest_path[4096];
    snprintf(manifest_path, sizeof manifest_path, "%s/Project.toml", root);
    if (!fs_path_exists(manifest_path)) {
        fprintf(stderr, "molto: no Project.toml in '%s'\n", root);
        return exit_invalid_manifest;
    }
    char *toml = fs_read_file(manifest_path);
    if (toml == NULL) {
        fprintf(stderr, "molto: could not read '%s'\n", manifest_path);
        return exit_invalid_manifest;
    }
    bool have_name = manifest_read_name(toml, name, name_size);
    *settings = profile_defaults(profile);
    /* A missing profile section simply leaves the built-in defaults in place. */
    bool have_profile = manifest_read_profile(toml, profile_name(profile), settings);
    (void)have_profile;
    free(toml);
    if (!have_name) {
        fprintf(stderr, "molto: Project.toml is missing a package name\n");
        return exit_invalid_manifest;
    }
    return exit_ok;
}

/* Compile every source, accumulating object paths and whether C++ is present. */
static int compile_sources(const char *root, build_profile profile,
                           const manifest_profile *settings, const char *include_flag,
                           const str_list *sources, str_list *objects, bool *any_cpp) {
    for (size_t i = 0; i < str_list_count(sources); i++) {
        const char *source = str_list_get(sources, i);
        if (source_is_cpp(source))
            *any_cpp = true;
        char object[4096];
        object_path_for(root, profile_name(profile), source, object, sizeof object);
        if (!make_parent_dirs(object)) {
            fprintf(stderr, "molto: could not create output directory for '%s'\n", object);
            return exit_build_failure;
        }
        if (!str_list_push(objects, object))
            return exit_build_failure;
        if (fs_source_newer(source, object)
            && !compile_one(source, object, settings, include_flag)) {
            fprintf(stderr, "molto: failed to compile '%s'\n", source);
            return exit_build_failure;
        }
    }
    return exit_ok;
}

int build_project(const char *root, build_profile profile) {
    char name[128];
    manifest_profile settings;
    int result = load_manifest(root, profile, name, sizeof name, &settings);
    if (result != exit_ok)
        return result;

    char src_dir[4096];
    snprintf(src_dir, sizeof src_dir, "%s/src", root);
    if (!fs_is_dir(src_dir)) {
        fprintf(stderr, "molto: no src directory in '%s'\n", root);
        return exit_build_failure;
    }

    str_list sources;
    str_list_init(&sources);
    if (!source_discovery_collect(src_dir, &sources) || str_list_count(&sources) == 0) {
        fprintf(stderr, "molto: no source files found under '%s'\n", src_dir);
        str_list_free(&sources);
        return exit_build_failure;
    }

    char include_flag[sizeof src_dir + 2];
    snprintf(include_flag, sizeof include_flag, "-I%s", src_dir);

    str_list objects;
    str_list_init(&objects);
    bool any_cpp = false;
    result = compile_sources(root, profile, &settings, include_flag,
                             &sources, &objects, &any_cpp);

    if (result == exit_ok) {
        char binary[4096];
        snprintf(binary, sizeof binary, "%s/build/%s/%s", root, profile_name(profile), name);
        if (!link_all(any_cpp, &objects, binary)) {
            fprintf(stderr, "molto: failed to link '%s'\n", binary);
            result = exit_build_failure;
        }
    }

    str_list_free(&sources);
    str_list_free(&objects);
    return result;
}
