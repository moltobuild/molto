#ifndef MOLTO_PROGRESS_H
#define MOLTO_PROGRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Saying that something is still happening.
 *
 * Molto has two kinds of work that take long enough to need this and cannot say
 * how long they will take. Asking a registry is one: a conflict search walks the
 * metadata graph once per candidate release (RFC-0008), which costs requests
 * rather than downloads — but enough of them to look like a hang. Analysing a
 * project is the other: `molto lint` forks a compiler and a linter over every
 * source and says nothing until the last one is back.
 *
 * So these are spinners and not bars. A bar needs a total, and neither of those
 * has an honest one: the number of questions a search will ask is not known
 * until it stops asking, and an analysis that replays most of its files from the
 * cache would count a denominator nobody waits for. Drawing a percentage over an
 * invented denominator would be printing a figure nobody measured.
 *
 * Everything goes to stderr, and only when stderr is a terminal. A spinner in
 * a log file is noise, and a spinner in a pipe is corruption of whatever was
 * being piped.
 *
 * This module composes and this module counts; it never decides that anyone
 * should see the result. Placement, animation and the choice to draw at all
 * belong to the caller — `util/loader.h` is the caller that does all three.
 *
 * Ported from pickup's `src/util/progress.c`, which had already answered these
 * questions for the same ecosystem.
 */

#define SPINNER_FRAMES 4
#define SPINNER_BRAILLE_FRAMES 10

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

/*
 * The braille spinner, composed rather than written.
 *
 * The ASCII spinner above writes itself, because the one thing it draws for
 * predates the region and lives on a line it owns by carriage return. This one
 * hands back bytes: a region paints a frame in a single write (`util/viewport.h`),
 * and a spinner that wrote itself could not be part of one.
 *
 * Ten frames rather than four, and glyphs rather than punctuation: the dots
 * travel around the cell, so the eye reads motion instead of a character being
 * replaced. It costs a terminal with a font that has U+2800..U+28FF, which is
 * the same bet the bar already makes on U+2588.
 */

/* Bytes enough for a composed line carrying a label of `label_bytes` bytes: the
   label itself, the three-byte glyph and the space after it, and the two
   sequences that cost bytes and occupy no columns at all. */
#define SPINNER_BRAILLE_SIZE(label_bytes) ((label_bytes) + 32)

/*
 * Write frame `frame` followed by `label` into `out`, and return the bytes
 * written, excluding the terminator.
 *
 * One line and nothing that would end it: no carriage return, no newline, no
 * figure. `frame` may grow without bound; it is taken modulo the frame count.
 *
 * With `colour`, the glyph alone is painted — the escape opens before it and
 * closes before the space, so the label is rendered in whatever the terminal
 * was already using. A spinner is Molto talking; the label is the work's own
 * name, and colouring it would claim it means something it does not.
 *
 * Returns 0 and writes an empty string when `out` is too small to hold the
 * whole line, the same contract `progress_bar_render` keeps: a line cut in the
 * middle of a glyph or of an escape is worse than no line.
 */
size_t spinner_braille_render(char *out, size_t size, size_t frame, const char *label, bool colour);

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
