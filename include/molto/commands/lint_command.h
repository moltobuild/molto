#ifndef MOLTO_LINT_COMMAND_H
#define MOLTO_LINT_COMMAND_H

#include <stdbool.h>

/* Execute `molto lint [--profile <name>] [--format text|json]`.
   `requested_profile` and `format` may be NULL, which means the defaults.
   Returns a molto_exit_code: non-zero when an error-severity diagnostic was
   produced. A warning reports and still succeeds (RFC-0005). */
[[nodiscard]] int lint_command_run(const char *requested_profile, bool refresh_toolchain,
                                   bool refresh_tools, const char *format);

#endif /* MOLTO_LINT_COMMAND_H */
