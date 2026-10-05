#include <moltest.h>

#include <molto/commands/test_command.h>
#include <molto/exit_code.h>
#include <molto/build/profile.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

MOLTEST(test_command) {
    char root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_test_cmd", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/tests", root);
    EXPECT_TRUE(fs_make_dirs(path));

    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"runner\"\nversion = \"0.1.0\"\n"));

    /* Project code the tests can link against. */
    snprintf(path, sizeof path, "%s/src/util.h", root);
    EXPECT_TRUE(fs_write_file(path, "int answer(void);\n"));
    snprintf(path, sizeof path, "%s/src/util.c", root);
    EXPECT_TRUE(fs_write_file(path, "#include \"util.h\"\nint answer(void) { return 7; }\n"));
    /* App entry point: must be excluded from the test links (each test has main). */
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    /* A passing test that calls into the project, and a failing test. */
    snprintf(path, sizeof path, "%s/tests/test_pass.c", root);
    EXPECT_TRUE(fs_write_file(path,
        "#include \"util.h\"\n"
        "int main(void) { return answer() == 7 ? 0 : 1; }\n"));
    char fail_path[512];
    snprintf(fail_path, sizeof fail_path, "%s/tests/test_fail.c", root);
    EXPECT_TRUE(fs_write_file(fail_path, "int main(void) { return 1; }\n"));

    char previous[4096];
    EXPECT_TRUE(getcwd(previous, sizeof previous) != NULL);
    EXPECT_TRUE(chdir(root) == 0);

    /* One test fails -> non-zero exit; both binaries built. */
    EXPECT_TRUE(test_command_run(NULL, false, 0, NULL, 0) == exit_build_failure);
    EXPECT_TRUE(fs_path_exists("build/debug/tests/test_pass" FS_EXECUTABLE_SUFFIX));
    EXPECT_TRUE(fs_path_exists("build/debug/tests/test_fail" FS_EXECUTABLE_SUFFIX));

    /* Fix the failing test -> everything passes. */
    EXPECT_TRUE(fs_write_file("tests/test_fail.c", "int main(void) { return 0; }\n"));
    EXPECT_TRUE(test_command_run(NULL, false, 0, NULL, 0) == exit_ok);

    EXPECT_TRUE(chdir(previous) == 0);

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(test_command_runs_the_binaries_with_the_projects_env) {
    /* The manifest exports [env] to the compiler and to `molto run`; a test
       binary is a program the project asked to be run too, and used to be the
       one place the table did not reach. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_test_env", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/tests", root);
    EXPECT_TRUE(fs_make_dirs(path));

    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"greeter\"\n"
                                    "[env]\nMOLTO_TEST_GREETING = \"hello\"\n"));
    snprintf(path, sizeof path, "%s/src/lib.c", root);
    EXPECT_TRUE(fs_write_file(path, "int lib(void) { return 0; }\n"));

    /* The assertion is the exit code: the test can only pass if the variable
       reached it. */
    snprintf(path, sizeof path, "%s/tests/test_env.c", root);
    EXPECT_TRUE(fs_write_file(path, "#include <stdlib.h>\n"
                                    "#include <string.h>\n"
                                    "int main(void) {\n"
                                    "    const char *say = getenv(\"MOLTO_TEST_GREETING\");\n"
                                    "    return say != NULL && strcmp(say, \"hello\") == 0"
                                    " ? 0 : 1;\n"
                                    "}\n"));

    char previous[4096];
    EXPECT_TRUE(getcwd(previous, sizeof previous) != NULL);
    EXPECT_TRUE(chdir(root) == 0);

    EXPECT_EQ(exit_ok, test_command_run(NULL, false, 0, NULL, 0));

    EXPECT_TRUE(chdir(previous) == 0);

    /* The other half of the promise: the variables are set after forking, so
       the process that ran the tests never saw them. */
    EXPECT_NULL(getenv("MOLTO_TEST_GREETING"));

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(test_build_prunes_a_deleted_test) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_test_prune", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/tests", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"pruner\"\n"));
    snprintf(path, sizeof path, "%s/src/lib.c", root);
    EXPECT_TRUE(fs_write_file(path, "int lib(void) { return 0; }\n"));
    snprintf(path, sizeof path, "%s/tests/keep.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));
    char doomed[512];
    snprintf(doomed, sizeof doomed, "%s/tests/doomed.c", root);
    EXPECT_TRUE(fs_write_file(doomed, "int main(void) { return 0; }\n"));

    str_list binaries;
    str_list_init(&binaries);
    ASSERT_TRUE(build_tests(root, profile_debug, NULL, false, 0, &binaries, NULL) == exit_ok);
    EXPECT_EQ(2, str_list_count(&binaries));
    str_list_free(&binaries);

    char doomed_binary[512];
    snprintf(doomed_binary, sizeof doomed_binary, "%s/build/debug/tests/doomed" FS_EXECUTABLE_SUFFIX, root);
    char doomed_object[512];
    snprintf(doomed_object, sizeof doomed_object,
             "%s/build/debug/obj/tests/doomed.c.o", root);
    EXPECT_TRUE(fs_path_exists(doomed_binary));
    EXPECT_TRUE(fs_path_exists(doomed_object));

    /* Deleting the source used to leave a ghost executable behind, which
       `molto test` would keep running forever. */
    EXPECT_TRUE(remove(doomed) == 0);
    str_list_init(&binaries);
    ASSERT_TRUE(build_tests(root, profile_debug, NULL, false, 0, &binaries, NULL) == exit_ok);
    EXPECT_EQ(1, str_list_count(&binaries));
    str_list_free(&binaries);

    EXPECT_FALSE(fs_path_exists(doomed_binary));
    EXPECT_FALSE(fs_path_exists(doomed_object));

    /* The surviving test is untouched. */
    char keep_binary[512];
    snprintf(keep_binary, sizeof keep_binary, "%s/build/debug/tests/keep" FS_EXECUTABLE_SUFFIX, root);
    EXPECT_TRUE(fs_path_exists(keep_binary));

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(test_command_forwards_what_follows_the_double_dash) {
    /* `molto test -- -v -k json` reaches the suite as its own arguments: the
       bootstrap's `make test TEST_ARGS=...` and CI's `-v` go through here. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_test_args", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/tests", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"args\"\n"));
    snprintf(path, sizeof path, "%s/src/lib.c", root);
    EXPECT_TRUE(fs_write_file(path, "int lib(void) { return 0; }\n"));

    /* Passes only when called with exactly `-k json`, after its own name. */
    snprintf(path, sizeof path, "%s/tests/test_args.c", root);
    EXPECT_TRUE(fs_write_file(path, "#include <string.h>\n"
                                    "int main(int argc, char **argv) {\n"
                                    "    return argc == 3 && strcmp(argv[1], \"-k\") == 0 &&\n"
                                    "           strcmp(argv[2], \"json\") == 0 ? 0 : 1;\n"
                                    "}\n"));

    char previous[4096];
    EXPECT_TRUE(getcwd(previous, sizeof previous) != NULL);
    EXPECT_TRUE(chdir(root) == 0);

    char k[] = "-k";
    char json[] = "json";
    char *forwarded[] = {k, json};
    EXPECT_EQ(exit_ok, test_command_run(NULL, false, 0, forwarded, 2));
    /* And without them it fails, so the pass above is the forwarding's. */
    EXPECT_EQ(exit_build_failure, test_command_run(NULL, false, 0, NULL, 0));

    EXPECT_TRUE(chdir(previous) == 0);
    (void)fs_remove_tree(root);
}

DESCRIBE(test_command_tells_each_binary_its_place_in_the_run) {
    /* RFC-0020: a plugin that measures the whole run (moltest-coverage) erases
       in the first executable and reports in the last, and only Molto knows
       which those are. Each binary appends what it was told to one file, so
       the file is the run as the binaries saw it. [env] tries to set both
       variables too, and loses: a position the manifest could fake would make
       a plugin judge a partial run as whole. The file is opened in binary
       mode: in text mode Windows writes "\r\n", which is not what is compared. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_test_position", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/tests", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"position\"\n"
                                    "[env]\nMOLTO_TEST_INDEX = \"99\"\n"
                                    "MOLTO_TEST_COUNT = \"99\"\n"));
    snprintf(path, sizeof path, "%s/src/lib.c", root);
    EXPECT_TRUE(fs_write_file(path, "int lib(void) { return 0; }\n"));

    static const char *const recorder =
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "int main(void) {\n"
        "    const char *index = getenv(\"MOLTO_TEST_INDEX\");\n"
        "    const char *count = getenv(\"MOLTO_TEST_COUNT\");\n"
        "    FILE *out = fopen(\"positions.txt\", \"ab\");\n"
        "    if(out == NULL) return 1;\n"
        "    fprintf(out, \"%s/%s\\n\", index ? index : \"-\", count ? count : \"-\");\n"
        "    return fclose(out) == 0 ? 0 : 1;\n"
        "}\n";
    snprintf(path, sizeof path, "%s/tests/test_a.c", root);
    EXPECT_TRUE(fs_write_file(path, recorder));
    snprintf(path, sizeof path, "%s/tests/test_b.c", root);
    EXPECT_TRUE(fs_write_file(path, recorder));

    char previous[4096];
    EXPECT_TRUE(getcwd(previous, sizeof previous) != NULL);
    EXPECT_TRUE(chdir(root) == 0);

    EXPECT_EQ(exit_ok, test_command_run(NULL, false, 0, NULL, 0));
    char *seen = fs_read_file("positions.txt");
    ASSERT_NOT_NULL(seen);
    EXPECT_STREQ("1/2\n2/2\n", seen);
    free(seen);

    EXPECT_TRUE(chdir(previous) == 0);
    (void)fs_remove_tree(root);
}
