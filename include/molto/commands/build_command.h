#ifndef MOLTO_BUILD_COMMAND_H
#define MOLTO_BUILD_COMMAND_H

#include <stdbool.h>

/* Execute `molto build [--profile <name>]` in the current directory.
   `profile_name` may be NULL (defaults to "debug").
   Returns a molto_exit_code. */
[[nodiscard]] int build_command_run(const char *profile_name, bool refresh_toolchain);

#endif /* MOLTO_BUILD_COMMAND_H */
