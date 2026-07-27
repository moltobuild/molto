#ifndef MOLTO_BUILD_SERVICE_H
#define MOLTO_BUILD_SERVICE_H

#include <molto/build/profile.h>

/* Build the project rooted at `root` (may be ".") using `profile`.
   Discovers sources under `root/src`, compiles them incrementally, and links
   an executable at `root/build/<profile>/<package-name>`.
   Returns a molto_exit_code. */
[[nodiscard]] int build_project(const char *root, build_profile profile);

#endif /* MOLTO_BUILD_SERVICE_H */
