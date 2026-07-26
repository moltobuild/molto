#include <molto/services/scaffold_service.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/manifest_service.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool make_subdir(const char *root, const char *sub) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", root, sub);
    return fs_make_dir(path);
}

static int write_manifest(const char *root, const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/Project.toml", root);
    if (fs_path_exists(path)) {
        fprintf(stderr, "molto: '%s' already exists\n", path);
        return exit_invalid_manifest;
    }
    char *content = manifest_render_default(name);
    if (content == NULL) {
        fprintf(stderr, "molto: failed to render manifest\n");
        return exit_build_failure;
    }
    bool ok = fs_write_file(path, content);
    free(content);
    if (!ok) {
        fprintf(stderr, "molto: failed to write '%s'\n", path);
        return exit_build_failure;
    }
    return exit_ok;
}

int scaffold_project(const char *root, const char *name) {
    if (!manifest_is_valid_name(name)) {
        fprintf(stderr, "molto: invalid package name '%s' (use snake_case)\n", name);
        return exit_usage_error;
    }
    if (strcmp(root, ".") != 0 && !fs_make_dir(root)) {
        fprintf(stderr, "molto: could not create directory '%s'\n", root);
        return exit_build_failure;
    }
    if (!make_subdir(root, "src") || !make_subdir(root, "tests")) {
        fprintf(stderr, "molto: could not create project layout\n");
        return exit_build_failure;
    }
    return write_manifest(root, name);
}
