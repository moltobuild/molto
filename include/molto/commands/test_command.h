#ifndef MOLTO_TEST_COMMAND_H
#define MOLTO_TEST_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/* Execute `molto test [--profile <name>] [--jobs <n>] [-- <args>]` in the
   current directory: build the project's test executables and run each one,
   reporting pass/fail. `requested_profile` may be NULL (defaults to "debug");
   `jobs` caps the compilation, not the tests, which run one at a time. The
   `forwarded_count` arguments after `--` are passed to every test binary, so a
   framework's own options (`-v`, `-k <filter>`) reach it.
   Returns exit_ok if every test passes, otherwise a molto_exit_code. */
[[nodiscard]] int test_command_run(const char *requested_profile, bool refresh_toolchain,
                                   size_t jobs, char *const *forwarded, int forwarded_count);

#endif /* MOLTO_TEST_COMMAND_H */
