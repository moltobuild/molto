#include "test_framework.h"
#include "tests.h"

#include <molto/cli.h>

#include <string.h>

void suite_cli(void) {
    CHECK(strlen(cli_version()) > 0);

    char *argv_new[] = { "molto", "new", "demo" };
    cli_invocation new_inv = cli_parse(3, argv_new);
    CHECK(new_inv.command == cli_cmd_new);
    CHECK(new_inv.arg != NULL && strcmp(new_inv.arg, "demo") == 0);

    char *argv_help[] = { "molto", "--help" };
    CHECK(cli_parse(2, argv_help).command == cli_cmd_help);

    char *argv_help_short[] = { "molto", "-h" };
    CHECK(cli_parse(2, argv_help_short).command == cli_cmd_help);

    char *argv_version[] = { "molto", "--version" };
    CHECK(cli_parse(2, argv_version).command == cli_cmd_version);

    char *argv_none[] = { "molto" };
    CHECK(cli_parse(1, argv_none).command == cli_cmd_none);

    char *argv_unknown[] = { "molto", "frobnicate" };
    CHECK(cli_parse(2, argv_unknown).command == cli_cmd_unknown);

    char *argv_build[] = { "molto", "build" };
    CHECK(cli_parse(2, argv_build).command == cli_cmd_build);

    /* Option value extraction. */
    char *argv_profile[] = { "molto", "build", "--profile", "release" };
    const char *value = cli_option_value(4, argv_profile, "--profile");
    CHECK(value != NULL && strcmp(value, "release") == 0);

    CHECK(cli_option_value(2, argv_build, "--profile") == NULL);

    char *argv_dangling[] = { "molto", "build", "--profile" };
    CHECK(cli_option_value(3, argv_dangling, "--profile") == NULL);

    /* cli_option_value stops at "--": flags after it belong to the program. */
    char *argv_after_sep[] = { "molto", "run", "--", "--profile", "release" };
    CHECK(cli_option_value(5, argv_after_sep, "--profile") == NULL);

    /* Forwarded arguments after "--". */
    int forwarded_count = -1;
    char **forwarded = cli_forwarded_args(5, argv_after_sep, &forwarded_count);
    CHECK(forwarded_count == 2);
    CHECK(forwarded != NULL && strcmp(forwarded[0], "--profile") == 0);
    CHECK(strcmp(forwarded[1], "release") == 0);

    char *argv_no_sep[] = { "molto", "run" };
    forwarded_count = -1;
    CHECK(cli_forwarded_args(2, argv_no_sep, &forwarded_count) == NULL);
    CHECK(forwarded_count == 0);

    char *argv_trailing_sep[] = { "molto", "run", "--" };
    forwarded_count = -1;
    forwarded = cli_forwarded_args(3, argv_trailing_sep, &forwarded_count);
    CHECK(forwarded_count == 0);
    CHECK(forwarded == &argv_trailing_sep[3]); /* points past the end, count 0 */
}
