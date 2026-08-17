#include <moltest.h>

#include <molto/services/process_service.h>

#include <stdlib.h>
#include <string.h>
#include <threads.h>

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

/* Hold a capture open for long enough that another one overlaps it. */
static int hold_a_pipe_open(void *unused) {
    (void)unused;
    const char *const argv[] = { "sh", "-c", "sleep 1", NULL };
    char out[64] = "";
    return process_capture_all(argv, NULL, 0, out, sizeof out, NULL);
}

/* How many descriptors a child of this process inherits. Counted rather than
   listed, and counted the same way both times it is asked, so whatever else
   the test binary happens to hold open cancels out. */
static unsigned long inherited_descriptors(void) {
    const char *const argv[] = { "sh", "-c", "ls /proc/self/fd | wc -l", NULL };
    char out[64] = "";
    if(process_capture_all(argv, NULL, 0, out, sizeof out, NULL) != 0)
        return 0;
    return strtoul(out, NULL, 10);
}

/* The pipe one capture opens must not reach another capture's child.
 *
 * A pipe reaches EOF when the last copy of its write end is closed, so a
 * compiler that inherited someone else's pipe holds that capture open for as
 * long as it runs. Under `-j` that turns a parallel build into a queue behind
 * whichever unit happens to be slowest — not a deadlock, and all the harder to
 * see for it. */
MOLTEST(a_capture_does_not_leak_its_pipe_into_another_child) {
    const unsigned long alone = inherited_descriptors();
    ASSERT_TRUE(alone > 0);

    thrd_t holder;
    ASSERT_EQ(thrd_success, thrd_create(&holder, hold_a_pipe_open, NULL));
    (void)thrd_sleep(&(struct timespec){ .tv_nsec = 200000000L }, NULL); /* 200 ms */

    const unsigned long beside_a_capture = inherited_descriptors();
    EXPECT_EQ(alone, beside_a_capture);

    int held = -1;
    (void)thrd_join(holder, &held);
    EXPECT_EQ(0, held);
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
