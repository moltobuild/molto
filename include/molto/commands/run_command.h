#ifndef MOLTO_RUN_COMMAND_H
#define MOLTO_RUN_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/* Execute `molto run [--profile <name>] [--jobs <n>] [-- <args...>]`: build the
   project in the current directory and, on success, run its binary, forwarding
   `forwarded` (with `forwarded_count` elements) to the program.
   `requested_profile` may be NULL (defaults to "debug"); `jobs` is 0 for the
   whole machine.
   Returns the program's exit code, or a molto_exit_code on failure. */
[[nodiscard]] int run_command_run(const char *requested_profile, bool refresh_toolchain,
                                  size_t jobs, char *const *forwarded, int forwarded_count);

#endif /* MOLTO_RUN_COMMAND_H */
