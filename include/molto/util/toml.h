#ifndef MOLTO_TOML_H
#define MOLTO_TOML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* A small, robust TOML parser. It supports a useful subset: [section] and
   [a.b] headers, bare keys, inline comments, basic and literal strings,
   signed integers with '_' separators, and booleans. Arrays and inline tables
   are recognized and skipped (so a [deps] section does not break parsing).
   It fails closed: any malformed input yields NULL and a line-tagged message. */
typedef struct toml_document toml_document;

/* Parse `text` into a document. On failure returns NULL and, if `err` is not
   NULL, writes a "Project.toml:<line>: <reason>" style message. The input is
   never modified. Free the result with toml_free. */
[[nodiscard]] toml_document *toml_parse(const char *text, char *err, size_t err_size);

/* Release a document. Safe on NULL. */
void toml_free(toml_document *doc);

/* Dictionary-style access. Each returns false if the key is absent or has a
   different type. String values are copied with safe truncation. */
[[nodiscard]] bool toml_get_string(const toml_document *doc, const char *section,
                                   const char *key, char *out, size_t out_size);
[[nodiscard]] bool toml_get_int(const toml_document *doc, const char *section,
                                const char *key, long *out);
[[nodiscard]] bool toml_get_bool(const toml_document *doc, const char *section,
                                 const char *key, bool *out);

/* True if at least one key was declared under `section`. */
[[nodiscard]] bool toml_has_section(const toml_document *doc, const char *section);

/* Print every parsed entry (section/key/type/value) for debugging. */
void toml_dump(const toml_document *doc, FILE *stream);

/* --- Schema binding: populate a struct directly (reflection-like) --- */

typedef enum {
    toml_field_string,
    toml_field_int,
    toml_field_bool,
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
#define TOML_STR(Struct, sec, key, member) \
    { (sec), (key), toml_field_string, offsetof(Struct, member), sizeof(((Struct *)0)->member) }
#define TOML_INT(Struct, sec, key, member) \
    { (sec), (key), toml_field_int, offsetof(Struct, member), 0 }
#define TOML_BOOL(Struct, sec, key, member) \
    { (sec), (key), toml_field_bool, offsetof(Struct, member), 0 }

/* Populate `out` from `doc` following `schema`. Fields whose key is absent are
   left untouched (seed defaults before calling). Returns false (with a message
   in `err`) if a present key has an incompatible value or a string field has
   size 0. Integer fields are written as `int`. */
[[nodiscard]] bool toml_bind(const toml_document *doc, const toml_field *schema,
                             size_t field_count, void *out,
                             char *err, size_t err_size);

#endif /* MOLTO_TOML_H */
