#include <moltest.h>

#include <molto/services/process_service.h>
#include <molto/util/thread.h>

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h> /* SetErrorMode, so a deliberate fault opens no dialog */
#endif

/*
 * A program that dies the way a broken program dies, on either platform.
 *
 * Each side asks for the death in the way its own platform defines, and both
 * arrive as `128 + SIGABRT`: one number, which is the point of translating
 * Windows' exception rather than growing a second contract for callers.
 *
 * Two things this may not do, both learnt the hard way.
 *
 * It may not fault a page. Writing through a null pointer is undefined
 * behaviour, and the sanitizers job exists to catch exactly that: UBSan
 * intercepts the store, reports it, and the child leaves with 1 rather than
 * dying. A test that needs undefined behaviour to pass is a test that cannot
 * run under the build that looks for it.
 *
 * And it may not raise SIGSEGV or SIGFPE, which are defined but no better
 * here: ASan installs handlers for both, so the child is caught there too and
 * again leaves with 1. `abort()` is the one abnormal death that reaches the
 * parent with the sanitizers on -- measured, not assumed.
 *
 * Not `raise(SIGTERM)`, which is what this test used to do: the C runtime on
 * Windows handles that itself and leaves with 3, and no parent can tell that
 * from `exit(3)`. The information is gone before the process ends.
 */
MOLTEST_FAKE(fake_crashing_program) {
    (void)argc;
    (void)argv;
#ifdef _WIN32
    /* `SetErrorMode` first, or the exception opens the Windows error dialog
       and the run waits on a box nobody is there to close.

       `abort()` is no use on this side: mingw's runtime leaves with 3, an
       ordinary status. STATUS_STACK_BUFFER_OVERRUN is what the platform's own
       fail-fast path raises, and `exception_to_signal` reads it as SIGABRT. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    RaiseException(0xC0000409u, EXCEPTION_NONCONTINUABLE, 0, NULL);
#else
    abort();
#endif
    return 0; /* not reached */
}

/* The crashing program, made once per test that wants it. */
static bool make_crasher(char *path, size_t size) {
    char at[MOLTEST_PATH];
    if(!moltest_temp_file("molto_crasher", at, sizeof at))
        return false;
    return moltest_fake_program(at, "behave fake_crashing_program\n", path, size);
}

MOLTEST(process_service) {
    const char *ok[] = { "true", NULL };
    EXPECT_TRUE(process_run(ok) == 0);

    const char *fail[] = { "false", NULL };
    EXPECT_TRUE(process_run(fail) != 0);

    /* A command that cannot be found: execvp fails and the child exits 127. */
    const char *missing[] = { "molto_no_such_command_zzz", NULL };
    int code = process_run(missing);
    EXPECT_TRUE(code == 127 || code == -1);

    /* A child that dies abnormally is reported as 128 + signal. */
    char crasher[MOLTEST_PATH];
    ASSERT_TRUE(make_crasher(crasher, sizeof crasher));
    const char *killed[] = { crasher, NULL };
    EXPECT_EQ(128 + SIGABRT, process_run(killed));
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

    char crasher[MOLTEST_PATH];
    ASSERT_TRUE(make_crasher(crasher, sizeof crasher));
    const char *const killed[] = { crasher, NULL };
    EXPECT_EQ(128 + SIGABRT, process_capture_all(killed, NULL, 0, out, sizeof out, NULL));
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

/* Hold a capture open for long enough that another one overlaps it.

   The result comes back through the argument rather than the return value: a
   thread's exit code is one of the things the two platforms disagree about, so
   molto/util/thread.h does not carry one back. */
static int hold_a_pipe_open(void *out_code) {
    const char *const argv[] = { "sh", "-c", "sleep 1", NULL };
    char out[64] = "";
    const int code = process_capture_all(argv, NULL, 0, out, sizeof out, NULL);
    *(int *)out_code = code;
    return code;
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

    int held = -1;
    thread holder;
    ASSERT_TRUE(thread_start(&holder, hold_a_pipe_open, &held));
    thread_sleep_ms(200);

    const unsigned long beside_a_capture = inherited_descriptors();
    EXPECT_EQ(alone, beside_a_capture);

    thread_join(&holder);
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

/* --- exchanging a document with a child process (RFC-0014) --- */

MOLTEST(process_exchange_sends_a_request_and_reads_the_answer) {
    /* `cat` is the identity plugin: it reads to EOF and writes back what it
       got, which is exactly the contract a frontend follows. */
    const char *const argv[] = {"cat", NULL};
    process_exchange io = {.request = "{\"schema\":1}", .timeout_ms = 5000};

    ASSERT_EQ(process_exchange_ok, process_exchange_run(argv, &io));
    EXPECT_EQ(0, io.code);
    ASSERT_NOT_NULL(io.answer);
    EXPECT_STREQ("{\"schema\":1}", io.answer);
    EXPECT_EQ(strlen("{\"schema\":1}"), io.answer_size);
    free(io.answer);
}

MOLTEST(process_exchange_does_not_deadlock_on_a_large_document) {
    /* The case that makes the poll loop necessary rather than nice. Writing the
       whole request before reading anything deadlocks here: the parent blocks
       on a full pipe to the child while the child blocks on a full pipe back,
       and neither is at fault. A megabyte is comfortably past both buffers. */
    const size_t size = 1024 * 1024;
    char *request = malloc(size + 1);
    ASSERT_NOT_NULL(request);
    memset(request, 'x', size);
    request[size] = '\0';

    const char *const argv[] = {"cat", NULL};
    process_exchange io = {.request = request, .request_size = size, .timeout_ms = 30000};

    ASSERT_EQ(process_exchange_ok, process_exchange_run(argv, &io));
    EXPECT_EQ(0, io.code);
    EXPECT_EQ(size, io.answer_size);
    free(io.answer);
    free(request);
}

MOLTEST(process_exchange_keeps_the_child_exit_code) {
    /* Exit 3 is a frontend declining — the file is not one it understands —
       which is not an error and lets molto try another. The exchange reports
       the number and does not interpret it. */
    const char *const argv[] = {"sh", "-c", "exit 3", NULL};
    process_exchange io = {.request = "{}", .timeout_ms = 5000};

    ASSERT_EQ(process_exchange_ok, process_exchange_run(argv, &io));
    EXPECT_EQ(3, io.code);
    free(io.answer);
}

MOLTEST(process_exchange_survives_a_child_that_never_reads) {
    /* Without SIGPIPE ignored this ends molto rather than the exchange, and it
       is the ordinary shape of a plugin that refuses before reading. */
    const char *const argv[] = {"sh", "-c", "exit 3", NULL};
    const size_t size = 1024 * 1024;
    char *request = malloc(size + 1);
    ASSERT_NOT_NULL(request);
    memset(request, 'x', size);
    request[size] = '\0';

    process_exchange io = {.request = request, .request_size = size, .timeout_ms = 10000};
    ASSERT_EQ(process_exchange_ok, process_exchange_run(argv, &io));
    EXPECT_EQ(3, io.code);
    free(io.answer);
    free(request);
}

MOLTEST(process_exchange_kills_a_child_that_never_finishes) {
    /* A plugin that hangs must not hang a build. */
    const char *const argv[] = {"sh", "-c", "sleep 30", NULL};
    process_exchange io = {.request = "{}", .timeout_ms = 200};

    ASSERT_EQ(process_exchange_timed_out, process_exchange_run(argv, &io));
    free(io.answer);
}

MOLTEST(process_exchange_refuses_an_answer_past_its_cap) {
    /* Refused mid-read rather than after: a document that does not fit is not
       going to be read whole, and reading it anyway is what the cap exists to
       prevent. */
    const char *const argv[] = {"sh", "-c", "yes molto", NULL};
    process_exchange io = {.request = NULL, .answer_max = 4096, .timeout_ms = 10000};

    ASSERT_EQ(process_exchange_too_large, process_exchange_run(argv, &io));
    free(io.answer);
}

MOLTEST(process_exchange_reports_a_command_that_is_not_there) {
    const char *const argv[] = {"molto-a-plugin-nobody-installed", NULL};
    process_exchange io = {.request = "{}", .timeout_ms = 5000};

    EXPECT_EQ(process_exchange_not_started, process_exchange_run(argv, &io));
    free(io.answer);
}

MOLTEST(process_exchange_closes_stdin_when_there_is_nothing_to_send) {
    /* A frontend asked for nothing still reads to EOF, so an empty request has
       to be a closed pipe and not an open one it would wait on forever. */
    const char *const argv[] = {"cat", NULL};
    process_exchange io = {.request = NULL, .timeout_ms = 5000};

    ASSERT_EQ(process_exchange_ok, process_exchange_run(argv, &io));
    EXPECT_EQ(0, io.code);
    EXPECT_EQ(0u, io.answer_size);
    free(io.answer);
}
