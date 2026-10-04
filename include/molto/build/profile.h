#ifndef MOLTO_PROFILE_H
#define MOLTO_PROFILE_H

#include <stdbool.h>

#include <molto/services/manifest_service.h>

/* Build profiles defined in RFC-0003 / spec section 13, and `coverage`, whose
   builds are instrumented for code coverage (RFC-0019). */
typedef enum {
    profile_debug,
    profile_release,
    profile_bench,
    profile_custom,
    profile_coverage,
} build_profile;

/* Parse a profile name into `*out`. Returns false for an unknown name. */
[[nodiscard]] bool profile_parse(const char *name, build_profile *out);

/* Canonical lowercase name of a profile ("debug", "release", ...). */
[[nodiscard]] const char *profile_name(build_profile profile);

/* The flag that instruments a build for coverage (RFC-0019), compile and link
   alike. Molto adds it in the coverage profile; nothing writes it by hand. */
#define PROFILE_COVERAGE_FLAG "--coverage"

/* Whether a toolchain of `vendor` (as pickup names it; "" when unknown) can
   build the coverage profile: GCC and Clang can, MSVC has no gcov coverage.
   An unknown vendor is given the benefit of the doubt, as C_COMPILER is. */
[[nodiscard]] bool profile_coverage_supported(const char *vendor);

#endif /* MOLTO_PROFILE_H */
