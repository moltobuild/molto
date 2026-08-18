#include <molto/commands/lint_command.h>

#include <molto/build/diagnostic_view.h>
#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/lint_service.h>
#include <molto/util/progress.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Size of the buffer holding the discovered workspace root. */
#define ROOT_BUFFER_SIZE 4096

/* The shapes `--format` accepts.
 *
 * `text` is one normalized line per finding, which is what a file another
 * program will read wants (RFC-0005). `rich` is the same findings drawn in the
 * frame a build draws them in, which is what a person at a terminal wants.
 * `json` is the contract CI reads, and neither of the other two moves it. */
#define FORMAT_TEXT "text"
#define FORMAT_JSON "json"
#define FORMAT_RICH "rich"

typedef enum { lint_format_text, lint_format_rich, lint_format_json } lint_format;

static bool parse_format(const char *format, lint_format *out) {
    if(format == NULL) {
        /* Unasked: a person at a terminal gets the frame, and anything being
           redirected gets the line another program can read. `molto lint >
           report` and every CI job that never passed the flag keep the output
           they have always had. */
        *out = progress_is_interactive(stdout) ? lint_format_rich : lint_format_text;
        return true;
    }
    if(strcmp(format, FORMAT_TEXT) == 0) {
        *out = lint_format_text;
        return true;
    }
    if(strcmp(format, FORMAT_RICH) == 0) {
        *out = lint_format_rich;
        return true;
    }
    if(strcmp(format, FORMAT_JSON) == 0) {
        *out = lint_format_json;
        return true;
    }
    return false;
}

/* Every finding, framed, one block per file. Which findings belong to which
   file is the view's own reading of them, since it is the view that knows what
   an entry carrying no file of its own is doing there. */
static void write_rich(FILE *out, const diagnostic_list *found, const char *root, bool colour) {
    const diagnostic_context ctx = {
        .action = diagnostic_view_checking,
        .root = root,
    };
    char *block = diagnostic_view_render_by_file(found, &ctx, colour);
    if(block == NULL)
        return;
    (void)fputs(block, out);
    free(block);
}

int lint_command_run(const char *requested_profile, bool refresh_toolchain, bool refresh_tools,
                     bool refresh_analysis, const char *format, size_t jobs) {
    build_profile profile = profile_debug;
    if(requested_profile != NULL && !profile_parse(requested_profile, &profile)) {
        fprintf(stderr,
                "molto: unknown profile '%s' "
                "(debug, release, bench, custom)\n",
                requested_profile);
        return exit_usage_error;
    }

    lint_format shape = lint_format_text;
    if(!parse_format(format, &shape)) {
        fprintf(stderr, "molto: unknown output format '%s' (text, rich, json)\n", format);
        return exit_usage_error;
    }
    const bool as_json = shape == lint_format_json;

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
        .refresh_analysis = refresh_analysis,
        .jobs = jobs,
        .progress = !as_json,
    };

    diagnostic_list found;
    diagnostic_list_init(&found);
    int code = lint_project(root, &request, &found);
    if(code == exit_ok) {
        if(shape == lint_format_json)
            diagnostic_write_json(stdout, &found, root);
        else if(shape == lint_format_rich)
            write_rich(stdout, &found, root,
                       progress_is_interactive(stdout) && getenv("NO_COLOR") == NULL);
        else
            diagnostic_write_text(stdout, &found, root);

        /* The summary below goes to stderr while the diagnostics went to stdout.
           A terminal interleaves them correctly because stdout is line buffered
           there; a pipe does not, and the summary lands in the middle of a
           diagnostic. Flush before writing to the other stream. */
        fflush(stdout);

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
