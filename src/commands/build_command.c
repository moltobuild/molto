#include <molto/commands/build_command.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>

int build_command_run(const char *requested_profile, bool refresh_toolchain, size_t jobs) {
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
    /* Progress goes to stderr, next to the diagnostics it is interleaved with.
       On stdout it would be buffered separately and could surface after the
       error it was supposed to precede — and it would also pollute the output
       of anyone redirecting a build. */
    const char *label = profile_name(profile);
    fprintf(stderr, "Compiling (%s)\n", label);
    int code = build_project(root, profile, refresh_toolchain, jobs, NULL, 0);
    if(code == exit_ok)
        fprintf(stderr, "Finished %s -> build/%s\n", label, label);
    return code;
}
