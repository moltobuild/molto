#ifndef MOLTO_FMT_COMMAND_H
#define MOLTO_FMT_COMMAND_H

#include <stdbool.h>

/* Execute `molto fmt [--check | --diff] [--refresh-tools]`.
   Formats the project's sources in place unless asked to report instead.
   Returns a molto_exit_code: non-zero under --check when a file would change,
   which is what makes it usable in CI. */
[[nodiscard]] int fmt_command_run(bool check, bool diff, bool refresh_tools);

#endif /* MOLTO_FMT_COMMAND_H */
