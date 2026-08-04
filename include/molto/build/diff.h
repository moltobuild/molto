#ifndef MOLTO_DIFF_H
#define MOLTO_DIFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * A unified diff between two versions of one file.
 *
 * `molto fmt --diff` shows what the formatter would change without writing it,
 * and clang-format has no --diff of its own: it prints the formatted file, and
 * the comparison is ours to make. This is the only part of the style commands
 * that does not delegate to a backend, which is why it is a module of its own
 * with no I/O beyond the stream it writes to.
 */

/* Lines of unchanged context shown around a change, as `diff -u` defaults. */
#define DIFF_CONTEXT_LINES 3

/* Write the unified diff from `original` to `formatted`, labelled with `path`,
   to `stream`. Writes nothing when the two are identical. `*changed` says
   whether they differed, which is what `--check` reports on. False only if the
   inputs could not be held in memory. */
[[nodiscard]] bool diff_unified(const char *original, const char *formatted, const char *path,
                                size_t context, FILE *stream, bool *changed);

#endif /* MOLTO_DIFF_H */
