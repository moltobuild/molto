#include <moltest.h>

#include <molto/commands/run_command.h>
#include <molto/services/fs_service.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

MOLTEST(run_command) {
    char root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_run", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\n"
        "name = \"runner\"\n"
        "version = \"0.1.0\"\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    /* Exit code echoes the number of forwarded arguments. */
    EXPECT_TRUE(fs_write_file(path,
        "int main(int argc, char **argv) { (void)argv; return argc - 1; }\n"));

    char previous[4096];
    EXPECT_TRUE(getcwd(previous, sizeof previous) != NULL);
    EXPECT_TRUE(chdir(root) == 0);

    /* Build + run with no forwarded arguments -> program returns 0. */
    EXPECT_TRUE(run_command_run(NULL, false, 0, NULL, 0) == 0);

    /* Forwarded arguments reach the program (argc - 1 == 2). */
    char *forwarded[] = { "alpha", "beta" };
    EXPECT_TRUE(run_command_run(NULL, false, 0, forwarded, 2) == 2);

    /* A program that dies abnormally is reported as 128 + signal, not as a
       "failed to start" error.

       A page fault rather than `raise(SIGTERM)`, because the two platforms
       agree about the first and not the second: POSIX raises SIGSEGV, Windows
       exits with EXCEPTION_ACCESS_VIOLATION, and both arrive here as
       `128 + SIGSEGV`. A raised SIGTERM is handled by the C runtime on
       Windows, which leaves with 3 -- a status no parent can tell from an
       ordinary `exit(3)`. */
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path,
        "#ifdef _WIN32\n"
        "#include <windows.h>\n"
        "#endif\n"
        "int main(void) {\n"
        "#ifdef _WIN32\n"
        "    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);\n"
        "#endif\n"
        "    volatile int *nowhere = 0;\n"
        "    *nowhere = 1;\n"
        "    return 0;\n"
        "}\n"));
    EXPECT_EQ(128 + SIGSEGV, run_command_run(NULL, false, 0, NULL, 0));

    EXPECT_TRUE(chdir(previous) == 0);

    char cmd[600];
    (void)fs_remove_tree(root);
}
