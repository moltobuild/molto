#include <molto/commands/build_command.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>

int build_command_run(const char *requested_profile) {
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
    const char *label = profile_name(profile);
    printf("Compiling (%s)\n", label);
    int code = build_project(root, profile, NULL, 0);
    if (code == exit_ok)
        printf("Finished %s -> build/%s\n", label, label);
    return code;
}
