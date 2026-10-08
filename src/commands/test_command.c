#include <molto/commands/test_command.h>

#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/exit_code.h>
#include <molto/project/project_ctx.h>
#include <molto/services/build_service.h>
#include <molto/services/process_service.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/str_list.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* process_run reports a signal death as 128 + signal (shell convention). */
#define SIGNAL_EXIT_BASE 128

/* Room for the variables of any program the build produced, plus the two that
   place a test binary in its run. */
#define TEST_RUN_MAX_VARS (PROJECT_RUN_MAX_VARS + 2)

/* Put the binary's place in the run (RFC-0020) in place of whatever [env] said
   about it. Dropped rather than kept beside ours: a child given one name twice
   gets whichever its platform picks, and a position the manifest could fake
   would have a plugin judge part of a run as the whole of it. `vars` needs room
   for two more than `count`. */
static size_t place_in_run(process_env_var *vars, size_t count, const char *index,
                           const char *total) {
    size_t kept = 0;
    for(size_t i = 0; i < count; i++) {
        const char *name = vars[i].name;
        if(name == NULL ||
           (strcmp(name, TEST_RUN_INDEX_VAR) != 0 && strcmp(name, TEST_RUN_COUNT_VAR) != 0))
            vars[kept++] = vars[i];
    }
    vars[kept++] = (process_env_var){.name = TEST_RUN_INDEX_VAR, .value = index};
    vars[kept++] = (process_env_var){.name = TEST_RUN_COUNT_VAR, .value = total};
    return kept;
}

/* Run one test binary and print its result, updating the pass/fail counters.
   The binary runs in the [env] it was built in: a variable the manifest sets
   for the compiler is one the code may well read at runtime too, and a test is
   the first place that would be noticed. It runs with the resolved toolchain's
   runtime directories on the loader path for the same reason — a test linked
   against the compiler's own shared libraries is exactly where that would be
   noticed first, as "could not start". It is also told where it stands in the
   run, `index` of `total`. */
static void run_one_test(const char *binary, const project_env *env,
                         const resolved_toolchain *chain, const char *const *argv, size_t index,
                         size_t total, size_t *passed, size_t *failed) {
    process_env_var vars[TEST_RUN_MAX_VARS];
    char runtime_path[TOOLCHAIN_RUNTIME_PATH_MAX];
    size_t var_count =
        project_run_vars(env, chain, vars, PROJECT_RUN_MAX_VARS, runtime_path, sizeof runtime_path);
    char index_text[24];
    char total_text[24];
    (void)snprintf(index_text, sizeof index_text, "%zu", index);
    (void)snprintf(total_text, sizeof total_text, "%zu", total);
    var_count = place_in_run(vars, var_count, index_text, total_text);
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

int test_command_run(const char *requested_profile, bool refresh_toolchain, size_t jobs,
                     char *const *forwarded, int forwarded_count) {
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
    resolved_toolchain chain;
    build_report *report = build_report_create(stderr);
    int code = build_tests_with(root, profile, NULL, refresh_toolchain, jobs, &binaries, &env,
                                &chain, report);
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

    /* One argv for every binary: its own path first, then what came after `--`.
       The same arguments go to each, which is what a single-mode suite wants
       and what a per-file one at least does not mind. */
    const char **argv = (const char **)calloc((size_t)forwarded_count + 2, sizeof *argv);
    if(argv == NULL) {
        str_list_free(&binaries);
        return exit_build_failure;
    }
    for(int i = 0; i < forwarded_count; i++)
        argv[i + 1] = forwarded[i];

    printf("running %zu test%s (%s)\n", total, total == 1 ? "" : "s", profile_name(profile));
    size_t passed = 0;
    size_t failed = 0;
    /* One at a time, in the order printed, and a failure does not stop the
       rest. RFC-0020 promises the first binary runs alone before any other and
       the last alone after every other; running them all in turn keeps both. */
    for(size_t i = 0; i < total; i++) {
        argv[0] = str_list_get(&binaries, i);
        run_one_test(argv[0], &env, &chain, argv, i + 1, total, &passed, &failed);
    }
    printf("%zu passed, %zu failed\n", passed, failed);

    free((void *)argv);
    str_list_free(&binaries);
    return failed == 0 ? exit_ok : exit_build_failure;
}
