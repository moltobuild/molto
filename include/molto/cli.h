#ifndef MOLTO_CLI_H
#define MOLTO_CLI_H

/* Command identifiers recognized by the molto CLI. */
typedef enum {
    cli_cmd_none,
    cli_cmd_help,
    cli_cmd_version,
    cli_cmd_new,
    cli_cmd_init,
    cli_cmd_build,
    cli_cmd_run,
    cli_cmd_test,
    cli_cmd_bench,
    cli_cmd_lint,
    cli_cmd_add,
    cli_cmd_remove,
    cli_cmd_publish,
    cli_cmd_update,
    cli_cmd_migrate,
    cli_cmd_unknown,
} cli_command;

/* Result of parsing argv: which command and its primary argument. */
typedef struct {
    cli_command command;
    const char *arg;       /* primary positional argument, or NULL */
    const char *raw_token; /* command token as typed, for diagnostics */
} cli_invocation;

/* Parse argv into a command invocation (pure, no side effects). */
[[nodiscard]] cli_invocation cli_parse(int argc, char **argv);

/* Parse and execute argv; returns the process exit code. */
[[nodiscard]] int cli_run(int argc, char **argv);

/* Return the value following the option `name` in argv (e.g. "--profile"),
   or NULL if the option is absent or has no value. Scanning stops at "--". */
[[nodiscard]] const char *cli_option_value(int argc, char **argv, const char *name);

/* Return a pointer into argv to the first argument after a bare "--", writing
   the number of such arguments into `*out_count`. Returns NULL (and 0) when
   there is no "--" separator. */
[[nodiscard]] char **cli_forwarded_args(int argc, char **argv, int *out_count);

/* Molto version string. */
[[nodiscard]] const char *cli_version(void);

/* Print usage/help text to stdout. */
void cli_print_usage(void);

#endif /* MOLTO_CLI_H */
