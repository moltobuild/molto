#ifndef MOLTO_BUILD_COMMAND_H
#define MOLTO_BUILD_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/* Execute `molto build [--profile <name>] [--jobs <n>]` in the current
   directory. `profile_name` may be NULL (defaults to "debug"); `jobs` is 0 when
   `-j` was not given, which compiles on the whole machine.
   Returns a molto_exit_code. */
[[nodiscard]] int build_command_run(const char *profile_name, bool refresh_toolchain, size_t jobs);

#endif /* MOLTO_BUILD_COMMAND_H */
