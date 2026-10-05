#include <moltest.h>

#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * `[[test.isolated]]` (RFC-0021): a test file linked alone, without the sources
 * it replaces, so a fake it defines stands in for the real function.
 *
 * Every project here has the same shape: `clock.c` defines clock_now() and
 * clock_zone(), and `app.c` calls clock_now() from another file. A fake of
 * clock_now() only links where clock.c is left out, and app.c only reaches
 * clock_now(), so that is the one fake a test needs: the rest of the replaced
 * file is never pulled in, which is what linking it as an archive buys.
 */

static const char *const CLOCK_H = "int clock_now(void);\nint clock_zone(void);\n";
static const char *const CLOCK_C = "#include \"clock.h\"\n"
                                   "int clock_now(void) { return 1000; }\n"
                                   "int clock_zone(void) { return 2; }\n";
static const char *const APP_C = "#include \"clock.h\"\n"
                                 "int app_elapsed(int start) { return clock_now() - start; }\n";

/* Passes only against the real clock. */
static const char *const REAL_TEST = "int app_elapsed(int start);\n"
                                     "int main(void) { return app_elapsed(0) == 1000 ? 0 : 1; }\n";
/* Passes only against its own fake, which would collide with the real one. */
static const char *const FAKE_TEST = "int app_elapsed(int start);\n"
                                     "int clock_now(void) { return 41; }\n"
                                     "int main(void) { return app_elapsed(1) == 40 ? 0 : 1; }\n";

static bool write_in(const char *root, const char *relative, const char *text) {
    char path[MOLTEST_PATH + 64];
    if(!fs_format_path(path, sizeof path, "%s/%s", root, relative))
        return false;
    char parent[sizeof path];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if(slash != NULL) {
        *slash = '\0';
        if(!fs_make_dirs(parent))
            return false;
    }
    return fs_write_file(path, text);
}

/* A project with src/clock.c, src/app.c and the given manifest. */
static bool clock_project(char *root, size_t root_size, const char *manifest) {
    return moltest_temp_dir("molto_isolated", root, root_size) &&
           write_in(root, "Project.toml", manifest) && write_in(root, "src/clock.h", CLOCK_H) &&
           write_in(root, "src/clock.c", CLOCK_C) && write_in(root, "src/app.c", APP_C);
}

/* Run every binary the build produced; how many exited 0. */
static size_t run_all(const str_list *binaries) {
    size_t passed = 0;
    for(size_t i = 0; i < str_list_count(binaries); i++) {
        const char *argv[] = {str_list_get(binaries, i), NULL};
        if(process_run(argv) == 0)
            passed++;
    }
    return passed;
}

static bool ends_with(const char *text, const char *suffix) {
    const size_t a = strlen(text);
    const size_t b = strlen(suffix);
    return a >= b && strcmp(text + a - b, suffix) == 0;
}

DESCRIBE(an_isolated_test_links_its_fake_instead_of_the_replaced_source) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(clock_project(root, sizeof root,
                              "[package]\nname = \"app\"\n"
                              "[[test.isolated]]\n"
                              "file = \"tests/test_fake.c\"\n"
                              "replaces = [\"src/clock.c\"]\n"));
    ASSERT_TRUE(write_in(root, "tests/test_real.c", REAL_TEST));
    ASSERT_TRUE(write_in(root, "tests/test_fake.c", FAKE_TEST));

    str_list binaries;
    str_list_init(&binaries);
    ASSERT_EQ(exit_ok, build_tests(root, profile_debug, NULL, false, 0, &binaries, NULL));
    /* Both linked, each against its own clock, and both pass. */
    EXPECT_EQ(2, (int)str_list_count(&binaries));
    EXPECT_EQ(2, (int)run_all(&binaries));

    str_list_free(&binaries);
    (void)fs_remove_tree(root);
}

DESCRIBE(an_isolated_test_leaves_the_single_suite_for_an_executable_of_its_own) {
    /* In single mode a framework owns main(): here a harness outside tests/
       that calls check(), defined by the one test file each executable has. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(clock_project(root, sizeof root,
                              "[package]\nname = \"app\"\n"
                              "[test]\nmode = \"single\"\nsources = [\"harness\"]\n"
                              "[[test.isolated]]\n"
                              "file = \"tests/test_fake.c\"\n"
                              "replaces = [\"src/clock.c\"]\n"));
    ASSERT_TRUE(write_in(root, "harness/main.c",
                         "int check(void);\nint main(void) { return check(); }\n"));
    ASSERT_TRUE(write_in(root, "tests/test_real.c",
                         "int app_elapsed(int start);\n"
                         "int check(void) { return app_elapsed(0) == 1000 ? 0 : 1; }\n"));
    ASSERT_TRUE(write_in(root, "tests/test_fake.c",
                         "int app_elapsed(int start);\n"
                         "int clock_now(void) { return 41; }\n"
                         "int check(void) { return app_elapsed(1) == 40 ? 0 : 1; }\n"));

    str_list binaries;
    str_list_init(&binaries);
    ASSERT_EQ(exit_ok, build_tests(root, profile_debug, NULL, false, 0, &binaries, NULL));
    ASSERT_EQ(2, (int)str_list_count(&binaries));
    /* The suite first, then the isolated file under its own stem. */
    EXPECT_TRUE(ends_with(str_list_get(&binaries, 0), "app_tests" FS_EXECUTABLE_SUFFIX));
    EXPECT_TRUE(ends_with(str_list_get(&binaries, 1), "test_fake" FS_EXECUTABLE_SUFFIX));
    EXPECT_EQ(2, (int)run_all(&binaries));

    str_list_free(&binaries);
    (void)fs_remove_tree(root);
}

DESCRIBE(an_isolated_test_replaces_a_source_of_a_dependency) {
    /* moltest-mock KI-3: a dependency is compiled into the test binary, so a
       fake of one of its functions collided with it. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_isolated_dep", root, sizeof root));
    ASSERT_TRUE(write_in(root, "clocklib/include/clock.h", CLOCK_H));
    ASSERT_TRUE(write_in(root, "clocklib/src/clock.c", CLOCK_C));
    ASSERT_TRUE(write_in(root, "clocklib/recipe.toml",
                         "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                         "name = \"clocklib\"\nversion = \"0.1.0\"\ntarget = \"any\"\n"
                         "[artifacts]\ntype = \"source\"\nstd = \"c17\"\n"
                         "sources = [\"src/clock.c\"]\ninclude = [\"include\"]\n"));
    char manifest[MOLTEST_PATH * 2];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\n"
             "[deps]\nclocklib = { path = \"%s/clocklib\" }\n"
             "[[test.isolated]]\n"
             "file = \"tests/test_fake.c\"\n"
             "replaces = [\"clocklib:src/clock.c\"]\n",
             root);
    char app[MOLTEST_PATH + 16];
    ASSERT_TRUE(fs_format_path(app, sizeof app, "%s/app", root));
    ASSERT_TRUE(write_in(app, "Project.toml", manifest));
    ASSERT_TRUE(write_in(app, "src/app.c", APP_C));
    ASSERT_TRUE(write_in(app, "tests/test_real.c", REAL_TEST));
    ASSERT_TRUE(write_in(app, "tests/test_fake.c", FAKE_TEST));

    str_list binaries;
    str_list_init(&binaries);
    ASSERT_EQ(exit_ok, build_tests(app, profile_debug, NULL, false, 0, &binaries, NULL));
    EXPECT_EQ(2, (int)str_list_count(&binaries));
    EXPECT_EQ(2, (int)run_all(&binaries));

    str_list_free(&binaries);
    (void)fs_remove_tree(root);
}

DESCRIBE(a_replacement_that_names_no_source_is_a_manifest_error) {
    /* A typo replaces nothing, leaves the real function in the binary, and
       would fail later as a duplicate symbol pointing nowhere near the cause. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(clock_project(root, sizeof root,
                              "[package]\nname = \"app\"\n"
                              "[[test.isolated]]\n"
                              "file = \"tests/test_fake.c\"\n"
                              "replaces = [\"src/clok.c\"]\n"));
    ASSERT_TRUE(write_in(root, "tests/test_fake.c", FAKE_TEST));

    str_list binaries;
    str_list_init(&binaries);
    EXPECT_EQ(exit_invalid_manifest,
              build_tests(root, profile_debug, NULL, false, 0, &binaries, NULL));

    str_list_free(&binaries);
    (void)fs_remove_tree(root);
}

DESCRIBE(an_isolated_file_that_is_not_a_test_is_a_manifest_error) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(clock_project(root, sizeof root,
                              "[package]\nname = \"app\"\n"
                              "[[test.isolated]]\n"
                              "file = \"tests/test_missing.c\"\n"
                              "replaces = [\"src/clock.c\"]\n"));
    ASSERT_TRUE(write_in(root, "tests/test_real.c", REAL_TEST));

    str_list binaries;
    str_list_init(&binaries);
    EXPECT_EQ(exit_invalid_manifest,
              build_tests(root, profile_debug, NULL, false, 0, &binaries, NULL));

    str_list_free(&binaries);
    (void)fs_remove_tree(root);
}

DESCRIBE(an_isolated_test_without_the_fake_it_needs_fails_to_link) {
    /* Leaving clock.c out leaves clock_now() out; a test that reaches it and
       fakes nothing has an undefined symbol, which is a failed build. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(clock_project(root, sizeof root,
                              "[package]\nname = \"app\"\n"
                              "[[test.isolated]]\n"
                              "file = \"tests/test_real.c\"\n"
                              "replaces = [\"src/clock.c\"]\n"));
    ASSERT_TRUE(write_in(root, "tests/test_real.c", REAL_TEST));

    str_list binaries;
    str_list_init(&binaries);
    EXPECT_EQ(exit_build_failure,
              build_tests(root, profile_debug, NULL, false, 0, &binaries, NULL));

    str_list_free(&binaries);
    (void)fs_remove_tree(root);
}

/* Build the tests with a report written to a file, and hand back what it said.
   Caller frees. */
static char *build_and_read_report(const char *root, int *code) {
    char path[MOLTEST_PATH];
    if(!moltest_temp_file("molto_isolated_report", path, sizeof path))
        return NULL;
    FILE *out = fopen(path, "w");
    if(out == NULL)
        return NULL;
    build_report *report = build_report_create(out);
    str_list binaries;
    str_list_init(&binaries);
    *code = build_tests_with(root, profile_debug, NULL, false, 0, &binaries, NULL, NULL, report);
    build_report_destroy(report);
    str_list_free(&binaries);
    fclose(out);
    char *text = fs_read_file(path);
    remove(path);
    return text;
}

DESCRIBE(the_link_note_names_both_places_a_missing_fake_can_come_from) {
    /* app.c also calls ext_log(), which nothing in src/ defines: in the shared
       suite tests/test_fakes.c fakes it, as one fakes another library. The
       isolated test links without that file, and the linker takes app.c whole,
       so ext_log() is missing — and it was never the replaced file's. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(clock_project(root, sizeof root,
                              "[package]\nname = \"app\"\n"
                              "[test]\nmode = \"single\"\nsources = [\"harness\"]\n"
                              "[[test.isolated]]\n"
                              "file = \"tests/test_zone.c\"\n"
                              "replaces = [\"src/clock.c\"]\n"));
    ASSERT_TRUE(write_in(root, "src/log.c",
                         "void ext_log(int level);\n"
                         "int app_logged(void) { ext_log(1); return 0; }\n"
                         "int clock_now(void);\n"
                         "int app_now(void) { return clock_now(); }\n"));
    ASSERT_TRUE(write_in(root, "harness/main.c",
                         "int check(void);\nint main(void) { return check(); }\n"));
    ASSERT_TRUE(write_in(root, "tests/test_fakes.c",
                         "int app_logged(void);\n"
                         "void ext_log(int level) { (void)level; }\n"
                         "int check(void) { return app_logged(); }\n"));
    ASSERT_TRUE(write_in(root, "tests/test_zone.c",
                         "int app_now(void);\n"
                         "int clock_now(void) { return 7; }\n"
                         "int check(void) { return app_now() == 7 ? 0 : 1; }\n"));

    int code = 0;
    char *said = build_and_read_report(root, &code);
    EXPECT_EQ(exit_build_failure, code);
    ASSERT_NOT_NULL(said);
    EXPECT_NOT_NULL(strstr(said, "tests/test_zone.c replaces src/clock.c"));
    EXPECT_NOT_NULL(strstr(said, "another test file"));
    free(said);
    (void)fs_remove_tree(root);
}
