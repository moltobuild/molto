#ifndef MOLTO_SOURCE_DISCOVERY_H
#define MOLTO_SOURCE_DISCOVERY_H

#include <stdbool.h>

#include <molto/util/str_list.h>

/* Recursively collect C and C++ source files (.c, .cpp, .cc) under `root`
   into `out`. Returns false on error (e.g. `root` cannot be read). */
[[nodiscard]] bool source_discovery_collect(const char *root, str_list *out);

/* Recursively collect everything a style tool has an opinion about: the sources
   above plus headers (.h, .hpp, .hh). Wider than what the build compiles,
   because RFC-0005 is explicit that style applies to a header as much as to a
   translation unit — and a header nobody includes still has to be formatted. */
[[nodiscard]] bool source_discovery_collect_styleable(const char *root, str_list *out);

/* Return true if `path` names a C++ source file (.cpp or .cc). */
[[nodiscard]] bool source_is_cpp(const char *path);

/* Return true if `path` names a header (.h, .hpp or .hh). */
[[nodiscard]] bool source_is_header(const char *path);

#endif /* MOLTO_SOURCE_DISCOVERY_H */
