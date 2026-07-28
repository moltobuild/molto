#include "test_framework.h"
#include "tests.h"

#include <molto/commands/test_command.h>
#include <molto/exit_code.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void suite_test_command(void) {
    char root[] = "/tmp/molto_test_cmd_XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    CHECK(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/tests", root);
    CHECK(fs_make_dirs(path));

    snprintf(path, sizeof path, "%s/Project.toml", root);
    CHECK(fs_write_file(path, "[package]\nname = \"runner\"\nversion = \"0.1.0\"\n"));

    /* Project code the tests can link against. */
    snprintf(path, sizeof path, "%s/src/util.h", root);
    CHECK(fs_write_file(path, "int answer(void);\n"));
    snprintf(path, sizeof path, "%s/src/util.c", root);
    CHECK(fs_write_file(path, "#include \"util.h\"\nint answer(void) { return 7; }\n"));
    /* App entry point: must be excluded from the test links (each test has main). */
    snprintf(path, sizeof path, "%s/src/main.c", root);
    CHECK(fs_write_file(path, "int main(void) { return 0; }\n"));

    /* A passing test that calls into the project, and a failing test. */
    snprintf(path, sizeof path, "%s/tests/test_pass.c", root);
    CHECK(fs_write_file(path,
        "#include \"util.h\"\n"
        "int main(void) { return answer() == 7 ? 0 : 1; }\n"));
    char fail_path[512];
    snprintf(fail_path, sizeof fail_path, "%s/tests/test_fail.c", root);
    CHECK(fs_write_file(fail_path, "int main(void) { return 1; }\n"));

    char previous[4096];
    CHECK(getcwd(previous, sizeof previous) != NULL);
    CHECK(chdir(root) == 0);

    /* One test fails -> non-zero exit; both binaries built. */
    CHECK(test_command_run(NULL) == exit_build_failure);
    CHECK(fs_path_exists("build/debug/tests/test_pass"));
    CHECK(fs_path_exists("build/debug/tests/test_fail"));

    /* Fix the failing test -> everything passes. */
    sleep(1);
    CHECK(fs_write_file("tests/test_fail.c", "int main(void) { return 0; }\n"));
    CHECK(test_command_run(NULL) == exit_ok);

    CHECK(chdir(previous) == 0);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
