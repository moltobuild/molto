#ifndef MOLTO_CLI_FRAMEWORK_H
#define MOLTO_CLI_FRAMEWORK_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A small declarative command-line framework. You describe an application as a
 * table of commands, each with a spec of options; the framework parses argv,
 * validates it, generates --help, and dispatches to a handler. Handlers read
 * the parsed values through the cli_args accessors instead of scanning argv.
 *
 * Supported syntax: `--opt value`, `--opt=value`, `-o value`, boolean flags
 * (`-v`, `--verbose`), a `--` separator (everything after is "forwarded"), and
 * positional arguments. `--help`/`-h` and `--version`/`-V` are handled for free.
 */

typedef enum {
    cli_opt_flag,  /* a boolean switch, present or not */
    cli_opt_value, /* takes an argument */
} cli_opt_kind;

/* One option a command accepts. */
typedef struct {
    const char *long_name; /* e.g. "--profile" (required) */
    char short_name;       /* e.g. 'p', or 0 for none */
    cli_opt_kind kind;
    const char *value_name; /* shown in help for value options, e.g. "<name>" */
    const char *help;
    const char *default_value; /* value options: reported when the flag is absent */
} cli_option;

/* The parsed arguments handed to a command handler (opaque). */
typedef struct cli_args cli_args;

/* A command handler: reads `args` and returns a process exit code. */
typedef int (*cli_handler)(const cli_args *args);

/* One command in the application. */
typedef struct {
    const char *name;     /* e.g. "build" */
    const char *summary;  /* one-line description for help */
    const char *arg_name; /* positional shown in help, e.g. "<name>", or NULL */
    const cli_option *options;
    size_t option_count;
    cli_handler handler;
} cli_command;

/* The application: program identity plus its command table. */
typedef struct {
    const char *program; /* e.g. "molto" */
    const char *version;
    const char *tagline; /* one line shown in the global help */
    const cli_command *commands;
    size_t command_count;
} cli_app;

/* Parse argv against `app`, handling --help/--version, validating options and
   dispatching to the matched command's handler. Returns the process exit code. */
[[nodiscard]] int cli_app_run(const cli_app *app, int argc, char **argv);

/* --- Accessors for use inside a handler --- */

/* Value of a value-option, its default if absent, or NULL. */
[[nodiscard]] const char *cli_args_option(const cli_args *args, const char *long_name);

/* Whether a flag-option was given. */
[[nodiscard]] bool cli_args_flag(const cli_args *args, const char *long_name);

/* Positional argument at `index`, or NULL if out of range. */
[[nodiscard]] const char *cli_args_positional(const cli_args *args, size_t index);

/* Number of positional arguments. */
[[nodiscard]] size_t cli_args_positional_count(const cli_args *args);

/* Arguments after a bare "--"; writes their count into `*count`. */
[[nodiscard]] char *const *cli_args_forwarded(const cli_args *args, int *count);

/* Name of the command being handled. */
[[nodiscard]] const char *cli_args_command_name(const cli_args *args);

#endif /* MOLTO_CLI_FRAMEWORK_H */
