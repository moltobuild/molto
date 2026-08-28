#include <molto/commands/run_command.h>

#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/exit_code.h>
#include <molto/project/project_ctx.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* process_run reports a signal death as 128 + signal (shell convention). */
#define SIGNAL_EXIT_BASE 128

/* What this project builds, when it is not something that runs. NULL for an
   executable, which is the only kind this command has anything to do. */
static const char *library_kind_name(artifact_kind artifact) {
    switch(artifact) {
    case artifact_static:
        return "static library";
    case artifact_shared:
        return "shared library";
    case artifact_source:
    case artifact_executable:
        break;
    }
    return NULL;
}

int run_command_run(const char *requested_profile, bool refresh_toolchain, size_t jobs,
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
    /* Read before anything is built. The program runs with the same [env] the
       build used, so a variable the project needs is there whether it is
       compiled or executed — and a project with nothing to run is told so
       before it spends a minute compiling something this command cannot use. */
    char manifest[4096];
    project_ctx ctx;
    char err[256] = "";
    if(!fs_format_path(manifest, sizeof manifest, "%s/Project.toml", root) ||
       !project_load(manifest, &ctx, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err[0] != '\0' ? err : "invalid manifest");
        return exit_invalid_manifest;
    }
    const char *library = library_kind_name(ctx.artifact);
    if(library != NULL) {
        fprintf(stderr,
                "molto: this project builds a %s and has nothing to run; "
                "`molto test` runs what exercises it\n",
                library);
        return exit_usage_error;
    }

    char binary[4096];
    build_report *report = build_report_create(stderr);
    int code =
        build_project_with(root, profile, refresh_toolchain, jobs, binary, sizeof binary, report);
    build_report_finish(report, profile_name(profile), code);
    build_report_destroy(report);
    if(code != exit_ok)
        return code;
    process_env_var vars[PROJECT_MAX_ENV];
    size_t var_count = project_env_to_vars(&ctx.env, vars, PROJECT_MAX_ENV);

    const char **argv = (const char **)malloc((size_t)(forwarded_count + 2) * sizeof(char *));
    if(argv == NULL)
        return exit_build_failure;
    argv[0] = binary;
    for(int i = 0; i < forwarded_count; i++)
        argv[i + 1] = forwarded[i];
    argv[forwarded_count + 1] = NULL;

    fprintf(stderr, "Running %s\n", binary);
    int status = process_run_env(argv, vars, var_count);
    free((void *)argv);

    if(status < 0) {
        /* The program could not be started at all (should not happen after a
           successful build, but report it honestly if it does). */
        fprintf(stderr, "molto: failed to start '%s'\n", binary);
        return exit_build_failure;
    }
    if(status > SIGNAL_EXIT_BASE) {
        /* The program started but was killed by a signal (e.g. a crash). */
        int signal_number = status - SIGNAL_EXIT_BASE;
        fprintf(stderr, "molto: '%s' terminated by signal %d (%s)\n", binary, signal_number,
                strsignal(signal_number));
    }
    return status;
}
