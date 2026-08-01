#ifndef MOLTO_TEST_COMMAND_H
#define MOLTO_TEST_COMMAND_H

#include <stdbool.h>

/* Execute `molto test [--profile <name>]` in the current directory: build the
   project's test executables and run each one, reporting pass/fail.
   `requested_profile` may be NULL (defaults to "debug").
   Returns exit_ok if every test passes, otherwise a molto_exit_code. */
[[nodiscard]] int test_command_run(const char *requested_profile,
                                   bool refresh_toolchain);

#endif /* MOLTO_TEST_COMMAND_H */
