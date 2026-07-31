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
    if (!fs_format_path(path, sizeof path, "%s/%s", root, sub))
        return false;
    return fs_make_dir(path);
}

static int write_manifest(const char *root, const char *name) {
    char path[PATH_MAX];
    if (!fs_format_path(path, sizeof path, "%s/Project.toml", root)) {
        fprintf(stderr, "molto: path too long to compose (%s)\n", root);
        return exit_build_failure;
    }
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

/* Starter program so `molto new` + `molto build`/`run` works out of the box. */
static const char main_template[] =
    "#include <stdio.h>\n"
    "\n"
    "int main(void) {\n"
    "    printf(\"Hello, world!\\n\");\n"
    "    return 0;\n"
    "}\n";

/* The two directories Molto owns and writes into. Both are derived from the
   sources and safe to delete (RFC-0004), so neither belongs in version
   control — and `.bin/` in particular holds a binary file that changes on
   every build. */
static const char gitignore_template[] =
    "# Build output\n"
    "/build/\n"
    "\n"
    "# Workspace database (molto-owned metadata)\n"
    "/.bin/\n";

/* Write one of the starter files, leaving an existing one untouched. */
static int write_starter_file(const char *root, const char *relative,
                              const char *content) {
    char path[PATH_MAX];
    if (!fs_format_path(path, sizeof path, "%s/%s", root, relative)) {
        fprintf(stderr, "molto: path too long to compose (%s)\n", root);
        return exit_build_failure;
    }
    if (fs_path_exists(path))
        return exit_ok; /* never clobber what the user already has */
    if (!fs_write_file(path, content)) {
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
    int result = write_manifest(root, name);
    if (result != exit_ok)
        return result;
    result = write_starter_file(root, "src/main.c", main_template);
    if (result != exit_ok)
        return result;
    return write_starter_file(root, ".gitignore", gitignore_template);
}
