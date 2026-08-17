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
   string. NULL is zero columns. */
[[nodiscard]] size_t text_columns(const char *s, size_t bytes);

#endif /* MOLTO_UTIL_TEXT_H */
