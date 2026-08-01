#include <moltest.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A project whose tests register themselves and have no main() of their own,
   the shape a test framework imposes. The runner lives outside src/, which is
   the other half of what [test] has to make possible. */
static bool framework_project(char *root, size_t root_size, const char *manifest) {
    snprintf(root, root_size, "%s", "/tmp/molto_modes_XXXXXX");
    if (mkdtemp(root) == NULL)
        return false;

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    if (!fs_make_dirs(path))
        return false;
    snprintf(path, sizeof path, "%s/tests", root);
    if (!fs_make_dirs(path))
        return false;
    snprintf(path, sizeof path, "%s/framework/src", root);
    if (!fs_make_dirs(path))
        return false;
    snprintf(path, sizeof path, "%s/framework/include", root);
    if (!fs_make_dirs(path))
        return false;

    snprintf(path, sizeof path, "%s/Project.toml", root);
    if (!fs_write_file(path, manifest))
        return false;

    /* The library under test, plus an app entry point that must stay out of
       the test link. */
    snprintf(path, sizeof path, "%s/src/lib.c", root);
    if (!fs_write_file(path, "int lib_answer(void) { return 0; }\n"))
        return false;
    snprintf(path, sizeof path, "%s/src/main.c", root);
    if (!fs_write_file(path, "int main(void) { return 0; }\n"))
        return false;

    /* The framework: a registry, and the main() the test files do not have. */
    snprintf(path, sizeof path, "%s/framework/include/tiny.h", root);
    if (!fs_write_file(path,
            "#ifndef TINY_H\n#define TINY_H\n"
            "void tiny_register(const char *name);\n"
            "#define TINY_CASE(n) \\\n"
            "    static void n##_body(void); \\\n"
            "    __attribute__((constructor)) static void n##_reg(void) { \\\n"
            "        tiny_register(#n); \\\n"
            "    } \\\n"
            "    static void n##_body(void)\n"
            "#endif\n"))
        return false;
    snprintf(path, sizeof path, "%s/framework/src/tiny.c", root);
    if (!fs_write_file(path,
            "#include <stdio.h>\n"
            "static int registered = 0;\n"
            "void tiny_register(const char *name) { (void)name; registered++; }\n"
            "int main(void) { printf(\"%d\\n\", registered); return 0; }\n"))
        return false;

    /* Two test files, neither with a main(). */
    snprintf(path, sizeof path, "%s/tests/test_one.c", root);
    if (!fs_write_file(path, "#include <tiny.h>\nTINY_CASE(one) { }\n"))
        return false;
    snprintf(path, sizeof path, "%s/tests/test_two.c", root);
    return fs_write_file(path, "#include <tiny.h>\nTINY_CASE(two) { }\n");
}

static void cleanup(const char *root) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(tests_link_into_one_binary_in_single_mode) {
    char root[64];
    ASSERT_TRUE(framework_project(root, sizeof root,
        "[package]\nname = \"suite\"\n"
        "[target]\nstd = \"c11\"\n"
        "[test]\n"
        "mode = \"single\"\n"
        "sources = [\"framework/src\"]\n"
        "include = [\"framework/include\"]\n"));

    str_list binaries;
    str_list_init(&binaries);
    ASSERT_TRUE(build_tests(root, profile_debug, false, &binaries) == exit_ok);

    /* One executable for the whole suite, named after the package. */
    ASSERT_EQ(1, str_list_count(&binaries));
    char expected[512];
    snprintf(expected, sizeof expected, "%s/build/debug/tests/suite_tests", root);
    EXPECT_STREQ(expected, str_list_get(&binaries, 0));
    EXPECT_TRUE(fs_path_exists(expected));

    /* It runs, and both test files registered into it — which only happens if
       they were linked together with the framework that owns main(). */
    char cmd[700];
    snprintf(cmd, sizeof cmd, "%s > %s/out.txt 2>&1", expected, root);
    EXPECT_TRUE(system(cmd) == 0);
    char out_path[512];
    snprintf(out_path, sizeof out_path, "%s/out.txt", root);
    char *out = fs_read_file(out_path);
    ASSERT_NOT_NULL(out);
    EXPECT_NOT_NULL(strstr(out, "2"));
    free(out);

    str_list_free(&binaries);
    cleanup(root);
}

MOLTEST(per_file_mode_is_still_the_default) {
    char root[64];
    /* Same project, no [test].mode: the contract RFC-0002 describes is
       unchanged, one executable per test file. These have no main(), so
       linking them individually must fail — which is the proof they were not
       quietly linked as a suite. */
    ASSERT_TRUE(framework_project(root, sizeof root,
        "[package]\nname = \"suite\"\n"
        "[target]\nstd = \"c11\"\n"
        "[test]\n"
        "sources = [\"framework/src\"]\n"
        "include = [\"framework/include\"]\n"));

    str_list binaries;
    str_list_init(&binaries);
    EXPECT_TRUE(build_tests(root, profile_debug, false, &binaries) != exit_ok);

    str_list_free(&binaries);
    cleanup(root);
}

MOLTEST(extra_test_sources_must_exist) {
    char root[64];
    ASSERT_TRUE(framework_project(root, sizeof root,
        "[package]\nname = \"suite\"\n"
        "[target]\nstd = \"c11\"\n"
        "[test]\nmode = \"single\"\nsources = [\"nowhere\"]\n"));

    /* A source that is not there is an error, not something to skip: the tests
       would fail to link later with a message about a missing symbol. */
    str_list binaries;
    str_list_init(&binaries);
    EXPECT_TRUE(build_tests(root, profile_debug, false, &binaries) != exit_ok);

    str_list_free(&binaries);
    cleanup(root);
}

MOLTEST(an_unknown_test_mode_is_a_manifest_error) {
    char root[64];
    ASSERT_TRUE(framework_project(root, sizeof root,
        "[package]\nname = \"suite\"\n"
        "[test]\nmode = \"whatever\"\n"));

    str_list binaries;
    str_list_init(&binaries);
    EXPECT_TRUE(build_tests(root, profile_debug, false, &binaries) == exit_invalid_manifest);

    str_list_free(&binaries);
    cleanup(root);
}
