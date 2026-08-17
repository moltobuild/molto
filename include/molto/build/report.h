#ifndef MOLTO_BUILD_REPORT_H
#define MOLTO_BUILD_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * What a build says while it runs, and what it says when it stops.
 *
 * This is the presentation half of a build: the build service reports facts —
 * this unit will be compiled, that one was already up to date, one more has
 * finished — and every decision about words, glyphs, colour, alignment and
 * whether to draw at all is taken here. Nothing below `src/services` composes a
 * line, and nothing here knows what a translation unit is.
 *
 * Three rules shape it.
 *
 * **A line is work.** A package with forty stale sources is one piece of work
 * and one line; a source of the project's own is a line of its own, because
 * that is the granularity someone editing it thinks in. What was already up to
 * date is counted, not listed: a hundred lines saying nothing happened is not a
 * report.
 *
 * **The bar has an honest denominator.** It is the number of units this build
 * will actually compile, which is why the build plans every pass before running
 * one. A bar over a total that grows is a bar that lies.
 *
 * **Everything goes to stderr, and the extras only when a person is there.** A
 * bar in a log file is noise and a bar in a pipe is corruption, so a
 * non-terminal gets the same lines with no bar and no escape sequences.
 * NO_COLOR speaks for the person even when the terminal does not.
 */

/* Which part of a build a unit belongs to, and — through it — the word and the
   glyph the line carries. */
typedef enum {
    build_origin_registry, /* a package a registry answered for */
    build_origin_module,   /* a path, git or archive dependency */
    build_origin_project,  /* the project's own sources */
    build_origin_tests,    /* a source compiled into the test binaries */
} build_origin;

/*
 * Who a unit belongs to, as the report will name it.
 *
 * The strings are borrowed: they live in the build plan, which outlives every
 * report of them. `name` is NULL for the project's own code, which is named by
 * its source rather than by a package; `version` is NULL when the origin
 * carries none, as a path dependency does — its bytes are whatever is on disk,
 * and no version answers for them.
 */
typedef struct {
    build_origin origin;
    const char *name;
    const char *version;
} build_unit_label;

typedef struct build_report build_report;

/*
 * A report drawing on `out`.
 *
 * Whether that stream reaches a person, and whether colour is welcome, is
 * settled here and once. NULL when the allocation failed — which a caller may
 * simply carry on with, because every function below takes NULL to mean "say
 * nothing". That is also how the test suite builds hundreds of projects in
 * silence.
 */
[[nodiscard]] build_report *build_report_create(FILE *out);
void build_report_destroy(build_report *report);

/* Whether escapes are welcome on this report's stream. Asked by whoever
   composes a block of text to hand to `build_report_message`, which cannot
   colour what it is given and must not be handed colour that is not wanted.
   No report means no terminal to answer for, so: plain. */
[[nodiscard]] bool build_report_wants_colour(const build_report *report);

/* One unit this build is going to compile. Calls naming the same package
   collapse into one line; `display` is what a line without a package name
   shows, which for the project's own code is the source. */
void build_report_will_compile(build_report *report, const build_unit_label *label,
                               const char *display);

/* One unit this build will not compile: it was already up to date, or the
   shared object cache had it. */
void build_report_skipped(build_report *report);

/* Print the inventory and put the bar up. `total` is how many units will be
   compiled — zero draws no bar, because there is nothing to watch. */
void build_report_begin(build_report *report, size_t total);

/* One unit has finished compiling, whether or not it succeeded. Safe to call
   from a worker thread, which is where it is called from. */
void build_report_unit_done(build_report *report);

/* Say something while the bar is up: it comes off the line, the message is
   printed, and it goes back. Anything a build writes to stderr between
   `begin` and `finish` goes through here, or it lands in the middle of the
   bar. */
void build_report_message(build_report *report, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Take the bar down and, if the build stands, say which profile it was and how
   long it took. A build that failed says nothing here: the line worth reading
   is the compiler's. */
void build_report_finish(build_report *report, const char *profile, bool ok);

#endif /* MOLTO_BUILD_REPORT_H */
