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
}
