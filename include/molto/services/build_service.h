#ifndef MOLTO_BUILD_SERVICE_H
#define MOLTO_BUILD_SERVICE_H

#include <stddef.h>

#include <molto/build/profile.h>
#include <molto/util/str_list.h>

/* Build the project rooted at `root` (may be ".") using `profile`.
   Discovers sources under `root/src`, compiles them incrementally, and links
   an executable at `root/build/<profile>/<package-name>`. When `out_binary` is
   not NULL and the build succeeds, the executable path is written into it
   (`out_binary_size` bytes). Returns a molto_exit_code. */
[[nodiscard]] int build_project(const char *root, build_profile profile,
                                char *out_binary, size_t out_binary_size);

/* Build the project's test executables: compiles `root/src` and then compiles
   and links each source under `root/tests` into its own executable at
   `root/build/<profile>/tests/<name>`, linked against the project's objects
   (excluding the app's src/main.c). Appends every built test binary path to
   `test_binaries_out` (caller-initialised, caller-freed). A missing or empty
   tests/ directory is not an error. Returns a molto_exit_code. */
[[nodiscard]] int build_tests(const char *root, build_profile profile,
                              str_list *test_binaries_out);

#endif /* MOLTO_BUILD_SERVICE_H */
