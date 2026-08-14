#ifndef MOLTO_SEMVER_H
#define MOLTO_SEMVER_H

#include <molto/util/str_list.h>

#include <stdbool.h>
#include <stddef.h>

/*
 * Semantic versions, ordered.
 *
 * Molto needs two things from semver and deliberately not a third. It needs to
 * **order** releases, so `molto add sqlite` can take the newest one, and it
 * needs to **validate** them, so a typo is refused rather than compared byte by
 * byte. It does not need to match ranges: a manifest names one exact version
 * (RFC-0008), so there is no requirement to satisfy, no constraint solver and
 * no backtracking anywhere in Molto.
 *
 * The ordering is semver's own, which is not string order and not numeric
 * order: `1.10.0` is newer than `1.9.0`, `1.0.0-alpha` is *older* than `1.0.0`,
 * and build metadata is ignored entirely.
 */

#define SEMVER_PRERELEASE_MAX 64
#define SEMVER_BUILD_MAX 64

typedef struct {
    unsigned long major;
    unsigned long minor;
    unsigned long patch;
    /* "" when absent. A version that has one is older than the same version
       without: `1.0.0-rc.1` precedes `1.0.0`, because a pre-release is a
       release that is not ready. */
    char prerelease[SEMVER_PRERELEASE_MAX];
    /* Recorded so a round trip keeps it, and then ignored: two versions that
       differ only here are the same version. */
    char build[SEMVER_BUILD_MAX];
} semver;

/* Parse `major.minor.patch` with optional `-prerelease` and `+build`.

   Strict on purpose: three components, all numeric, no leading zeroes, no `v`
   prefix. A registry serves version strings it never interprets (RFC-0010), so
   this is the only place that decides what one means — and something that is
   not a version should be refused here rather than sorted somewhere. */
[[nodiscard]] bool semver_parse(const char *text, semver *out);

/* Negative, zero or positive as `a` precedes, equals or follows `b`, by semver
   precedence. Build metadata takes no part. */
[[nodiscard]] int semver_compare(const semver *a, const semver *b);

/* The highest of `count` version strings, written to `out`.

   Unparseable entries are skipped rather than refused: a registry may serve a
   toolchain-style version under a name a package also uses, and one odd string
   should not stop a resolution that has good candidates. False when none of
   them parsed. */
[[nodiscard]] bool semver_highest(const char *const *versions, size_t count, char *out,
                                  size_t out_size);

/* Order `list` newest first, in place, and answer how many of the leading
   entries are versions.

   Unparseable entries are not dropped -- they are pushed to the tail and left
   in the order they arrived -- so a caller that wants only the versions reads
   the returned count and one that wants everything still has it.

   `semver_highest` answers what to install; this answers what to try next,
   which is what a search for a version that removes a conflict needs. */
[[nodiscard]] size_t semver_sort_desc(str_list *list);

#endif /* MOLTO_SEMVER_H */
