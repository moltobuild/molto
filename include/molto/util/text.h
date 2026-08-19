#ifndef MOLTO_UTIL_TEXT_H
#define MOLTO_UTIL_TEXT_H

#include <stddef.h>

/*
 * Bytes in, columns out.
 *
 * A compiler reports a position in a source line as a count of bytes; a
 * terminal advances by columns, and a UTF-8 character is up to four of the
 * first and one of the second. Pointing at what the compiler named therefore
 * means converting between the two, and converting in that direction: give it
 * the byte offset, get back where to put the caret.
 *
 * The conversion is a character count, not a display width: a character that
 * a terminal draws two columns wide — most of CJK, and emoji — is counted as
 * one. Doing better means wcwidth, which means a locale, which Molto never
 * sets. A caret under a line of Han characters will sit to the left of where
 * it belongs; a caret under anything Latin will not.
 */

/* The width, in columns, of the first `bytes` bytes of `s`. Stops at the
   terminating NUL, so a `bytes` past the end of the string is the whole
   string. NULL is zero columns. A tab counts as one, which is what it is
   worth to a string and not what it is worth to a terminal — for that, ask
   the two functions below. */
[[nodiscard]] size_t text_columns(const char *s, size_t bytes);

/* The column byte `offset` of `s` falls in once tabs have been advanced to
   the next stop of `tab_width`.
 *
 * This is the conversion a caret needs when the compiler counted bytes, which
 * clang does. gcc counts what a terminal counts and needs no conversion at
 * all — the two disagree on any line holding a tab or anything outside ASCII,
 * and agree on every other line there has ever been. */
[[nodiscard]] size_t text_column_of_byte(const char *s, size_t offset, size_t tab_width);

/* Copy `s` into `out` with its tabs advanced to the next stop of `tab_width`,
   so what is printed occupies the columns the compiler counted. Truncates
   rather than refusing. */
void text_expand_tabs(const char *s, size_t tab_width, char *out, size_t out_size);

/*
 * Copy `s` into `out`, replacing its middle with `…` when it does not fit.
 *
 * A URL and a path both carry what identifies them at their two ends and the
 * least of it in between: `https://github.com/` and `molto` say which package
 * this is, and the organisation between them rarely settles anything. Dropping
 * the tail — which is what an `snprintf` into a short buffer does — keeps the
 * half that identifies nothing and does it silently, so the reader is not told
 * that a name they do not recognise is a name they were not shown.
 *
 * The cut lands between characters, never inside one. Given less room than an
 * ellipsis plus a byte on each side, this degrades to a plain prefix rather
 * than writing a broken ellipsis. `out` is always terminated, and never
 * written past `out_size`; NULL is the empty string.
 */
void text_elide_middle(const char *s, char *out, size_t out_size);

#endif /* MOLTO_UTIL_TEXT_H */
