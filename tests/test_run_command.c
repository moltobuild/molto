#include "test_framework.h"
#include "tests.h"

#include <molto/commands/run_command.h>
#include <molto/services/fs_service.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void suite_run_command(void) {
    char root[] = "/tmp/molto_run_XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    CHECK(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    CHECK(fs_write_file(path,
        "[package]\n"
        "name = \"runner\"\n"
        "version = \"0.1.0\"\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    /* Exit code echoes the number of forwarded arguments. */
    CHECK(fs_write_file(path,
        "int main(int argc, char **argv) { (void)argv; return argc - 1; }\n"));

    char previous[4096];
    CHECK(getcwd(previous, sizeof previous) != NULL);
    CHECK(chdir(root) == 0);

    /* Build + run with no forwarded arguments -> program returns 0. */
    CHECK(run_command_run(NULL, NULL, 0) == 0);

    /* Forwarded arguments reach the program (argc - 1 == 2). */
    char *forwarded[] = { "alpha", "beta" };
    CHECK(run_command_run(NULL, forwarded, 2) == 2);

    /* A program that dies from a signal is reported as 128 + signal, not as a
       "failed to start" error. The sleep(1) ensures the rewritten source is
       newer than the previous object so it actually recompiles. */
    sleep(1);
    snprintf(path, sizeof path, "%s/src/main.c", root);
    CHECK(fs_write_file(path,
        "#include <signal.h>\n"
        "int main(void) { raise(SIGTERM); return 0; }\n"));
    CHECK(run_command_run(NULL, NULL, 0) == 128 + SIGTERM);

    CHECK(chdir(previous) == 0);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
