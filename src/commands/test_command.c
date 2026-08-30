#include <molto/commands/test_command.h>

#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/exit_code.h>
#include <molto/project/project_ctx.h>
#include <molto/services/build_service.h>
#include <molto/services/process_service.h>
#include <molto/util/str_list.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>

/* process_run reports a signal death as 128 + signal (shell convention). */
#define SIGNAL_EXIT_BASE 128

/* Run one test binary and print its result, updating the pass/fail counters.
   The binary runs in the [env] it was built in: a variable the manifest sets
   for the compiler is one the code may well read at runtime too, and a test is
   the first place that would be noticed. */
static void run_one_test(const char *binary, const project_env *env, size_t *passed,
                         size_t *failed) {
    const char *argv[] = {binary, NULL};
    process_env_var vars[PROJECT_MAX_ENV];
    size_t var_count = project_env_to_vars(env, vars, PROJECT_MAX_ENV);
    int status = process_run_env(argv, vars, var_count);
    if(status == 0) {
        printf("  %s ... ok\n", binary);
        (*passed)++;
    } else if(status < 0) {
        printf("  %s ... FAILED (could not start)\n", binary);
        (*failed)++;
    } else if(status > SIGNAL_EXIT_BASE) {
        printf("  %s ... FAILED (signal %d)\n", binary, status - SIGNAL_EXIT_BASE);
        (*failed)++;
    } else {
        printf("  %s ... FAILED (exit %d)\n", binary, status);
        (*failed)++;
    }
}

int test_command_run(const char *requested_profile, bool refresh_toolchain, size_t jobs) {
    build_profile profile = profile_debug;
    if(requested_profile != NULL && !profile_parse(requested_profile, &profile)) {
        fprintf(stderr, "molto: unknown profile '%s'\n", requested_profile);
        return exit_usage_error;
    }

    char root[4096];
    if(!workspace_find_root(root, sizeof root)) {
        fprintf(stderr, "molto: not inside a molto workspace (no Project.toml found)\n");
        return exit_invalid_manifest;
    }
    str_list binaries;
    str_list_init(&binaries);
    project_env env;
    build_report *report = build_report_create(stderr);
    int code =
        build_tests_with(root, profile, NULL, refresh_toolchain, jobs, &binaries, &env, report);
    build_report_finish(report, profile_name(profile), code);
    build_report_destroy(report);
    if(code != exit_ok) {
        str_list_free(&binaries);
        return code;
    }

    size_t total = str_list_count(&binaries);
    if(total == 0) {
        printf("no tests found\n");
        str_list_free(&binaries);
        return exit_ok;
    }

    printf("running %zu test%s (%s)\n", total, total == 1 ? "" : "s", profile_name(profile));
    size_t passed = 0;
    size_t failed = 0;
    for(size_t i = 0; i < total; i++)
        run_one_test(str_list_get(&binaries, i), &env, &passed, &failed);
    printf("%zu passed, %zu failed\n", passed, failed);

    str_list_free(&binaries);
    return failed == 0 ? exit_ok : exit_build_failure;
}
