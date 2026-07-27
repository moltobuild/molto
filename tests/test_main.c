#include "test_framework.h"
#include "tests.h"

int tests_run = 0;
int tests_failed = 0;

int main(void) {
    printf("Running molto tests\n");
    RUN_SUITE(suite_str_list);
    RUN_SUITE(suite_manifest_service);
    RUN_SUITE(suite_profile);
    RUN_SUITE(suite_source_discovery);
    RUN_SUITE(suite_process_service);
    RUN_SUITE(suite_cli);
    RUN_SUITE(suite_build_service);
    RUN_SUITE(suite_run_command);
    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
