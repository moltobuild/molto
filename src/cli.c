#include <molto/cli.h>

#include <molto/commands/build_command.h>
#include <molto/commands/clean_command.h>
#include <molto/commands/fmt_command.h>
#include <molto/commands/init_command.h>
#include <molto/commands/lint_command.h>
#include <molto/commands/new_command.h>
#include <molto/commands/run_command.h>
#include <molto/commands/test_command.h>
#include <molto/exit_code.h>
#include <molto/util/cli.h>

#include <stdio.h>

#define MOLTO_VERSION "0.2.0"

const char *cli_version(void) { return MOLTO_VERSION; }

/* The --all switch of `molto clean`. */
static const cli_option clean_options[] = {
    {"--all", 'a', cli_opt_flag, NULL, "Also remove .bin/ (the incremental state)", NULL},
};

/* The options shared by build/run/test. --refresh-toolchain asks again which
   compiler satisfies [target] instead of reusing the recorded answer. */
static const cli_option build_options[] = {
    {"--profile", 'p', cli_opt_value, "<name>", "Build profile (debug, release, bench, custom)",
     "debug"},
    {"--refresh-toolchain", 0, cli_opt_flag, NULL,
     "Resolve the compiler again instead of reusing the cached one", NULL},
};

/* `molto lint` takes what a build takes — the profile decides which defines are
   in force, so it decides what even compiles — plus the machine-readable output
   CI wants. */
static const cli_option lint_options[] = {
    {"--profile", 'p', cli_opt_value, "<name>", "Build profile (debug, release, bench, custom)",
     "debug"},
    {"--refresh-toolchain", 0, cli_opt_flag, NULL,
     "Resolve the compiler again instead of reusing the cached one", NULL},
    {"--refresh-tools", 0, cli_opt_flag, NULL,
     "Ask pickup again which formatter and linter this machine has", NULL},
    {"--format", 'f', cli_opt_value, "<fmt>", "Output format (text, json)", "text"},
};

/* `molto fmt` writes by default; --check and --diff are two ways of asking
   what it would do without doing it. --refresh-tools asks pickup again which
   formatter this machine has instead of reusing the recorded answer. */
static const cli_option fmt_options[] = {
    {"--check", 'c', cli_opt_flag, NULL, "Report what would change and write nothing (for CI)",
     NULL},
    {"--diff", 'd', cli_opt_flag, NULL, "Print the unified diff instead of writing", NULL},
    {"--refresh-tools", 0, cli_opt_flag, NULL,
     "Ask pickup again which formatter and linter this machine has", NULL},
};

/* --- command handlers: thin adapters over the *_command_run functions --- */

static int handle_new(const cli_args *args) {
    return new_command_run(cli_args_positional(args, 0));
}

static int handle_init(const cli_args *args) {
    (void)args;
    return init_command_run();
}

static bool wants_refresh(const cli_args *args) {
    return cli_args_flag(args, "--refresh-toolchain");
}

static int handle_build(const cli_args *args) {
    return build_command_run(cli_args_option(args, "--profile"), wants_refresh(args));
}

static int handle_run(const cli_args *args) {
    int forwarded_count = 0;
    char *const *forwarded = cli_args_forwarded(args, &forwarded_count);
    return run_command_run(cli_args_option(args, "--profile"), wants_refresh(args), forwarded,
                           forwarded_count);
}

static int handle_test(const cli_args *args) {
    return test_command_run(cli_args_option(args, "--profile"), wants_refresh(args));
}

static int handle_lint(const cli_args *args) {
    return lint_command_run(cli_args_option(args, "--profile"), wants_refresh(args),
                            cli_args_flag(args, "--refresh-tools"),
                            cli_args_option(args, "--format"));
}

static int handle_fmt(const cli_args *args) {
    return fmt_command_run(cli_args_flag(args, "--check"), cli_args_flag(args, "--diff"),
                           cli_args_flag(args, "--refresh-tools"));
}

static int handle_clean(const cli_args *args) {
    return clean_command_run(cli_args_flag(args, "--all"));
}

static int handle_unimplemented(const cli_args *args) {
    fprintf(stderr,
            "molto: '%s' is not implemented yet "
            "(see rfcs/0002-cli-specification.md)\n",
            cli_args_command_name(args));
    return exit_not_implemented;
}

/* --- command table --- */

static const cli_command commands[] = {
    {"new", "Create a new project in a new directory", "<name>", NULL, 0, handle_new},
    {"init", "Initialize a project in the current directory", NULL, NULL, 0, handle_init},
    {"build", "Compile the project", NULL, build_options,
     sizeof build_options / sizeof build_options[0], handle_build},
    {"run", "Build and run the project (args after -- go to the program)", NULL, build_options,
     sizeof build_options / sizeof build_options[0], handle_run},
    {"test", "Build and run the project's tests", NULL, build_options,
     sizeof build_options / sizeof build_options[0], handle_test},
    {"clean", "Remove build output", NULL, clean_options,
     sizeof clean_options / sizeof clean_options[0], handle_clean},
    {"fmt", "Format the project's sources", NULL, fmt_options,
     sizeof fmt_options / sizeof fmt_options[0], handle_fmt},
    {"bench", "Run benchmarks", NULL, NULL, 0, handle_unimplemented},
    {"lint", "Run diagnostics and static checks", NULL, lint_options,
     sizeof lint_options / sizeof lint_options[0], handle_lint},
    {"add", "Add a dependency", "<dep>", NULL, 0, handle_unimplemented},
    {"remove", "Remove a dependency", "<dep>", NULL, 0, handle_unimplemented},
    {"publish", "Publish the package to a registry", NULL, NULL, 0, handle_unimplemented},
    {"update", "Update dependency versions", NULL, NULL, 0, handle_unimplemented},
    {"migrate", "Import a Make/CMake/Meson project", "<system>", NULL, 0, handle_unimplemented},
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
