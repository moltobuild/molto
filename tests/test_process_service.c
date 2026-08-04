#include <moltest.h>

#include <molto/services/process_service.h>

#include <stdlib.h>
#include <string.h>

MOLTEST(process_service) {
    const char *ok[] = { "true", NULL };
    EXPECT_TRUE(process_run(ok) == 0);

    const char *fail[] = { "false", NULL };
    EXPECT_TRUE(process_run(fail) != 0);

    /* A command that cannot be found: execvp fails and the child exits 127. */
    const char *missing[] = { "molto_no_such_command_zzz", NULL };
    int code = process_run(missing);
    EXPECT_TRUE(code == 127 || code == -1);

    /* A child killed by a signal is reported as 128 + signal. */
    const char *killed[] = { "sh", "-c", "kill -TERM $$", NULL };
    EXPECT_TRUE(process_run(killed) == 128 + 15); /* SIGTERM = 15 */
}

MOLTEST(process_exports_env_only_to_the_child) {
    /* The child sees the variable... */
    const char *const argv[] = {
        "sh", "-c", "test \"$MOLTO_PROBE\" = \"exported\"", NULL
    };
    const process_env_var vars[] = { { "MOLTO_PROBE", "exported" } };
    EXPECT_EQ(0, process_run_env(argv, vars, 1));

    /* ...and molto's own environment is left alone, so one project's [env]
       cannot leak into anything else this process does. */
    EXPECT_NULL(getenv("MOLTO_PROBE"));

    /* Without the variable, the same command fails. */
    EXPECT_TRUE(process_run_env(argv, NULL, 0) != 0);
}

MOLTEST(process_capture_all_reads_both_streams) {
    /* A compiler diagnoses on stderr and clang-tidy on stdout, so lint needs
       both; capturing only stdout would come back empty for the compiler. */
    const char *const argv[] = { "sh", "-c", "echo to-stdout; echo to-stderr >&2", NULL };
    char out[256] = "";
    bool truncated = true;

    EXPECT_EQ(0, process_capture_all(argv, NULL, 0, out, sizeof out, &truncated));
    EXPECT_NOT_NULL(strstr(out, "to-stdout"));
    EXPECT_NOT_NULL(strstr(out, "to-stderr"));
    EXPECT_FALSE(truncated);
}

MOLTEST(process_capture_all_exports_env_to_the_child) {
    const char *const argv[] = { "sh", "-c", "echo \"$MOLTO_PROBE\"", NULL };
    const process_env_var vars[] = { { "MOLTO_PROBE", "exported" } };
    char out[256] = "";

    EXPECT_EQ(0, process_capture_all(argv, vars, 1, out, sizeof out, NULL));
    EXPECT_NOT_NULL(strstr(out, "exported"));
    EXPECT_NULL(getenv("MOLTO_PROBE"));
}

MOLTEST(process_capture_all_truncates_without_killing_the_child) {
    /* More output than the buffer holds. The rest has to be read and thrown
       away: closing the pipe early would kill the child with SIGPIPE and report
       a signal death instead of the exit code it was about to give. */
    const char *const argv[] = {
        "sh", "-c", "head -c 200000 /dev/zero | tr '\\0' x; exit 7", NULL
    };
    char out[64] = "";
    bool truncated = false;

    EXPECT_EQ(7, process_capture_all(argv, NULL, 0, out, sizeof out, &truncated));
    EXPECT_TRUE(truncated);
    EXPECT_EQ(sizeof out - 1, strlen(out));
}

MOLTEST(process_capture_all_reports_a_child_that_could_not_run) {
    const char *const missing[] = { "molto_no_such_command_zzz", NULL };
    char out[64] = "";
    int code = process_capture_all(missing, NULL, 0, out, sizeof out, NULL);
    EXPECT_TRUE(code == 127 || code == -1);

    const char *const killed[] = { "sh", "-c", "kill -TERM $$", NULL };
    EXPECT_EQ(128 + 15, process_capture_all(killed, NULL, 0, out, sizeof out, NULL));
}

MOLTEST(process_capture_still_leaves_stderr_alone) {
    /* The old contract, guarded: pickup explains itself on stderr while Molto
       reads its answer on stdout. */
    const char *const argv[] = { "sh", "-c", "echo answer; echo noise >&2", NULL };
    char out[256] = "";

    EXPECT_EQ(0, process_capture(argv, out, sizeof out));
    EXPECT_NOT_NULL(strstr(out, "answer"));
    EXPECT_NULL(strstr(out, "noise"));
}

MOLTEST(process_builds_an_argv_from_a_list) {
    str_list list;
    str_list_init(&list);
    ASSERT_TRUE(str_list_push(&list, "echo"));
    ASSERT_TRUE(str_list_push(&list, "hello"));

    const char **argv = process_argv_from_list(&list);
    ASSERT_NOT_NULL(argv);
    EXPECT_STREQ("echo", argv[0]);
    EXPECT_STREQ("hello", argv[1]);
    EXPECT_NULL(argv[2]);

    char out[64] = "";
    EXPECT_EQ(0, process_capture_all(argv, NULL, 0, out, sizeof out, NULL));
    EXPECT_NOT_NULL(strstr(out, "hello"));

    free((void *)argv);
    str_list_free(&list);
}
