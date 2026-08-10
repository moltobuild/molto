#ifndef MOLTO_TOML_H
#define MOLTO_TOML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <molto/util/str_list.h>

/*
 * A small, robust TOML parser. It supports a useful subset: [section] and
 * [a.b] headers, bare keys, inline comments, basic and literal strings, signed
 * integers with '_' separators, booleans, single-line string arrays, and inline
 * tables. It fails closed: any malformed input yields NULL and a line-tagged
 * message.
 *
 * An inline table is stored as a subsection, so it reads back exactly as the
 * equivalent header would:
 *
 *   [deps]
 *   sqlite = { git = "https://…", tag = "3.53.4" }
 *
 *   toml_get_string(doc, "deps.sqlite", "git", url, sizeof url);
 *
 * which is also how toml_section_keys enumerates it.
 *
 * Two ways to read the parsed document:
 *
 *   1) Dictionary style — pull individual values:
 *        toml_document *doc = toml_parse(text, err, sizeof err);
 *        char name[64];
 *        toml_get_string(doc, "package", "name", name, sizeof name);
 *        toml_free(doc);
 *
 *   2) Schema binding — fill a struct in one call (see toml_bind below):
 *        typedef struct { char name[64]; long width; } config;
 *        const toml_field schema[] = {
 *            TOML_STR(config, "package", "name",  name),
 *            TOML_INT(config, "layout",  "width", width),
 *        };
 *        toml_bind(doc, schema, 2, &cfg, err, sizeof err);
 */
/* Longest section name accepted. Public because a caller reading an array of
   tables has to size a buffer for one (see toml_table_array_section). */
#define TOML_SECTION_MAX 128

typedef struct toml_document toml_document;

/* Parse `text` into a document. On failure returns NULL and, if `err` is not
   NULL, writes a "Project.toml:<line>: <reason>" style message. The input is
   never modified. Free the result with toml_free. */
[[nodiscard]] toml_document *toml_parse(const char *text, char *err, size_t err_size);

/* Release a document. Safe on NULL. */
void toml_free(toml_document *doc);

/* Dictionary-style access. Each returns false if the key is absent or has a
   different type. String values are copied with safe truncation. */
[[nodiscard]] bool toml_get_string(const toml_document *doc, const char *section, const char *key,
                                   char *out, size_t out_size);
[[nodiscard]] bool toml_get_int(const toml_document *doc, const char *section, const char *key,
                                long *out);
[[nodiscard]] bool toml_get_bool(const toml_document *doc, const char *section, const char *key,
                                 bool *out);

/* True if at least one key was declared under `section`. */
[[nodiscard]] bool toml_has_section(const toml_document *doc, const char *section);

/* Append the keys declared under `section`, in declaration order, to `out`
   (caller-initialised). For sections whose keys are not known in advance, such
   as a table of environment variables. Returns false on allocation failure. */
[[nodiscard]] bool toml_section_keys(const toml_document *doc, const char *section, str_list *out);

/* Append the names declared immediately inside `section`, once each and in
   declaration order, to `out` (caller-initialised) — whether each was written
   as a value or as a table.

     [deps]
     yyjson = "1.2.32"
     sqlite = { git = "…", tag = "…" }
     [deps.http.opts]
     jobs = 4

   answers "yyjson", "sqlite", "http" for "deps": the immediate name, never the
   grandchild "http.opts", and never twice. Pass "" for the top level, which
   answers the root's own keys and the names of the top-level tables.

   It exists because a table whose entries are themselves tables cannot be
   enumerated otherwise: toml_section_keys answers only the entries written as a
   plain value, since `sqlite = { … }` stores its members under "deps.sqlite"
   and stores nothing at all under "deps". A [deps] reader built on that would
   read half a manifest and report no error, which is the failure this parser
   exists to prevent.

   An element of a [[name]] array is not a member: it is stored as "name[0]" and
   is walked with toml_table_array_count. Returns false on allocation failure. */
[[nodiscard]] bool toml_section_members(const toml_document *doc, const char *section,
                                        str_list *out);

/* Copy the string elements of an array value into `out` (caller-initialised).
   Returns false if the key is absent or is not a string array. Arrays must fit
   on a single line. */
[[nodiscard]] bool toml_get_array(const toml_document *doc, const char *section, const char *key,
                                  str_list *out);

/* --- Arrays of tables: [[name]] repeated --- */

/* Each [[name]] table is stored under its own section, named "name[0]",
   "name[1]", ... so the accessors above read one exactly as they read any other
   section. These two turn "the nth tool" into the section name to ask for:

     size_t count = toml_table_array_count(doc, "tool");
     for (size_t i = 0; i < count; i++) {
         char section[TOML_SECTION_MAX];
         if (toml_table_array_section("tool", i, section, sizeof section))
             toml_get_string(doc, section, "path", path, sizeof path);
     }

   How many [[name]] tables the document declares. */
[[nodiscard]] size_t toml_table_array_count(const toml_document *doc, const char *name);

/* The section name element `index` of a [[name]] array was stored under. False
   if it does not fit in `out`. Purely a spelling: it does not consult a
   document, so pair it with toml_table_array_count to stay in range. */
[[nodiscard]] bool toml_table_array_section(const char *name, size_t index, char *out,
                                            size_t out_size);

/* Print every parsed entry (section/key/type/value) for debugging. */
void toml_dump(const toml_document *doc, FILE *stream);

/* --- Schema binding: populate a struct directly (reflection-like) --- */

typedef enum {
    toml_field_string,
    toml_field_int,
    toml_field_bool,
    toml_field_array, /* string array; readable via toml_get_array, not bindable */
} toml_field_type;

/* One field of a binding schema: which TOML (section,key) maps to which struct
   member (by byte offset). `size` is the buffer capacity for string fields and
   is ignored (0) for int/bool. */
typedef struct {
    const char *section;
    const char *key;
    toml_field_type type;
    size_t offset;
    size_t size;
} toml_field;

/* Declare schema entries legibly, e.g. TOML_STR(config, "package", "name", name). */
#define TOML_STR(Struct, sec, key, member)                                                         \
    {(sec), (key), toml_field_string, offsetof(Struct, member), sizeof(((Struct *)0)->member)}
#define TOML_INT(Struct, sec, key, member)                                                         \
    {(sec), (key), toml_field_int, offsetof(Struct, member), 0}
#define TOML_BOOL(Struct, sec, key, member)                                                        \
    {(sec), (key), toml_field_bool, offsetof(Struct, member), 0}

/* Populate `out` from `doc` following `schema`. Fields whose key is absent are
   left untouched (seed defaults before calling). Returns false (with a message
   in `err`) if a present key has an incompatible value or a string field has
   size 0. Integer fields are written as `int`. */
[[nodiscard]] bool toml_bind(const toml_document *doc, const toml_field *schema, size_t field_count,
                             void *out, char *err, size_t err_size);

#endif /* MOLTO_TOML_H */
