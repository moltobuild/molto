#ifndef MOLTO_UTIL_VIEWPORT_H
#define MOLTO_UTIL_VIEWPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * A region of the screen that is redrawn rather than scrolled past.
 *
 * A build that names every file it compiles fills a terminal's history with a
 * hundred lines nobody reads twice. The lines are worth having while the file
 * is being compiled and worth nothing afterwards, which is the definition of
 * something that belongs in a region and not in a log: it is drawn at the
 * bottom of the screen, redrawn as the work moves, and taken away when the
 * work stops. What survives is the verdict.
 *
 * Two rules hold the whole thing up.
 *
 * **The region counts rows of the terminal.** Moving back to the top of it is
 * a cursor moved up by a number of rows, so a line that occupies two rows
 * instead of one puts every later move in the wrong place — and the region,
 * unable to find what it drew, leaves it on the screen and draws over
 * something else. That is why every line is cut to the terminal's width before
 * it is written, and why the cut counts columns rather than bytes. Truncation
 * here is correctness, not tidiness.
 *
 * **Nothing here asks whether anyone is watching.** It writes what it is given
 * to the stream it is given, and the decision to draw at all belongs to the
 * caller, who has already made it once — the same division `ansi.h` draws for
 * colour. It is also what lets the drawing half be pinned byte for byte
 * against a temporary file.
 *
 * The cursor is never hidden. Doing it properly means restoring it from a
 * signal handler — Ctrl-C during a long build is the common case, not the
 * exotic one — and that is a terminal-wide piece of state to own in a process
 * that forks compilers. The cost of not hiding it is a cursor parked at the
 * end of the last row, which is where the bar already left it.
 */

/* What a terminal is taken to be when nothing will say. Eighty by twenty-four
   is the size of the terminal that everything else was designed against. */
#define VIEWPORT_FALLBACK_COLUMNS 80
#define VIEWPORT_FALLBACK_ROWS 24

/* The largest terminal this will believe in. A COLUMNS somebody exported by
   hand years ago is not a measurement, and sizing a buffer against it is
   sizing a buffer against whatever that number happens to be. */
#define VIEWPORT_COLUMNS_MAX 1024
#define VIEWPORT_ROWS_MAX 256

typedef struct {
    size_t columns;
    size_t rows;
} viewport_size;

/* What two environment variables describe, or the fallback where they do not.
   Pure, with the ioctl sitting on top of it. A value that is not a number, or
   that is zero, is not an answer: it is a variable somebody exported and
   nobody updated. Anything larger than the maxima above is taken as those. */
[[nodiscard]] viewport_size viewport_size_from_env(const char *columns, const char *lines);

/* How many rows and columns the terminal behind `out` has.
 *
 * Asked once per frame rather than once per build: a window dragged narrower
 * halfway through is the ordinary case, and a region drawn against the width
 * of a minute ago wraps — which is the failure the whole module is arranged to
 * avoid. It costs one ioctl per frame and buys not having to own SIGWINCH. */
[[nodiscard]] viewport_size viewport_measure(FILE *out);

/* Bytes enough for a line of `columns` columns: four bytes per column for the
   characters, plus room for the sequences that cost bytes and no columns at
   all, plus the reset that closes a line cut in the middle of colour. */
#define VIEWPORT_FIT_SIZE(columns) ((columns) * 4 + 64)

/*
 * Copy at most `columns` columns of `line` into `out`, and return the bytes
 * written, terminator excluded.
 *
 * It counts columns and not bytes, and counts them the way `text_columns`
 * counts them: a UTF-8 character is one column however many bytes it takes,
 * and an SGR sequence is no columns however many bytes it takes. Neither a cut
 * through the middle of a character nor a cut through the middle of a sequence
 * is possible — each is taken whole or not taken. An escape that is malformed
 * or that the line ends in the middle of is dropped rather than passed on.
 *
 * A cut line that had opened colour closes it with ANSI_RESET, because the cut
 * took away the reset the line was carrying and the colour would otherwise run
 * on into the row below and out onto the prompt when the build ends. A cut
 * line that opened none gains no escape at all: on a stream without colour the
 * region writes none.
 *
 * The count is a character count and not a display width, inherited from
 * `text_columns` along with its limitation: a path in CJK or an emoji in a
 * filename measures short, and a row carrying one may wrap by a column or two.
 * Doing better means wcwidth, which means a locale, which Molto never sets.
 *
 * `line` holds no newlines. One row of the region is one row.
 *
 * Returns 0 and writes the empty string when `out` is smaller than
 * VIEWPORT_FIT_SIZE(columns) — the same contract `progress_bar_render` keeps,
 * and for the same reason: a buffer sized by the column count is a buffer that
 * would be overrun by the third character outside ASCII.
 */
size_t viewport_fit(const char *line, size_t columns, char *out, size_t out_size);

/*
 * How many rows of in-flight work the region may show.
 *
 * The smallest of what there is, what the caller will allow, and a third of
 * the screen. The third is not taste: it is what guarantees the rows, the line
 * that counts what did not fit, and the bar all fit on the screen at once, and
 * a region taller than the screen scrolls its own anchor away. On a screen too
 * short to give up three rows the answer is none of them — what shrinks is the
 * list of files, and the bar always survives.
 */
[[nodiscard]] size_t viewport_height(size_t in_flight, size_t rows, size_t maximum);

#endif /* MOLTO_UTIL_VIEWPORT_H */
