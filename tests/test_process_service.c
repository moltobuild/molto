#include "test_framework.h"
#include "tests.h"

#include <molto/services/process_service.h>

void suite_process_service(void) {
    const char *ok[] = { "true", NULL };
    CHECK(process_run(ok) == 0);

    const char *fail[] = { "false", NULL };
    CHECK(process_run(fail) != 0);

    /* A command that cannot be found: execvp fails and the child exits 127. */
    const char *missing[] = { "molto_no_such_command_zzz", NULL };
    int code = process_run(missing);
    CHECK(code == 127 || code == -1);
}
