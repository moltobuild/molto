#include <molto/commands/test_command.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/process_service.h>
#include <molto/util/str_list.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>

/* process_run reports a signal death as 128 + signal (shell convention). */
#define SIGNAL_EXIT_BASE 128

/* Run one test binary and print its result, updating the pass/fail counters. */
static void run_one_test(const char *binary, size_t *passed, size_t *failed) {
    const char *argv[] = { binary, NULL };
    int status = process_run(argv);
    if (status == 0) {
        printf("  %s ... ok\n", binary);
        (*passed)++;
    } else if (status < 0) {
        printf("  %s ... FAILED (could not start)\n", binary);
        (*failed)++;
    } else if (status > SIGNAL_EXIT_BASE) {
        printf("  %s ... FAILED (signal %d)\n", binary, status - SIGNAL_EXIT_BASE);
        (*failed)++;
    } else {
        printf("  %s ... FAILED (exit %d)\n", binary, status);
        (*failed)++;
    }
}

int test_command_run(const char *requested_profile) {
    build_profile profile = profile_debug;
    if (requested_profile != NULL && !profile_parse(requested_profile, &profile)) {
        fprintf(stderr, "molto: unknown profile '%s'\n", requested_profile);
        return exit_usage_error;
    }

    char root[4096];
    if (!workspace_find_root(root, sizeof root)) {
        fprintf(stderr, "molto: not inside a molto workspace (no Project.toml found)\n");
        return exit_invalid_manifest;
    }
    str_list binaries;
    str_list_init(&binaries);
    int code = build_tests(root, profile, &binaries);
    if (code != exit_ok) {
        str_list_free(&binaries);
        return code;
    }

    size_t total = str_list_count(&binaries);
    if (total == 0) {
        printf("no tests found\n");
        str_list_free(&binaries);
        return exit_ok;
    }

    printf("running %zu test%s (%s)\n", total, total == 1 ? "" : "s",
           profile_name(profile));
    size_t passed = 0;
    size_t failed = 0;
    for (size_t i = 0; i < total; i++)
        run_one_test(str_list_get(&binaries, i), &passed, &failed);
    printf("%zu passed, %zu failed\n", passed, failed);

    str_list_free(&binaries);
    return failed == 0 ? exit_ok : exit_build_failure;
}
