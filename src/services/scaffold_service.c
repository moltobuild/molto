#include <molto/services/scaffold_service.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/manifest_service.h>
#include <molto/services/source_service.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool make_subdir(const char *root, const char *sub) {
    char path[PATH_MAX];
    if(!fs_format_path(path, sizeof path, "%s/%s", root, sub))
        return false;
    return fs_make_dir(path);
}

static int write_manifest(const char *root, const char *name, project_kind kind,
                          const char *moltest_tag) {
    char path[PATH_MAX];
    if(!fs_format_path(path, sizeof path, "%s/Project.toml", root)) {
        fprintf(stderr, "molto: path too long to compose (%s)\n", root);
        return exit_build_failure;
    }
    if(fs_path_exists(path)) {
        fprintf(stderr, "molto: '%s' already exists\n", path);
        return exit_invalid_manifest;
    }
    char *content = manifest_render_default(name, kind, moltest_tag);
    if(content == NULL) {
        fprintf(stderr, "molto: failed to render manifest\n");
        return exit_build_failure;
    }
    bool ok = fs_write_file(path, content);
    free(content);
    if(!ok) {
        fprintf(stderr, "molto: failed to write '%s'\n", path);
        return exit_build_failure;
    }
    return exit_ok;
}

/* Starter program so `molto new` + `molto build`/`run` works out of the box. */
static const char main_template[] = "#include <stdio.h>\n"
                                    "\n"
                                    "int main(void) {\n"
                                    "    printf(\"Hello, world!\\n\");\n"
                                    "    return 0;\n"
                                    "}\n";

/* Room for one rendered starter file. */
#define STARTER_FILE_MAX 1024

/* Starter library: one function declared in include/, defined in src/, and
   checked in tests/ with moltest, so `molto new` + `molto test` works out of
   the box. The package name is snake_case and therefore already a C
   identifier; the header guard takes it upper-cased. */
static const char header_template[] = "#ifndef %s_H\n"
                                      "#define %s_H\n"
                                      "\n"
                                      "/* Return the sum of `a` and `b`. */\n"
                                      "int %s_add(int a, int b);\n"
                                      "\n"
                                      "#endif /* %s_H */\n";

static const char source_template[] = "#include <%s.h>\n"
                                      "\n"
                                      "int %s_add(int a, int b) {\n"
                                      "    return a + b;\n"
                                      "}\n";

static const char test_template[] = "#include <moltest.h>\n"
                                    "#include <%s.h>\n"
                                    "\n"
                                    "DESCRIBE(%s_add_sums_its_arguments) {\n"
                                    "    EXPECT_EQ(3, %s_add(1, 2));\n"
                                    "}\n";

/* The two directories Molto owns and writes into. Both are derived from the
   sources and safe to delete (RFC-0004), so neither belongs in version
   control — and `.bin/` in particular holds a binary file that changes on
   every build. */
static const char gitignore_template[] = "# Build output\n"
                                         "/build/\n"
                                         "\n"
                                         "# Workspace database (molto-owned metadata)\n"
                                         "/.bin/\n"
                                         "\n"
                                         "# What every build tells clangd and clang-tidy about\n"
                                         "# this project: derived from Project.toml and the tree\n"
                                         "/compile_commands.json\n";

/* Write one of the starter files, leaving an existing one untouched. */
static int write_starter_file(const char *root, const char *relative, const char *content) {
    char path[PATH_MAX];
    if(!fs_format_path(path, sizeof path, "%s/%s", root, relative)) {
        fprintf(stderr, "molto: path too long to compose (%s)\n", root);
        return exit_build_failure;
    }
    if(fs_path_exists(path))
        return exit_ok; /* never clobber what the user already has */
    if(!fs_write_file(path, content)) {
        fprintf(stderr, "molto: failed to write '%s'\n", path);
        return exit_build_failure;
    }
    return exit_ok;
}

/* Compose `relative` from `relative_format` and the name, then write the
   already-rendered `content` there unless the file exists. */
static int write_named_file(const char *root, const char *relative_format, const char *name,
                            const char *content, int rendered) {
    char relative[PATH_MAX];
    const int written = snprintf(relative, sizeof relative, relative_format, name);
    if(written < 0 || (size_t)written >= sizeof relative || rendered < 0 ||
       (size_t)rendered >= STARTER_FILE_MAX) {
        fprintf(stderr, "molto: name too long for the starter files (%s)\n", name);
        return exit_usage_error;
    }
    return write_starter_file(root, relative, content);
}

static int write_library_files(const char *root, const char *name) {
    char upper[STARTER_FILE_MAX];
    size_t i = 0;
    for(; name[i] != '\0' && i + 1 < sizeof upper; i++)
        upper[i] = (char)toupper((unsigned char)name[i]);
    upper[i] = '\0';

    char content[STARTER_FILE_MAX];
    int rendered = snprintf(content, sizeof content, header_template, upper, upper, name, upper);
    int result = write_named_file(root, "include/%s.h", name, content, rendered);
    if(result != exit_ok)
        return result;

    rendered = snprintf(content, sizeof content, source_template, name, name);
    result = write_named_file(root, "src/%s.c", name, content, rendered);
    if(result != exit_ok)
        return result;

    rendered = snprintf(content, sizeof content, test_template, name, name, name);
    return write_named_file(root, "tests/test_%s.c", name, content, rendered);
}

int scaffold_project(const char *root, const char *name, project_kind kind,
                     const char *moltest_tag) {
    if(!manifest_is_valid_name(name)) {
        fprintf(stderr, "molto: invalid package name '%s' (use snake_case)\n", name);
        return exit_usage_error;
    }
    if(strcmp(root, ".") != 0 && !fs_make_dir(root)) {
        fprintf(stderr, "molto: could not create directory '%s'\n", root);
        return exit_build_failure;
    }
    /* include/ is created because the generated manifest declares it: the two
       have to agree, or the first header lands somewhere -I never looks. */
    if(!make_subdir(root, "src") || !make_subdir(root, "tests") || !make_subdir(root, "include")) {
        fprintf(stderr, "molto: could not create project layout\n");
        return exit_build_failure;
    }
    int result = write_manifest(root, name, kind, moltest_tag);
    if(result != exit_ok)
        return result;
    result = kind == project_kind_library ? write_library_files(root, name)
                                          : write_starter_file(root, "src/main.c", main_template);
    if(result != exit_ok)
        return result;
    return write_starter_file(root, ".gitignore", gitignore_template);
}

void scaffold_newest_moltest_tag(char *out, size_t size) {
    char err[512] = "";
    if(source_git_newest_release(MANIFEST_MOLTEST_GIT, out, size, err, sizeof err))
        return;
    /* Offline is a fine place to start a project; a branch is not a fine
       thing to start it on. The newest release this molto knows of is still
       a release, and the manifest says which. */
    snprintf(out, size, "%s", MANIFEST_MOLTEST_KNOWN_TAG);
    fprintf(stderr, "molto: %s; moltest pinned to %s, the newest release this molto knows\n", err,
            MANIFEST_MOLTEST_KNOWN_TAG);
}
