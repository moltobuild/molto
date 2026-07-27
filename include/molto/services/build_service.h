#ifndef MOLTO_BUILD_SERVICE_H
#define MOLTO_BUILD_SERVICE_H

#include <stddef.h>

#include <molto/build/profile.h>

/* Build the project rooted at `root` (may be ".") using `profile`.
   Discovers sources under `root/src`, compiles them incrementally, and links
   an executable at `root/build/<profile>/<package-name>`. When `out_binary` is
   not NULL and the build succeeds, the executable path is written into it
   (`out_binary_size` bytes). Returns a molto_exit_code. */
[[nodiscard]] int build_project(const char *root, build_profile profile,
                                char *out_binary, size_t out_binary_size);

#endif /* MOLTO_BUILD_SERVICE_H */
