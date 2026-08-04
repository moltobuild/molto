#ifndef MOLTO_JSON_H
#define MOLTO_JSON_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A read-only JSON reader.
 *
 * It exists because the C standard library has none and RFC-0005 puts the style
 * configuration in JSON: `format.json` and `linter.json`. It parses a whole
 * document into one owned block and hands out lightweight handles into it, so
 * freeing is a single call and no accessor can outlive the document by
 * accident.
 *
 * Ported from pickup, where the same reader consumes the registry APIs. Kept
 * deliberately diffable against it: the two should not drift.
 *
 * Every accessor tolerates being handed something that is not there: reading a
 * field a server stopped sending yields an invalid value rather than a crash,
 * and the caller finds out by asking.
 */

/* Deepest nesting accepted. A document past this is refused rather than
   allowed to exhaust the stack. */
#define JSON_MAX_DEPTH 64

typedef enum {
    json_type_invalid,
    json_type_null,
    json_type_bool,
    json_type_number,
    json_type_string,
    json_type_array,
    json_type_object,
} json_type;

typedef struct json_document json_document;

/* A handle to one value inside a document. Copyable, and only valid while the
   document it came from is alive. */
typedef struct {
    const json_document *doc;
    size_t index;
} json_value;

/* Parse `text`. Returns NULL if it is not well-formed JSON, if it nests deeper
   than JSON_MAX_DEPTH, or if memory ran out. The caller owns the result and
   must pass it to json_free. `text` is not retained. */
[[nodiscard]] json_document *json_parse(const char *text);

/* Release a document and everything read out of it. Safe with NULL. */
void json_free(json_document *doc);

/* The document's top-level value. */
[[nodiscard]] json_value json_root(const json_document *doc);

/* False for anything that is not there: a missing key, an index past the end,
   or a lookup into a value of the wrong kind. */
[[nodiscard]] bool json_is_valid(json_value value);

/* The kind of `value`, or json_type_invalid if there is no value. */
[[nodiscard]] json_type json_type_of(json_value value);

/* Member `key` of an object. Invalid if `value` is not an object or has no
   such member. */
[[nodiscard]] json_value json_get(json_value object, const char *key);

/* Element `index` of an array. Invalid if `value` is not an array or the index
   is past the end. */
[[nodiscard]] json_value json_at(json_value array, size_t index);

/* Number of elements in an array, or of members in an object. Zero for
   anything else. */
[[nodiscard]] size_t json_count(json_value value);

/* The text of a string value, NUL-terminated with escapes already decoded, or
   NULL if `value` is not a string. Owned by the document. */
[[nodiscard]] const char *json_string(json_value value);

/* Read a number as an integer. False if `value` is not a number, or holds one
   that is not a whole number or does not fit in long long. Deliberately not an
   int: a reader that silently narrows is worse than one that will not fit. */
[[nodiscard]] bool json_number(json_value value, long long *out);

/* Read a boolean. False if `value` is not a boolean. */
[[nodiscard]] bool json_bool(json_value value, bool *out);

/* Name of member `index` of an object, or NULL. With json_member_at, this is
   how a reader walks a table whose keys it does not know in advance — the
   `rules` of linter.json — and how it refuses a key it does not recognise
   instead of ignoring a typo. */
[[nodiscard]] const char *json_key_at(json_value object, size_t index);

/* Value of member `index` of an object. Invalid if `value` is not an object or
   the index is past the end. */
[[nodiscard]] json_value json_member_at(json_value object, size_t index);

#endif /* MOLTO_JSON_H */
