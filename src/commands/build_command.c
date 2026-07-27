#include <molto/commands/build_command.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>

#include <stdio.h>

int build_command_run(const char *requested_profile) {
    build_profile profile = profile_debug;
    if (requested_profile != NULL && !profile_parse(requested_profile, &profile)) {
        fprintf(stderr, "molto: unknown profile '%s'\n", requested_profile);
        return exit_usage_error;
    }
    const char *label = profile_name(profile);
    printf("Compiling (%s)\n", label);
    int code = build_project(".", profile, NULL, 0);
    if (code == exit_ok)
        printf("Finished %s -> build/%s\n", label, label);
    return code;
}
