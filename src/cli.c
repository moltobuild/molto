#include <molto/cli.h>

#include <molto/commands/build_command.h>
#include <molto/commands/init_command.h>
#include <molto/commands/new_command.h>
#include <molto/exit_code.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MOLTO_VERSION "0.1.0"

const char *cli_version(void) {
    return MOLTO_VERSION;
}

typedef struct {
    const char *token;
    cli_command command;
} command_entry;

static const command_entry command_table[] = {
    { "new",     cli_cmd_new },
    { "init",    cli_cmd_init },
    { "build",   cli_cmd_build },
    { "run",     cli_cmd_run },
    { "test",    cli_cmd_test },
    { "bench",   cli_cmd_bench },
    { "lint",    cli_cmd_lint },
    { "add",     cli_cmd_add },
    { "remove",  cli_cmd_remove },
    { "publish", cli_cmd_publish },
    { "update",  cli_cmd_update },
    { "migrate", cli_cmd_migrate },
};

static cli_command lookup_command(const char *token) {
    size_t count = sizeof command_table / sizeof command_table[0];
    for (size_t i = 0; i < count; i++) {
        if (strcmp(token, command_table[i].token) == 0)
            return command_table[i].command;
    }
    return cli_cmd_unknown;
}

static bool matches_flag(const char *token, const char *long_flag, const char *short_flag) {
    return strcmp(token, long_flag) == 0 || strcmp(token, short_flag) == 0;
}

cli_invocation cli_parse(int argc, char **argv) {
    cli_invocation invocation = { .command = cli_cmd_none, .arg = NULL, .raw_token = NULL };
    if (argc < 2)
        return invocation;
    const char *token = argv[1];
    invocation.raw_token = token;
    if (matches_flag(token, "--help", "-h")) {
        invocation.command = cli_cmd_help;
        return invocation;
    }
    if (matches_flag(token, "--version", "-V")) {
        invocation.command = cli_cmd_version;
        return invocation;
    }
    invocation.command = lookup_command(token);
    if (argc >= 3)
        invocation.arg = argv[2];
    return invocation;
}

const char *cli_option_value(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    }
    return NULL;
}

void cli_print_usage(void) {
    printf(
        "molto %s - a modern packaging ecosystem for C and C++\n"
        "\n"
        "Usage:\n"
        "    molto <command> [arguments]\n"
        "\n"
        "Commands:\n"
        "    new <name>     Create a new project in a new directory\n"
        "    init           Initialize a project in the current directory\n"
        "    build          Compile the project\n"
        "    run            Build and run the project\n"
        "    test           Run tests\n"
        "    bench          Run benchmarks\n"
        "    lint           Run diagnostics and static checks\n"
        "    add <dep>      Add a dependency\n"
        "    remove <dep>   Remove a dependency\n"
        "    publish        Publish the package to a registry\n"
        "    update         Update dependency versions\n"
        "    migrate <sys>  Import a Make/CMake/Meson project\n"
        "\n"
        "Options:\n"
        "    -h, --help     Show this help\n"
        "    -V, --version  Show version\n",
        cli_version());
}

static int run_unimplemented(const char *token) {
    fprintf(stderr, "molto: command '%s' is not yet implemented\n", token);
    return exit_build_failure;
}

int cli_run(int argc, char **argv) {
    cli_invocation invocation = cli_parse(argc, argv);
    switch (invocation.command) {
        case cli_cmd_none:
        case cli_cmd_help:
            cli_print_usage();
            return exit_ok;
        case cli_cmd_version:
            printf("molto %s\n", cli_version());
            return exit_ok;
        case cli_cmd_new:
            return new_command_run(invocation.arg);
        case cli_cmd_init:
            return init_command_run();
        case cli_cmd_build:
            return build_command_run(cli_option_value(argc, argv, "--profile"));
        case cli_cmd_run:
        case cli_cmd_test:
        case cli_cmd_bench:
        case cli_cmd_lint:
        case cli_cmd_add:
        case cli_cmd_remove:
        case cli_cmd_publish:
        case cli_cmd_update:
        case cli_cmd_migrate:
            return run_unimplemented(invocation.raw_token);
        case cli_cmd_unknown:
        default:
            fprintf(stderr, "molto: unknown command '%s'\n\n",
                    invocation.raw_token != NULL ? invocation.raw_token : "");
            cli_print_usage();
            return exit_usage_error;
    }
}
