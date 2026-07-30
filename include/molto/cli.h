#ifndef MOLTO_CLI_H
#define MOLTO_CLI_H

/* Parse argv and execute the matched molto command; returns the exit code. */
[[nodiscard]] int cli_run(int argc, char **argv);

/* Molto version string. */
[[nodiscard]] const char *cli_version(void);

#endif /* MOLTO_CLI_H */
