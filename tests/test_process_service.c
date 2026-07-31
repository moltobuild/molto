#include <moltest.h>

#include <molto/services/process_service.h>

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
