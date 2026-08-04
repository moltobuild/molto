#include <molto/commands/fmt_command.h>

#include <molto/exit_code.h>
#include <molto/services/fmt_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>

/* Size of the buffer holding the discovered workspace root. */
#define ROOT_BUFFER_SIZE 4096

/* Report what happened on stderr, so `molto fmt --diff > patch` yields the
   diff and nothing else. */
static void report(const fmt_result *result, fmt_mode mode) {
    size_t changed = str_list_count(&result->changed);
    if(mode == fmt_mode_write) {
        fprintf(stderr, "%zu file%s formatted\n", changed, changed == 1 ? "" : "s");
        return;
    }
    fprintf(stderr, "%zu file%s would change\n", changed, changed == 1 ? "" : "s");
}

int fmt_command_run(bool check, bool diff, bool refresh_tools, bool refresh_analysis) {
    if(check && diff) {
        fprintf(stderr, "molto: --check and --diff are two answers to the same "
                        "question; pick one\n");
        return exit_usage_error;
    }

    char root[ROOT_BUFFER_SIZE];
    if(!workspace_find_root(root, sizeof root)) {
        fprintf(stderr, "molto: no Project.toml found in this directory or above\n");
        return exit_invalid_manifest;
    }

    fmt_mode mode = check ? fmt_mode_check : (diff ? fmt_mode_diff : fmt_mode_write);
    const fmt_request request = {
        .mode = mode,
        .refresh_tools = refresh_tools,
        .refresh_analysis = refresh_analysis,
        .diff_stream = mode == fmt_mode_diff ? stdout : NULL,
    };

    fmt_result result;
    fmt_result_init(&result);
    int code = fmt_project(root, &request, &result);
    if(code == exit_ok) {
        /* Whatever the formatter had to say, in Molto's own shape. */
        diagnostic_write_text(stdout, &result.diagnostics, root);
        report(&result, mode);
        /* Writing is not a failure; reporting a file that would change is. */
        if(mode != fmt_mode_write && str_list_count(&result.changed) > 0)
            code = exit_build_failure;
    }
    fmt_result_free(&result);
    return code;
}
