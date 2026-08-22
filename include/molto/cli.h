#ifndef MOLTO_CLI_H
#define MOLTO_CLI_H

#include <stdbool.h>

/* Parse argv and execute the matched molto command; returns the exit code. */
[[nodiscard]] int cli_run(int argc, char **argv);

/* Molto version string. */
[[nodiscard]] const char *cli_version(void);

/* Whether `name` is one of Molto's own commands.

   It exists for `molto plugin list`, which would otherwise report a
   `molto-build` on PATH as something a user could run. The command table is
   searched before any plugin (RFC-0014), so such a binary is unreachable, and a
   listing that did not say so would be describing a command that does not
   exist. */
[[nodiscard]] bool cli_has_command(const char *name);

#endif /* MOLTO_CLI_H */
