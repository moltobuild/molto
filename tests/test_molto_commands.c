#include <moltest.h>

#include <molto/cli.h>
#include <molto/exit_code.h>

#include <stdio.h>

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
