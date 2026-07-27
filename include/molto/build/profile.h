#ifndef MOLTO_PROFILE_H
#define MOLTO_PROFILE_H

#include <stdbool.h>

#include <molto/services/manifest_service.h>

/* Build profiles defined in RFC-0003 / spec section 13. */
typedef enum {
    profile_debug,
    profile_release,
    profile_bench,
    profile_custom,
} build_profile;

/* Parse a profile name into `*out`. Returns false for an unknown name. */
[[nodiscard]] bool profile_parse(const char *name, build_profile *out);

/* Canonical lowercase name of a profile ("debug", "release", ...). */
[[nodiscard]] const char *profile_name(build_profile profile);

/* Built-in default build settings for a profile, used as a base before
   overriding with values from Project.toml. */
[[nodiscard]] manifest_profile profile_defaults(build_profile profile);

#endif /* MOLTO_PROFILE_H */
