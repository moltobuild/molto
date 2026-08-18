#ifndef MOLTO_UTIL_LOADER_H
#define MOLTO_UTIL_LOADER_H

#include <stdio.h>

/*
 * A spinner that turns while something else blocks.
 *
 * The pieces were all here already and none of them animated anything.
 * `util/progress.h` composes a frame, `util/viewport.h` puts rows on the screen
 * and takes them away again — but between the two sat the assumption that
 * whoever was working would come up for air often enough to draw. A build does:
 * it finishes translation units, and each one is an opportunity to redraw. An
 * analysis does not. `molto lint` submits every file to a pool and blocks in
 * `task_pool_wait` until the last one is back, and a caller blocked in a
 * condition variable cannot draw anything at all.
 *
 * So the drawing is somebody else's thread. That is the same answer a build
 * arrived at for its region (`build/report.c`), reduced to the case where there
 * is nothing to report but that the work is still going.
 *
 * **The loader owns the stream while it runs.** Getting back to the row it drew
 * is a relative cursor movement, so anything else written between two frames
 * leaves the loader painting over the wrong line. Nothing here locks; the
 * caller picks a window in which it is the only writer, which for lint is the
 * pool — its workers capture what their processes say into buffers and the
 * whole of it is reported afterwards, on the main thread.
 *
 * Everything is one row, and only on a terminal. A pipe and a log file get
 * nothing, not even the escape that would clear a row nobody drew.
 */

/* The longest label this will animate. A spinner labels a phase of a command,
   not a path, so the labels are literals in the caller and this bound exists to
   size a frame buffer rather than to constrain anybody. */
#define LOADER_LABEL_MAX 128

typedef struct loader loader;

/*
 * Start animating `label` on `out`, and return the handle that stops it.
 *
 * Returns NULL when `out` is not a terminal — there is nothing to draw, and
 * saying so by returning nothing is what keeps the decision out of the caller —
 * and also when a thread or the memory for one cannot be had. Both are the same
 * thing to a caller: no loader, and `loader_stop(NULL)` does nothing. Work that
 * fails to announce itself is not work that failed.
 *
 * A label longer than LOADER_LABEL_MAX is truncated. Colour follows NO_COLOR,
 * read once here rather than once per frame.
 */
[[nodiscard]] loader *loader_start(FILE *out, const char *label);

/*
 * Stop the animation, take the row off the screen, and free the handle.
 *
 * Safe on NULL. On return the cursor sits at the start of a blank row, so
 * whatever is printed next — a diagnostic, an error, a summary — begins on a
 * clean line rather than after half a spinner.
 */
void loader_stop(loader *load);

#endif /* MOLTO_UTIL_LOADER_H */
