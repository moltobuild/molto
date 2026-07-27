#ifndef MOLTO_BUILD_SERVICE_H
#define MOLTO_BUILD_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/build/profile.h>

/* Build the project rooted at `root` (may be ".") using `profile`.
   Discovers sources under `root/src`, compiles them incrementally, and links
   an executable at `root/build/<profile>/<package-name>`.
   Returns a molto_exit_code. */
[[nodiscard]] int build_project(const char *root, build_profile profile);

/* Resolve the output executable path for `root`/`profile` by reading the
   package name from Project.toml. Returns false if the manifest or its name
   is missing; otherwise writes the path into `out` (`out_size` bytes). */
[[nodiscard]] bool build_binary_path(const char *root, build_profile profile,
                                     char *out, size_t out_size);

#endif /* MOLTO_BUILD_SERVICE_H */
