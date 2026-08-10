#include <molto/cli.h>

#include <molto/commands/add_command.h>
#include <molto/commands/build_command.h>
#include <molto/commands/clean_command.h>
#include <molto/commands/fmt_command.h>
#include <molto/commands/init_command.h>
#include <molto/commands/lint_command.h>
#include <molto/commands/login_command.h>
#include <molto/commands/new_command.h>
#include <molto/commands/publish_command.h>
#include <molto/commands/run_command.h>
#include <molto/commands/test_command.h>
#include <molto/exit_code.h>
#include <molto/util/cli.h>

#include <stdio.h>
#include <string.h>

/* Handed in by whoever compiled this, from [package] version in Project.toml.
   Molto defines it for every project it builds, and the bootstrap Makefile
   reads the same line out of the same file -- so the number lives in one place
   and a binary cannot disagree with the manifest it was built from. */
#ifndef MOLTO_PKG_VERSION
#define MOLTO_PKG_VERSION "0.0.0-unknown"
#endif

const char *cli_version(void) { return MOLTO_PKG_VERSION; }

/* The --all switch of `molto clean`. */
static const cli_option clean_options[] = {
    {"--all", 'a', cli_opt_flag, NULL, "Also remove .bin/ (the incremental state)", NULL},
};

/* `molto add`. The source keys mirror RFC-0003's, so what is typed here and
   what ends up in the manifest are spelled the same. */
static const cli_option add_options[] = {
    {"--dev", 0, cli_opt_flag, NULL, "Add to [dev-deps]: needed to develop, never shipped", NULL},
    {"--git", 0, cli_opt_value, "<url>", "Take it from a git repository", NULL},
    {"--path", 0, cli_opt_value, "<dir>", "Take it from a local directory", NULL},
    {"--archive", 0, cli_opt_value, "<url>", "Take it from a source archive", NULL},
    {"--registry", 0, cli_opt_value, "<name>", "Resolve it against a named registry", NULL},
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
    {"--refresh-analysis", 0, cli_opt_flag, NULL,
     "Analyse every file again instead of replaying what did not change", NULL},
    {"--format", 'f', cli_opt_value, "<fmt>", "Output format (text, json)", "text"},
};

/* `molto login` has two ways in: exchange an email and password for a token,
   or paste one made on the registry's account page. --token is also the only
   way to log in where no terminal can hide a typed password, such as CI. */
static const cli_option login_options[] = {
    {"--registry", 'r', cli_opt_value, "<url>", "Registry to log in to", NULL},
    {"--email", 'e', cli_opt_value, "<address>", "Account to sign in as (prompted otherwise)",
     NULL},
    {"--token", 't', cli_opt_value, "<token>", "Store this token instead of signing in", NULL},
};

/* `molto publish` finds both its inputs in the current directory by default.
   --dry-run reads and hashes them and stops before anything is sent, which is
   the only way to check a recipe without spending a coordinate: they are
   immutable once published. */
static const cli_option publish_options[] = {
    {"--recipe", 0, cli_opt_value, "<path>", "Recipe describing the artifact", "recipe.toml"},
    {"--file", 'f', cli_opt_value, "<path>", "Archive to publish (the .tar.zst beside the recipe)",
     NULL},
    {"--dry-run", 0, cli_opt_flag, NULL, "Check and hash, but send nothing", NULL},
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
    {"--refresh-analysis", 0, cli_opt_flag, NULL,
     "Format every file again instead of skipping what did not change", NULL},
};

/* --- command handlers: thin adapters over the *_command_run functions --- */

static int handle_new(const cli_args *args) {
    return new_command_run(cli_args_positional(args, 0));
}

static int handle_init(const cli_args *args) {
    (void)args;
    return init_command_run();
}

/* `<name>@<version>` is one argument, because that is how a version is asked
   for everywhere else a package manager is used. */
static int handle_add(const cli_args *args) {
    const char *spec = cli_args_positional(args, 0);
    char name[128] = "";
    const char *version = NULL;
    if(spec != NULL) {
        const char *at = strrchr(spec, '@');
        if(at == NULL) {
            snprintf(name, sizeof name, "%s", spec);
        } else {
            snprintf(name, sizeof name, "%.*s", (int)(at - spec), spec);
            version = at + 1;
        }
    }

    /* Exactly one source, checked here so the message is about the command
       line rather than about a manifest the user did not write by hand. */
    static const char *const keys[] = {"git", "path", "archive"};
    const char *source_key = NULL;
    const char *source = NULL;
    for(size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char flag[16];
        snprintf(flag, sizeof flag, "--%s", keys[i]);
        const char *value = cli_args_option(args, flag);
        if(value == NULL)
            continue;
        if(source != NULL) {
            fprintf(stderr, "molto: a dependency has exactly one source\n");
            return exit_usage_error;
        }
        source_key = keys[i];
        source = value;
    }

    return add_command_run(spec == NULL ? NULL : name, version, source_key, source,
                           cli_args_option(args, "--registry"), cli_args_flag(args, "--dev"));
}

static int handle_remove(const cli_args *args) {
    return remove_command_run(cli_args_positional(args, 0));
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
                            cli_args_flag(args, "--refresh-analysis"),
                            cli_args_option(args, "--format"));
}

static int handle_fmt(const cli_args *args) {
    return fmt_command_run(cli_args_flag(args, "--check"), cli_args_flag(args, "--diff"),
                           cli_args_flag(args, "--refresh-tools"),
                           cli_args_flag(args, "--refresh-analysis"));
}

static int handle_clean(const cli_args *args) {
    return clean_command_run(cli_args_flag(args, "--all"));
}

static int handle_login(const cli_args *args) {
    return login_command_run(cli_args_option(args, "--registry"), cli_args_option(args, "--email"),
                             cli_args_option(args, "--token"));
}

static int handle_publish(const cli_args *args) {
    return publish_command_run(cli_args_option(args, "--recipe"), cli_args_option(args, "--file"),
                               cli_args_flag(args, "--dry-run"));
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
    {"add", "Add a dependency", "<dep>[@<version>]", add_options,
     sizeof add_options / sizeof add_options[0], handle_add},
    {"remove", "Remove a dependency", "<dep>", NULL, 0, handle_remove},
    {"login", "Store a registry credential", NULL, login_options,
     sizeof login_options / sizeof login_options[0], handle_login},
    {"publish", "Publish an artifact to a registry", NULL, publish_options,
     sizeof publish_options / sizeof publish_options[0], handle_publish},
    {"update", "Update dependency versions", NULL, NULL, 0, handle_unimplemented},
    {"migrate", "Import a Make/CMake/Meson project", "<system>", NULL, 0, handle_unimplemented},
};

int cli_run(int argc, char **argv) {
    const cli_app app = {
        .program = "molto",
        .version = MOLTO_PKG_VERSION,
        .tagline = "a modern packaging ecosystem for C and C++",
        .commands = commands,
        .command_count = sizeof commands / sizeof commands[0],
    };
    return cli_app_run(&app, argc, argv);
}
