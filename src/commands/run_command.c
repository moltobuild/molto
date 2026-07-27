#include <molto/commands/run_command.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/process_service.h>

#include <stdio.h>
#include <stdlib.h>

int run_command_run(const char *requested_profile,
                    char *const *forwarded, int forwarded_count) {
    build_profile profile = profile_debug;
    if (requested_profile != NULL && !profile_parse(requested_profile, &profile)) {
        fprintf(stderr, "molto: unknown profile '%s'\n", requested_profile);
        return exit_usage_error;
    }

    int code = build_project(".", profile);
    if (code != exit_ok)
        return code;

    char binary[4096];
    if (!build_binary_path(".", profile, binary, sizeof binary)) {
        fprintf(stderr, "molto: could not resolve target binary\n");
        return exit_invalid_manifest;
    }

    const char **argv = malloc((size_t)(forwarded_count + 2) * sizeof(char *));
    if (argv == NULL)
        return exit_build_failure;
    argv[0] = binary;
    for (int i = 0; i < forwarded_count; i++)
        argv[i + 1] = forwarded[i];
    argv[forwarded_count + 1] = NULL;

    fprintf(stderr, "Running %s\n", binary);
    int status = process_run(argv);
    free(argv);

    if (status < 0) {
        fprintf(stderr, "molto: failed to run '%s'\n", binary);
        return exit_build_failure;
    }
    return status;
}
