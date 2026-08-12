#ifndef MOLTO_MANIFEST_SERVICE_H
#define MOLTO_MANIFEST_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/* Build settings for a single profile (RFC-0003). */
typedef struct {
    int opt_level;
    bool debug_info;
} manifest_profile;

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

/* Render a default Project.toml for a package named `name`.
   Returns a heap-allocated string the caller must free(), or NULL on error. */
[[nodiscard]] char *manifest_render_default(const char *name);

#endif /* MOLTO_MANIFEST_SERVICE_H */
