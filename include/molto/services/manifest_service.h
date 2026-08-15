#ifndef MOLTO_MANIFEST_SERVICE_H
#define MOLTO_MANIFEST_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/util/doc.h>

/* Build settings for a single profile (RFC-0003). */
typedef struct {
    int opt_level;
    bool debug_info;
} manifest_profile;

/* Limits on the publishing metadata. Generous enough for what these keys are
   for — a sentence, an SPDX expression, a URL — and, like every other manifest
   limit, an error rather than a silent truncation: a description cut in half
   is one the author never wrote, and it is what a registry would serve. */
#define MANIFEST_DESCRIPTION_MAX 256
#define MANIFEST_LICENSE_MAX 64
#define MANIFEST_URL_MAX 256
#define MANIFEST_MAX_AUTHORS 8
#define MANIFEST_AUTHOR_MAX 128

/*
 * What a package says about itself, as opposed to what it needs to build.
 *
 * The same table under two names: a manifest writes it as `[package]`
 * (RFC-0003) beside `name` and `version`, and a recipe writes it as `[about]`
 * (RFC-0009). RFC-0009 requires the two to agree, which is why there is one
 * reader and not two — two readers of one format drift, and the copy that only
 * ever sees registry answers has no local file to diff against.
 *
 * Every field is optional, and empty means "not stated". Nothing here reaches a
 * compile line; it is what a catalogue search, a licence audit and a bill of
 * materials read.
 */
typedef struct {
    char description[MANIFEST_DESCRIPTION_MAX];
    /* An SPDX expression, checked for shape and not against the identifier
       list. See manifest_is_valid_license. */
    char license[MANIFEST_LICENSE_MAX];
    char homepage[MANIFEST_URL_MAX];
    char repository[MANIFEST_URL_MAX];
    char authors[MANIFEST_MAX_AUTHORS][MANIFEST_AUTHOR_MAX];
    size_t author_count;
} manifest_about;

/* Validate that `name` is a legal package identifier (snake_case, RFC-0001). */
[[nodiscard]] bool manifest_is_valid_name(const char *name);

/* True when `version` names one concrete version and not a range.

   The operators ^, ~, >=, >, <, <=, * and comma-separated conjunctions are not
   part of this format, and RFC-0008 gives the reason: a range is a standing
   authorisation to run code that does not exist yet, granted to whoever
   controls the publisher's account later. Refusing it is a security decision
   and not an ergonomic one, which is why it is a hard error rather than a
   warning.

   Semver still validates — `3.5` and `latest` are refused too, so a typo is
   caught rather than compared byte by byte against a registry's answers.

   `out_operator`, when not NULL, receives the offending operator so a caller
   can name it in the message. It is set to "" when the version is malformed
   for some other reason. */
[[nodiscard]] bool manifest_is_exact_version(const char *version, char *out_operator,
                                             size_t operator_size);

/* True when `expression` is shaped like an SPDX licence expression: identifiers
   joined by AND, OR and WITH, with parentheses balanced.

   Shape only. The SPDX identifier list is deliberately not embedded: it would
   be a list that expires, and refusing a licence published after this binary
   was built is worse than accepting one that is misspelled. What this does
   catch is the shape — `MIT OR` with nothing after it, `(MIT` never closed, two
   identifiers with nothing joining them — which is what a typo looks like.

   `+` is allowed as a suffix (`GPL-2.0+`), and WITH is treated as one more
   binary operator rather than as the exception clause it formally is: telling
   an exception identifier from a licence one needs the list this refuses to
   carry. */
[[nodiscard]] bool manifest_is_valid_license(const char *expression);

/* Read the publishing metadata out of `table`: "package" for a manifest,
   "about" for a recipe.

   Read through doc_view, so the same code answers for a recipe.toml on disk and
   for the parsed recipe a registry serves inside an artifact's `metadata`.

   An absent table, and every absent key, leave the corresponding field empty:
   none of this is required of anyone. What is refused is a key that is there
   and wrong — the wrong type, a value past its limit, a licence that is not an
   expression — with a message naming the table and the key. */
[[nodiscard]] bool manifest_read_about(doc_view doc, const char *table, manifest_about *out,
                                       char *err, size_t err_size);

/* Render a default Project.toml for a package named `name`.
   Returns a heap-allocated string the caller must free(), or NULL on error. */
[[nodiscard]] char *manifest_render_default(const char *name);

#endif /* MOLTO_MANIFEST_SERVICE_H */
