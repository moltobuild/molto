#ifndef MOLTO_GLOB_H
#define MOLTO_GLOB_H

#include <stdbool.h>

/*
 * Shell-style pattern matching, as POSIX `fnmatch` does it with no flags.
 *
 * Molto had this from libc until the Windows port, where mingw ships no
 * <fnmatch.h> at all. It is written out here rather than shimmed because the
 * behaviour is already a promise the manifest documentation makes, and a
 * promise that holds on one platform is not a promise.
 *
 * What a pattern means:
 *
 *   *        any run of characters, the empty one included, and it crosses a
 *            slash — this is fnmatch without FNM_PATHNAME, which is what a
 *            pattern like "build/" followed by two stars relies on to reach
 *            all the way down
 *   ?        exactly one character, a slash included
 *   [abc]    one of those characters; [a-z] a range, [!abc] or [^abc] none of
 *            them. An unterminated [ is a literal bracket, as fnmatch has it
 *   \x       x itself, however special it would otherwise be
 *
 * Matching is anchored at both ends: the pattern describes the whole string.
 */
[[nodiscard]] bool glob_match(const char *pattern, const char *text);

#endif /* MOLTO_GLOB_H */
