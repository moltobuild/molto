#include <molto/cli.h>

#include <molto/commands/build_command.h>
#include <molto/commands/init_command.h>
#include <molto/commands/new_command.h>
#include <molto/commands/run_command.h>
#include <molto/commands/test_command.h>
#include <molto/exit_code.h>
#include <molto/util/cli.h>

#include <stdio.h>

#define MOLTO_VERSION "0.1.0"

const char *cli_version(void) {
    return MOLTO_VERSION;
}

/* A --profile option shared by build/run/test. */
static const cli_option profile_option[] = {
    { "--profile", 'p', cli_opt_value, "<name>",
      "Build profile (debug, release, bench, custom)", "debug" },
};

/* --- command handlers: thin adapters over the *_command_run functions --- */

static int handle_new(const cli_args *args) {
    return new_command_run(cli_args_positional(args, 0));
}

static int handle_init(const cli_args *args) {
    (void)args;
    return init_command_run();
}

static int handle_build(const cli_args *args) {
    return build_command_run(cli_args_option(args, "--profile"));
}

static int handle_run(const cli_args *args) {
    int forwarded_count = 0;
    char *const *forwarded = cli_args_forwarded(args, &forwarded_count);
    return run_command_run(cli_args_option(args, "--profile"), forwarded, forwarded_count);
}

static int handle_test(const cli_args *args) {
    return test_command_run(cli_args_option(args, "--profile"));
}

static int handle_unimplemented(const cli_args *args) {
    fprintf(stderr, "molto: '%s' is not implemented yet "
                    "(see rfcs/0002-cli-specification.md)\n",
            cli_args_command_name(args));
    return exit_not_implemented;
}

/* --- command table --- */

static const cli_command commands[] = {
    { "new", "Create a new project in a new directory", "<name>", NULL, 0, handle_new },
    { "init", "Initialize a project in the current directory", NULL, NULL, 0, handle_init },
    { "build", "Compile the project", NULL,
      profile_option, sizeof profile_option / sizeof profile_option[0], handle_build },
    { "run", "Build and run the project (args after -- go to the program)", NULL,
      profile_option, sizeof profile_option / sizeof profile_option[0], handle_run },
    { "test", "Build and run the project's tests", NULL,
      profile_option, sizeof profile_option / sizeof profile_option[0], handle_test },
    { "bench", "Run benchmarks", NULL, NULL, 0, handle_unimplemented },
    { "lint", "Run diagnostics and static checks", NULL, NULL, 0, handle_unimplemented },
    { "add", "Add a dependency", "<dep>", NULL, 0, handle_unimplemented },
    { "remove", "Remove a dependency", "<dep>", NULL, 0, handle_unimplemented },
    { "publish", "Publish the package to a registry", NULL, NULL, 0, handle_unimplemented },
    { "update", "Update dependency versions", NULL, NULL, 0, handle_unimplemented },
    { "migrate", "Import a Make/CMake/Meson project", "<system>", NULL, 0, handle_unimplemented },
};

int cli_run(int argc, char **argv) {
    const cli_app app = {
        .program = "molto",
        .version = MOLTO_VERSION,
        .tagline = "a modern packaging ecosystem for C and C++",
        .commands = commands,
        .command_count = sizeof commands / sizeof commands[0],
    };
    return cli_app_run(&app, argc, argv);
}
