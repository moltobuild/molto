#include <moltest.h>

#include <molto/services/configure_service.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Running a dependency's configure, against a fake one.
 *
 * The fake does what a real one does to the tree — writes a header — and
 * counts its own runs, so the stamp's job (running once per compiler) can be
 * measured. It needs `sh` and `make`, which is what a real configure needs. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];
} sandbox;

static bool make_source(sandbox *at) {
    if (!moltest_temp_dir("molto_configure", at->root, sizeof at->root))
        return false;
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/configure", at->root);
    if (!fs_write_file(file, "echo run >> runs.txt\n"
                             "echo \"#define CONFIGURED_FOR \\\"$CC $*\\\"\" > config.h\n"
                             "echo \"#define TOOL \\\"$TOOL\\\"\" >> config.h\n"))
        return false;
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof dir, "%s/src/port", at->root);
    if (!fs_make_dirs(dir))
        return false;
    snprintf(file, sizeof file, "%s/src/port/Makefile", at->root);
    return fs_write_file(file, "paths.h:\n\techo '#define PATHS 1' > paths.h\n");
}

static int runs(const sandbox *at) {
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/runs.txt", at->root);
    char *text = fs_read_file(file);
    int count = 0;
    for (const char *c = text; c != NULL && *c != '\0'; c++)
        count += *c == '\n';
    free(text);
    return count;
}

static recipe_build delegated(void) {
    recipe_build build = {0};
    build.system = recipe_build_autotools;
    build.via = recipe_via_delegate;
    snprintf(build.args[build.arg_count++], RECIPE_BUILD_ARG_MAX, "--without-icu");
    snprintf(build.env[build.env_count++], RECIPE_BUILD_ARG_MAX, "TOOL=true");
    snprintf(build.targets[build.target_count++], RECIPE_BUILD_ARG_MAX, "src/port/paths.h");
    return build;
}

DESCRIBE(configure_writes_what_upstream_configure_writes) {
    sandbox at;
    ASSERT_TRUE(make_source(&at));
    const recipe_build build = delegated();
    char err[512] = "";
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "cc", NULL, err, sizeof err));

    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/config.h", at.root);
    char *text = fs_read_file(file);
    ASSERT_NOT_NULL(text);
    /* CC is the compiler molto resolved, the arguments are the recipe's, and
       so is the environment. */
    EXPECT_NOT_NULL(strstr(text, "cc --without-icu"));
    EXPECT_NOT_NULL(strstr(text, "#define TOOL \"true\""));
    free(text);
    snprintf(file, sizeof file, "%s/src/port/paths.h", at.root);
    EXPECT_TRUE(fs_path_exists(file));
    (void)fs_remove_tree(at.root);
}

DESCRIBE(configure_runs_once_per_compiler) {
    sandbox at;
    ASSERT_TRUE(make_source(&at));
    const recipe_build build = delegated();
    char err[512] = "";
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "cc", NULL, err, sizeof err));
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "cc", NULL, err, sizeof err));
    EXPECT_EQ(1, runs(&at));

    /* Another compiler is another machine as far as configure is concerned. */
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "gcc", NULL, err, sizeof err));
    EXPECT_EQ(2, runs(&at));
    (void)fs_remove_tree(at.root);
}

DESCRIBE(a_failing_configure_is_reported_with_its_log) {
    sandbox at;
    ASSERT_TRUE(make_source(&at));
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/configure", at.root);
    ASSERT_TRUE(fs_write_file(file, "echo 'configure: error: no compiler' >&2\nexit 1\n"));
    const recipe_build build = delegated();
    char err[512] = "";
    EXPECT_FALSE(configure_dependency("fake", at.root, &build, "cc", NULL, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "fake"));
    EXPECT_NOT_NULL(strstr(err, "config.log"));
    (void)fs_remove_tree(at.root);
}

DESCRIBE(the_fingerprint_changes_with_what_shapes_the_answer) {
    recipe_build build = delegated();
    char first[65];
    char second[65];
    configure_fingerprint(&build, "cc", NULL, first);
    configure_fingerprint(&build, "cc", NULL, second);
    EXPECT_STREQ(first, second);
    snprintf(build.args[0], RECIPE_BUILD_ARG_MAX, "--with-icu");
    configure_fingerprint(&build, "cc", NULL, second);
    EXPECT_TRUE(strcmp(first, second) != 0);
    configure_fingerprint(&build, "cc", "x86_64-w64-mingw32", first);
    EXPECT_TRUE(strcmp(first, second) != 0);
}

DESCRIBE(a_build_molto_does_not_configure_runs_nothing) {
    const recipe_build none = {0};
    char err[512] = "";
    EXPECT_TRUE(configure_dependency("fake", "/nonexistent", &none, "cc", NULL, err, sizeof err));
}
