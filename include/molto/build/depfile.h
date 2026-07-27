#ifndef MOLTO_DEPFILE_H
#define MOLTO_DEPFILE_H

#include <stdbool.h>

#include <molto/util/str_list.h>

/*
 * Parser for the dependency files gcc/g++ emit with -MMD -MF. Such a file is in
 * Makefile syntax: a target, a colon, and the list of files it depends on (the
 * source plus every header it includes), possibly across several lines joined
 * with a trailing backslash. For example:
 *
 *     build/debug/obj/main.c.o: src/main.c src/util.h \
 *      src/config.h
 *
 * Molto reads the prerequisite list to decide whether a translation unit must
 * be recompiled when one of its headers changes.
 */

/* Extract the prerequisites (everything after the ':') from `text` into `out`,
   skipping the target. Handles line continuations ('\' + newline) and treats
   runs of whitespace as separators. Returns false if `text` has no ':' (an
   empty prerequisite list after ':' is still a success). Escaped spaces ("\ ")
   are kept as literal spaces in a path on a best-effort basis. */
[[nodiscard]] bool depfile_parse(const char *text, str_list *out);

/* Read the depfile at `path` and parse it. Returns false if the file does not
   exist or cannot be read. */
[[nodiscard]] bool depfile_read(const char *path, str_list *out);

#endif /* MOLTO_DEPFILE_H */
