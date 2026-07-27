#ifndef MOLTO_SOURCE_DISCOVERY_H
#define MOLTO_SOURCE_DISCOVERY_H

#include <stdbool.h>

#include <molto/util/str_list.h>

/* Recursively collect C and C++ source files (.c, .cpp, .cc) under `root`
   into `out`. Returns false on error (e.g. `root` cannot be read). */
[[nodiscard]] bool source_discovery_collect(const char *root, str_list *out);

/* Return true if `path` names a C++ source file (.cpp or .cc). */
[[nodiscard]] bool source_is_cpp(const char *path);

#endif /* MOLTO_SOURCE_DISCOVERY_H */
