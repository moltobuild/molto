#ifndef MOLTO_DOC_H
#define MOLTO_DOC_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/util/json.h>
#include <molto/util/str_list.h>
#include <molto/util/toml.h>

/*
 * One recipe, two encodings.
 *
 * A recipe is TOML on disk — what its author writes and `molto publish` reads —
 * and JSON on the wire: the registry parses it at publication, serves the
 * parsed document inside an artifact's `metadata` (RFC-0010), and never keeps
 * the text it came from.
 *
 * Both are the same document, so both get one reader. Two readers for one
 * format drift, and they drift asymmetrically: the copy that only ever sees
 * registry answers has no local file anyone can diff against.
 *
 * The surface is deliberately the subset a recipe uses — tables of scalars and
 * string arrays, addressed by a dotted path. Anything one encoding can express
 * and the other cannot is outside it, and stays outside it.
 */

typedef enum {
    doc_backend_toml,
    doc_backend_json,
} doc_backend;

/* A borrowed view of a parsed document. Copyable, and valid only while the
   document it was made from is alive — like json_value, and for the same
   reason: a view that owned something would need a free() nobody would call. */
typedef struct {
    doc_backend backend;
    const toml_document *toml; /* set when backend is doc_backend_toml */
    json_value json;           /* the object the tables hang off, otherwise */
} doc_view;

[[nodiscard]] doc_view doc_from_toml(const toml_document *doc);

/* `object` is the recipe itself: for a registry answer, the `metadata` member
   of an artifact, not the artifact. */
[[nodiscard]] doc_view doc_from_json(json_value object);

/* True when `table` is declared and is a table. `table` is a dotted path:
   "source", "toolchain.c". "" is the document's own top level, where a
   recipe's schema, form, kind, name, version and target live. */
[[nodiscard]] bool doc_has_table(doc_view doc, const char *table);

/* True when `key` is declared inside `table`, whatever type it holds.

   It exists because the readers below answer false for two different things —
   "not there" and "there, but not what you asked for" — and the difference is
   the difference between a default and an error. A `sources = "sqlite3.c"`
   that should have been a list would otherwise compile nothing, quietly. */
[[nodiscard]] bool doc_has_key(doc_view doc, const char *table, const char *key);

/* Read one value. Each returns false when the key is absent or holds another
   type, and leaves `out` untouched, so a caller can seed a default first. */
[[nodiscard]] bool doc_get_string(doc_view doc, const char *table, const char *key, char *out,
                                  size_t out_size);
[[nodiscard]] bool doc_get_int(doc_view doc, const char *table, const char *key, long *out);
[[nodiscard]] bool doc_get_bool(doc_view doc, const char *table, const char *key, bool *out);

/* Append a string array's elements to `out` (caller-initialised). False when
   the key is absent, is not an array, or holds an element that is not a
   string: half a list is worse than no list, because half a list compiles. */
[[nodiscard]] bool doc_get_array(doc_view doc, const char *table, const char *key, str_list *out);

/* Append the names declared immediately inside `table`, once each and in
   document order, values and tables alike. The TOML side is
   toml_section_members; the JSON side is the object's own members. This is how
   a table whose keys are not known in advance — a recipe's [deps] — is walked. */
[[nodiscard]] bool doc_table_members(doc_view doc, const char *table, str_list *out);

/* Copy a string array into a fixed-size destination: `capacity` entries of
   `stride` bytes each. Overflowing either is an error with a message rather
   than a silent truncation — a dropped define produces a green build that
   compiled something else. `dest` is the first byte of the array, so a caller
   passes `dest[0]` and `sizeof dest[0]`. An absent key is not an error and
   leaves `*count` alone. */
[[nodiscard]] bool doc_read_strings(doc_view doc, const char *table, const char *key, char *dest,
                                    size_t capacity, size_t stride, size_t *count, char *err,
                                    size_t err_size);

#endif /* MOLTO_DOC_H */
