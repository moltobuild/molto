#ifndef MOLTO_BUILD_DIAGNOSTIC_VIEW_H
#define MOLTO_BUILD_DIAGNOSTIC_VIEW_H

#include <stdbool.h>
#include <stdio.h>

#include <molto/build/diagnostic.h>

/*
 * What a compiler said, drawn.
 *
 * `diagnostic_write_text` states a diagnostic in one line, which is the right
 * shape for a file another program will read. This states it in a frame, which
 * is the right shape for a person: the source line the compiler pointed at, a
 * caret under the character it named, and a footer saying whose code that was
 * and which compiler was asked to build it.
 *
 * Three things are worth knowing about how it works.
 *
 * **It draws the caret itself.** The compiler drew one too, and its version is
 * discarded here rather than suppressed at the command line — gcc and clang
 * spell that flag differently, clang-tidy takes neither, and the flag would
 * land in the fingerprint that decides whether an object is stale, so asking
 * for it would rebuild the world to change how a message looks. A caret line
 * is unmistakable on the page, so it is recognised there instead.
 *
 * **Nothing is thrown away.** A line the parser could not read is printed as
 * the tool wrote it, and diagnostics past the frame limit fall back to the one
 * line form. A frame is a way of reading output, never a filter on it.
 *
 * **It composes rather than prints.** A build renders the whole block into one
 * string and hands it to `build_report_message` in a single call, because that
 * is what makes it atomic against the progress bar and against the other
 * workers. `lint`, with no bar to work around, writes straight out.
 */

/* What was being done, which decides the words the header uses. Compiling is
   the default, so a context that says nothing about it is about a source. */
typedef enum {
    diagnostic_view_compiling,
    diagnostic_view_linking,
    /* Analysis, which is not a build: nothing was produced and nothing was
       stopped, so a finding says what it is and not what it cost. */
    diagnostic_view_checking,
} diagnostic_view_action;

/* Whose code this is, as the footer will say it. Every field is borrowed, and
   every one of them may be NULL: a line that has nothing to say is omitted
   rather than printed empty. */
typedef struct {
    /* What failed, named the way the build's inventory named it. */
    const char *unit;
    diagnostic_view_action action;
    /* The package it belongs to, and its version. NULL for a project's own
       code, which is named by its source rather than by a package. */
    const char *package;
    const char *version;
    /* Where that package's sources live. Worth a line only when it is
       somewhere the reader can go and look. */
    const char *source;
    /* Which compiler was asked, as "gcc 12.3.0". */
    const char *compiler;
    /* The project root, so paths are shown the way a project looks. */
    const char *root;
} diagnostic_context;

/* Render `list` as a block of text, NUL-terminated, for the caller to free.
   NULL when there was nothing worth drawing or the allocation failed — the
   first of which is the ordinary case, since most units compile quietly. */
[[nodiscard]] char *diagnostic_view_render(const diagnostic_list *list,
                                           const diagnostic_context *ctx, bool colour);

/* The same block, written to `out`. Nothing is written when there is nothing
   to draw. */
void diagnostic_view_write(FILE *out, const diagnostic_list *list, const diagnostic_context *ctx,
                           bool colour);

#endif /* MOLTO_BUILD_DIAGNOSTIC_VIEW_H */
