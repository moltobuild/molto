#include <moltest.h>

#include <molto/commands/fmt_command.h>
#include <molto/commands/lint_command.h>
#include <molto/exit_code.h>
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

    EXPECT_EQ(exit_invalid_manifest, lint_command_run(NULL, false, false, NULL));

    workspace_leave(&ws);
}

MOLTEST(fmt_outside_a_workspace_is_a_manifest_error) {
    workspace ws;
    ASSERT_TRUE(workspace_enter(&ws, false));

    EXPECT_EQ(exit_invalid_manifest, fmt_command_run(false, false, false));

    workspace_leave(&ws);
}

MOLTEST(lint_rejects_an_unknown_profile) {
    /* Validated before the workspace is even looked for: a typo in a flag is
       the user's mistake, not the project's. */
    EXPECT_EQ(exit_usage_error, lint_command_run("relase", false, false, NULL));
}

MOLTEST(lint_rejects_an_unknown_output_format) {
    EXPECT_EQ(exit_usage_error, lint_command_run(NULL, false, false, "yaml"));
}

MOLTEST(lint_accepts_the_formats_it_documents) {
    workspace ws;
    ASSERT_TRUE(workspace_enter(&ws, true));

    /* An empty project has nothing to analyse, which is success, not an error
       — and it proves the two formats are accepted rather than rejected. */
    EXPECT_EQ(exit_ok, lint_command_run(NULL, false, false, "text"));
    EXPECT_EQ(exit_ok, lint_command_run(NULL, false, false, "json"));
    EXPECT_EQ(exit_ok, lint_command_run("release", false, false, NULL));

    workspace_leave(&ws);
}

MOLTEST(fmt_rejects_check_and_diff_together) {
    /* Two answers to the same question: which one was meant is not guessable. */
    EXPECT_EQ(exit_usage_error, fmt_command_run(true, true, false));
}
