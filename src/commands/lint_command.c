#include <molto/commands/lint_command.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/lint_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <string.h>

/* Size of the buffer holding the discovered workspace root. */
#define ROOT_BUFFER_SIZE 4096

/* The shapes `--format` accepts. */
#define FORMAT_TEXT "text"
#define FORMAT_JSON "json"

static bool parse_format(const char *format, bool *as_json) {
    if(format == NULL || strcmp(format, FORMAT_TEXT) == 0) {
        *as_json = false;
        return true;
    }
    if(strcmp(format, FORMAT_JSON) == 0) {
        *as_json = true;
        return true;
    }
    return false;
}

int lint_command_run(const char *requested_profile, bool refresh_toolchain, bool refresh_tools,
                     const char *format) {
    build_profile profile = profile_debug;
    if(requested_profile != NULL && !profile_parse(requested_profile, &profile)) {
        fprintf(stderr,
                "molto: unknown profile '%s' "
                "(debug, release, bench, custom)\n",
                requested_profile);
        return exit_usage_error;
    }

    bool as_json = false;
    if(!parse_format(format, &as_json)) {
        fprintf(stderr, "molto: unknown output format '%s' (text, json)\n", format);
        return exit_usage_error;
    }

    char root[ROOT_BUFFER_SIZE];
    if(!workspace_find_root(root, sizeof root)) {
        fprintf(stderr, "molto: no Project.toml found in this directory or above\n");
        return exit_invalid_manifest;
    }

    /* Progress on stderr, so `molto lint > report` yields the diagnostics and
       nothing else, and `--format json` leaves stdout a parseable document. */
    if(!as_json)
        fprintf(stderr, "Checking (%s)\n", profile_name(profile));

    const lint_request request = {
        .profile = profile,
        .refresh_toolchain = refresh_toolchain,
        .refresh_tools = refresh_tools,
    };

    diagnostic_list found;
    diagnostic_list_init(&found);
    int code = lint_project(root, &request, &found);
    if(code == exit_ok) {
        if(as_json)
            diagnostic_write_json(stdout, &found, root);
        else
            diagnostic_write_text(stdout, &found, root);

        size_t errors = diagnostic_count_severity(&found, diagnostic_severity_error);
        size_t warnings = diagnostic_count_severity(&found, diagnostic_severity_warning);
        if(!as_json)
            fprintf(stderr, "%zu error%s, %zu warning%s\n", errors, errors == 1 ? "" : "s",
                    warnings, warnings == 1 ? "" : "s");
        /* Only an error fails the command; a warning reports and succeeds. */
        code = errors > 0 ? exit_build_failure : exit_ok;
    }
    diagnostic_list_free(&found);
    return code;
}
