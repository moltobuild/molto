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
