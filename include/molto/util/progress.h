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
