#include <moltest.h>

#include <molto/commands/fmt_command.h>
#include <molto/commands/lint_command.h>
#include <molto/exit_code.h>
#include <molto/services/fmt_service.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The commands find their workspace by walking up from the working directory,
   so these enter a temporary one rather than running where the harness sits —
   which is Molto's own workspace, and would analyse the whole codebase. */
typedef struct {
    char root[64];
    char previous[4096];
} workspace;

static bool workspace_enter(workspace *ws, bool with_manifest) {
    if (getcwd(ws->previous, sizeof ws->previous) == NULL)
        return false;
    snprintf(ws->root, sizeof ws->root, "%s", "/tmp/molto_style_cmd_XXXXXX");
    if (mkdtemp(ws->root) == NULL)
        return false;

    if (with_manifest) {
        char manifest[128];
        snprintf(manifest, sizeof manifest, "%s/Project.toml", ws->root);
        if (!fs_write_file(manifest, "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n"))
            return false;
    }
    return chdir(ws->root) == 0;
}

static void workspace_leave(workspace *ws) {
    (void)chdir(ws->previous);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", ws->root);
    (void)system(cmd);
}

MOLTEST(lint_outside_a_workspace_is_a_manifest_error) {
    workspace ws;
    ASSERT_TRUE(workspace_enter(&ws, false));

    EXPECT_EQ(exit_invalid_manifest, lint_command_run(NULL, false, false, false, NULL));

    workspace_leave(&ws);
}

MOLTEST(fmt_outside_a_workspace_is_a_manifest_error) {
    workspace ws;
    ASSERT_TRUE(workspace_enter(&ws, false));

    EXPECT_EQ(exit_invalid_manifest, fmt_command_run(false, false, false, false));

    workspace_leave(&ws);
}

MOLTEST(lint_rejects_an_unknown_profile) {
    /* Validated before the workspace is even looked for: a typo in a flag is
       the user's mistake, not the project's. */
    EXPECT_EQ(exit_usage_error, lint_command_run("relase", false, false, false, NULL));
}

MOLTEST(lint_rejects_an_unknown_output_format) {
    EXPECT_EQ(exit_usage_error, lint_command_run(NULL, false, false, false, "yaml"));
}

MOLTEST(lint_accepts_the_formats_it_documents) {
    workspace ws;
    ASSERT_TRUE(workspace_enter(&ws, true));

    /* An empty project has nothing to analyse, which is success, not an error
       — and it proves the two formats are accepted rather than rejected. */
    EXPECT_EQ(exit_ok, lint_command_run(NULL, false, false, false, "text"));
    EXPECT_EQ(exit_ok, lint_command_run(NULL, false, false, false, "json"));
    EXPECT_EQ(exit_ok, lint_command_run("release", false, false, false, NULL));

    workspace_leave(&ws);
}

MOLTEST(fmt_rejects_check_and_diff_together) {
    /* Two answers to the same question: which one was meant is not guessable. */
    EXPECT_EQ(exit_usage_error, fmt_command_run(true, true, false, false));
}

/* Format `root` in write mode and report how many files it rewrote, or -1 when
   this machine has no formatter to run. */
static int rewritten_count(const char *root) {
    fmt_request request;
    memset(&request, 0, sizeof request);
    request.mode = fmt_mode_write;

    fmt_result result;
    fmt_result_init(&result);
    int code = fmt_project(root, &request, &result);
    int count = code == exit_ok ? (int)str_list_count(&result.changed) : -1;
    fmt_result_free(&result);
    return count;
}

MOLTEST(fmt_counts_the_files_it_rewrote_and_not_the_ones_it_left) {
    workspace ws;
    ASSERT_TRUE(workspace_enter(&ws, true));

    char source[128];
    snprintf(source, sizeof source, "%s/src", ws.root);
    ASSERT_TRUE(fs_make_dirs(source));
    snprintf(source, sizeof source, "%s/src/main.c", ws.root);
    ASSERT_TRUE(fs_write_file(source, "int main(void){\nint    x=1;\n   return x;\n}\n"));

    int first = rewritten_count(ws.root);
    if(first < 0) {
        workspace_leave(&ws);
        SKIP("this machine has no formatter");
    }

    /* --in-place exits zero whether or not it changed anything, so a count
       taken from the exit status reported nothing at all. */
    EXPECT_EQ(1, first);

    /* And the second pass is the half that a fixed answer would also pass:
       nothing changed, so nothing is counted. */
    EXPECT_EQ(0, rewritten_count(ws.root));

    workspace_leave(&ws);
}

/* --- the result cache (RFC-0006) --- */

/* Format `root` in `mode` and report how many files the tool was run for, by
   counting what it had to look at rather than what it changed. -1 when this
   machine has no formatter. */
static int formatted_count(const char *root, fmt_mode mode, bool refresh_analysis) {
    fmt_request request;
    memset(&request, 0, sizeof request);
    request.mode = mode;
    request.refresh_analysis = refresh_analysis;
    if (mode == fmt_mode_diff)
        request.diff_stream = fopen("/dev/null", "w");

    fmt_result result;
    fmt_result_init(&result);
    int code = fmt_project(root, &request, &result);
    int count = code == exit_ok ? (int)str_list_count(&result.changed) : -1;
    fmt_result_free(&result);
    if (request.diff_stream != NULL)
        fclose(request.diff_stream);
    return count;
}

/* A workspace with one file that needs formatting and one that does not. */
static bool workspace_with_sources(workspace *ws) {
    if (!workspace_enter(ws, true))
        return false;
    char path[128];
    snprintf(path, sizeof path, "%s/src", ws->root);
    if (!fs_make_dirs(path))
        return false;
    snprintf(path, sizeof path, "%s/src/messy.c", ws->root);
    if (!fs_write_file(path, "int messy(void){\nint    x=1;\n   return x;\n}\n"))
        return false;
    snprintf(path, sizeof path, "%s/src/tidy.c", ws->root);
    return fs_write_file(path, "int tidy(void) { return 0; }\n");
}

MOLTEST(fmt_does_not_run_the_formatter_for_a_file_it_already_formatted) {
    workspace ws;
    ASSERT_TRUE(workspace_with_sources(&ws));

    int first = formatted_count(ws.root, fmt_mode_write, false);
    if (first < 0) {
        workspace_leave(&ws);
        SKIP("this machine has no formatter");
    }
    EXPECT_EQ(1, first);

    /* Formatting recorded that both files are in their final form, so a check
       right after has nothing to run — the entry does not name the mode,
       because "this file is formatted" is the same fact all three modes are
       after. */
    EXPECT_EQ(0, formatted_count(ws.root, fmt_mode_check, false));
    EXPECT_EQ(0, formatted_count(ws.root, fmt_mode_diff, false));

    workspace_leave(&ws);
}

MOLTEST(fmt_still_sees_a_file_that_changed_after_it_was_recorded) {
    workspace ws;
    ASSERT_TRUE(workspace_with_sources(&ws));

    if (formatted_count(ws.root, fmt_mode_write, false) < 0) {
        workspace_leave(&ws);
        SKIP("this machine has no formatter");
    }
    EXPECT_EQ(0, formatted_count(ws.root, fmt_mode_check, false));

    /* The failure this must never have: a cache that lets --check pass over a
       file that is not formatted would be a green CI on unformatted code. */
    char path[128];
    snprintf(path, sizeof path, "%s/src/tidy.c", ws.root);
    ASSERT_TRUE(fs_write_file(path, "int tidy(void){return    0;}\n"));
    EXPECT_EQ(1, formatted_count(ws.root, fmt_mode_check, false));

    workspace_leave(&ws);
}

MOLTEST(fmt_formats_everything_again_when_the_style_changes) {
    workspace ws;
    ASSERT_TRUE(workspace_with_sources(&ws));

    if (formatted_count(ws.root, fmt_mode_write, false) < 0) {
        workspace_leave(&ws);
        SKIP("this machine has no formatter");
    }

    /* A narrower column is a different style, and every file has to be judged
       against it — the path of the translated configuration does not change
       when format.json does, which is why its content is in the fingerprint. */
    char path[128];
    snprintf(path, sizeof path, "%s/format.json", ws.root);
    ASSERT_TRUE(fs_write_file(path, "{\n    \"style\": { \"line_width\": 20 }\n}\n"));
    EXPECT_EQ(1, formatted_count(ws.root, fmt_mode_write, false));

    workspace_leave(&ws);
}
