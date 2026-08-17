#ifndef MOLTO_PROGRESS_H
#define MOLTO_PROGRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Saying that something is still happening.
 *
 * Molto has one kind of work that takes long enough to need this and cannot
 * say how long it will take: asking a registry. A conflict search walks the
 * metadata graph once per candidate release (RFC-0008), which costs requests
 * rather than downloads — but enough of them to look like a hang.
 *
 * So this is a spinner and not a bar. A bar needs a total, and there is no
 * honest total here: the number of questions the search will ask is not known
 * until it stops asking. Drawing a percentage over an invented denominator
 * would be printing a figure nobody measured.
 *
 * Everything goes to stderr, and only when stderr is a terminal. A spinner in
 * a log file is noise, and a spinner in a pipe is corruption of whatever was
 * being piped.
 *
 * Ported from pickup's `src/util/progress.c`, which had already answered these
 * questions for the same ecosystem.
 */

#define SPINNER_FRAMES 4

/* Whether anything drawn on `out` would be seen by a person. */
[[nodiscard]] bool progress_is_interactive(FILE *out);

/*
 * A bar, for the one thing that does have an honest total.
 *
 * The reservation above applies to a search, not to a compilation: a build
 * works out which translation units are stale before it compiles any of them,
 * so the denominator is counted rather than invented. That is the whole reason
 * the build plans every pass before running one.
 *
 * These two are pure — they compose bytes and answer a question, and neither
 * decides whether anyone should see them. Colour, placement and the choice to
 * draw at all belong to the caller.
 */

/* Bytes needed to hold a bar of `cells` columns. A cell is a three-byte glyph,
   so a column is not a byte and sizing a buffer by the column count would
   truncate it two thirds of the way along. */
#define PROGRESS_BAR_SIZE(cells) ((cells) * 3 + 1)

/* Write a bar of `cells` columns for `done` out of `total` into `out`, and
   return the bytes written, excluding the terminator.

   Nothing else goes in: no percentage, no escapes, no leading space. A total of
   zero is a full bar — a build with nothing to compile is a build that is
   done — and `done` beyond `total` is clamped rather than overflowing the
   buffer. Returns 0 and writes an empty string when `out` is too small for
   PROGRESS_BAR_SIZE(cells). */
size_t progress_bar_render(char *out, size_t size, size_t done, size_t total, size_t cells);

/* `done` out of `total` as a whole percentage, floored, so 100 is reached only
   when the last unit is done. A total of zero is 100. */
[[nodiscard]] size_t progress_bar_percent(size_t done, size_t total);

/* Erase the line the cursor is on and return to its start.

   Unlike progress_clear this writes an escape sequence rather than a run of
   spaces, because the run assumes the terminal is at least as wide as the run
   is long. It is not: a bar redrawn twenty times a second on an eighty-column
   terminal would scroll the display on every frame. */
void progress_erase_line(FILE *out);

/* One frame of the spinner, with a label and no figure. `frame` may grow
   without bound; it is taken modulo the frame count. */
void spinner_wait(FILE *out, const char *label, size_t frame);

/* Blank the line the spinner is on and return to its start. */
void progress_clear(FILE *out);

/*
 * Whether anything was drawn, so it can be taken away exactly once.
 *
 * A caller cannot simply clear unconditionally: on a non-terminal nothing was
 * drawn, and the clear itself would be the only thing written.
 */
typedef struct {
    bool drawn;
} progress_line;

/* Clear the line if this one was drawn on, and mark it clean. Called before
   printing anything else, because half a spinner in front of an error message
   reads as part of the error message. */
void progress_line_clear(FILE *out, progress_line *line);

#endif /* MOLTO_PROGRESS_H */
