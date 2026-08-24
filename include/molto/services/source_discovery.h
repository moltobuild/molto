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

/* Collect what the tests are built from: everything under `<root>/tests`, plus
   the extra entries `[test].sources` lists. A listed directory is walked; a
   listed file is taken as it is; a relative entry anchors at `root` and an
   absolute one is used as written.

   It reports through `err` rather than through stderr, and it takes the entries
   rather than a `project_ctx`, because two callers now need the same answer for
   different reasons: the build compiles the sources, and the native frontend
   describes them. A collector that printed would make the second one talk.

   A missing or empty `tests/` is not an error — there is simply nothing. An
   entry `[test].sources` names that does not exist is one. */
[[nodiscard]] bool source_discovery_collect_tests(const char *root, const str_list *extra,
                                                  str_list *out, char *err, size_t err_size);

/* Return true if `path` names a C++ source file (.cpp or .cc). */
[[nodiscard]] bool source_is_cpp(const char *path);

/* Return true if `path` names a header (.h, .hpp or .hh). */
[[nodiscard]] bool source_is_header(const char *path);

#endif /* MOLTO_SOURCE_DISCOVERY_H */
