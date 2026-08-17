#ifndef MOLTO_TEST_COMMAND_H
#define MOLTO_TEST_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/* Execute `molto test [--profile <name>] [--jobs <n>]` in the current
   directory: build the project's test executables and run each one, reporting
   pass/fail. `requested_profile` may be NULL (defaults to "debug"); `jobs`
   caps the compilation, not the tests, which run one at a time.
   Returns exit_ok if every test passes, otherwise a molto_exit_code. */
[[nodiscard]] int test_command_run(const char *requested_profile, bool refresh_toolchain,
                                   size_t jobs);

#endif /* MOLTO_TEST_COMMAND_H */
