#include <moltest.h>

#include <molto/cli.h>
#include <molto/commands/metadata_command.h>
#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/util/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Exercises molto's real command table, as opposed to test_cli.c, which
   exercises the CLI framework with a synthetic app. */
static int run_molto(const char *command) {
    char program[] = "molto";
    char argument[32];
    snprintf(argument, sizeof argument, "%s", command);
    char *argv[] = { program, argument };
    return cli_run(2, argv);
}

MOLTEST(molto_reports_commands_that_are_not_implemented_yet) {
    /* Declared in the CLI but with no implementation behind them. They used to
       return 1, the code for "the build failed", which no script could tell
       apart from a real compilation error. */
    /* `lint` and `fmt` are deliberately not exercised here: this runs in the
       harness's working directory, which is Molto's own workspace, so a case
       for either would analyse the whole codebase inside a unit test. They are
       covered in test_lint_service.c and test_fmt_service.c, which chdir into
       a temporary workspace first. */
    /* `login` and `publish` are not here any more: they are implemented, and
       neither is exercised in this file because both would reach a registry. */
    /* Nor are `add` and `remove`, which now edit the manifest. They are
       covered in test_manifest_edit.c, which works on a temporary one — a case
       here would rewrite Molto's own. */
    static const char *pending[] = {
        "bench", "update", "migrate",
    };
    for (size_t i = 0; i < sizeof pending / sizeof pending[0]; i++)
        EXPECT_EQ(exit_not_implemented, run_molto(pending[i]));
}

MOLTEST(molto_rejects_an_unknown_command) {
    EXPECT_EQ(exit_usage_error, run_molto("frobnicate"));
}

MOLTEST(molto_answers_help_and_version) {
    EXPECT_EQ(exit_ok, run_molto("--help"));
    EXPECT_EQ(exit_ok, run_molto("--version"));
    EXPECT_NOT_NULL(cli_version());
}

MOLTEST(molto_without_a_command_prints_help) {
    /* Bare `molto` shows the command list and succeeds; only an unusable
       invocation (unknown command, bad option) is a usage error. */
    char program[] = "molto";
    char *argv[] = { program };
    EXPECT_EQ(exit_ok, cli_run(1, argv));
}

MOLTEST(molto_metadata_writes_a_bill_of_materials) {
    /* Driven through metadata_command_run rather than the argv table, for the
       reason `lint` and `fmt` are left out above: this runs in Molto's own
       workspace, and the document belongs in a temporary file rather than in
       the middle of the test output.
     *
     * Molto declares no dependencies, so this resolves an empty graph and
     * reaches no network — and an empty graph is the case an emitter is most
     * likely to turn into a broken array, which is why it is worth running end
     * to end and not only in test_sbom_cyclonedx.c. */
    char path[] = "/tmp/molto_bom_XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    EXPECT_EQ(exit_ok, metadata_command_run(path, false));

    char *text = fs_read_file(path);
    ASSERT_NOT_NULL(text);
    json_document *parsed = json_parse(text);
    ASSERT_NOT_NULL(parsed);

    const json_value root = json_root(parsed);
    EXPECT_STREQ("CycloneDX", json_string(json_get(root, "bomFormat")));
    /* Whatever workspace this ran in, the document is about a package. */
    const json_value self = json_get(json_get(root, "metadata"), "component");
    EXPECT_NOT_NULL(json_string(json_get(self, "name")));

    json_free(parsed);
    free(text);
    remove(path);
}

MOLTEST(molto_metadata_refuses_a_file_it_cannot_write) {
    EXPECT_EQ(exit_build_failure, metadata_command_run("/nonexistent_dir/sbom.json", false));
}
