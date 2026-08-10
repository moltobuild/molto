#ifndef MOLTO_MANIFEST_EDIT_H
#define MOLTO_MANIFEST_EDIT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Editing `Project.toml` in place, for `molto add` and `molto remove`.
 *
 * RFC-0003 promises the manifest is never "generated or overwritten silently".
 * Molto writes to it only when a command asks, and when it does, everything the
 * command did not touch has to survive: comments, key order, blank lines,
 * whichever quoting style was used. A user's manifest is a file they wrote.
 *
 * That rules out the obvious implementation. Reserialising a parsed document
 * cannot work, because the parser keeps none of it — comments are stripped
 * before any reader sees them, `1_000` becomes 1000, and a multi-line array
 * collapses. So this edits *lines*: it finds the one line that has to change
 * and rewrites the file around it, byte for byte.
 *
 * Every edit is validated by parsing the result before it is written. An edit
 * that would produce a manifest Molto cannot read is refused, and the file on
 * disk is left exactly as it was.
 */

/* Add or replace one dependency.
 *
 * `table` is "deps" or "dev-deps"; the table is created at the end of the file
 * if it is not there. `value` is the TOML right-hand side, already composed:
 * `"3.53.4"` or `{ path = "modules/http" }`.
 *
 * An entry that already exists is replaced in place, so re-adding a dependency
 * at a new version leaves it where the user put it rather than moving it to
 * the bottom. Adding to one table a name the other already has is refused: one
 * package is one version (RFC-0008), and the two spellings would disagree
 * about which. */
[[nodiscard]] bool manifest_add_dep(const char *path, const char *table, const char *name,
                                    const char *value, char *err, size_t err_size);

/* Remove one dependency, from whichever of the two tables holds it. Removing
   what is not there is an error: silence would leave the user believing a
   dependency is gone when it is about to be compiled again. */
[[nodiscard]] bool manifest_remove_dep(const char *path, const char *name, char *err,
                                       size_t err_size);

/* Which table holds `name`, or NULL. The returned string is a literal. */
[[nodiscard]] const char *manifest_find_dep(const char *text, const char *name);

#endif /* MOLTO_MANIFEST_EDIT_H */
