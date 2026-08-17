#ifndef MOLTO_BUILD_REPORT_H
#define MOLTO_BUILD_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <molto/exit_code.h>

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
 * **Everything goes to stderr, and a terminal gets a region where everything
 * else gets lines.** A bar in a log file is noise and a bar in a pipe is
 * corruption, so a non-terminal gets one line per source, no region and no
 * escape sequences at all. A terminal gets the inventory counted rather than
 * listed, and the files themselves named in a region at the bottom of the
 * screen that is redrawn as the work moves and taken away when it stops — the
 * names are worth having while the file is compiling and worth nothing
 * afterwards, and a build that leaves a hundred of them in the scrollback is
 * charging the reader for both. NO_COLOR speaks for the person even when the
 * terminal does not.
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
    /* Where this unit's sources live, absolute. NULL for the project's own
       code, whose sources live under the root everything is already shown
       relative to. Nothing on an inventory line uses it: it is what lets a
       diagnostic name the source relative to the package it belongs to rather
       than to whoever is building it. */
    const char *source;
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

/* Draw as though somebody were watching.
 *
 * This exists for the test suite, which has no terminal and needs the region
 * pinned byte for byte all the same. It does not touch colour: the escapes
 * stay off unless the stream asked for them, so what a test reads is the shape
 * of the region and not a paint job. */
void build_report_force_interactive(build_report *report);

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

/*
 * Which row of the region a unit has been given.
 *
 * Opaque, and a number rather than a pointer: a pointer into the table is a
 * pointer that dangles the moment the table moves, and it invites a caller to
 * read the table without the lock that guards it. It is handed straight back
 * to `unit_done` and means nothing anywhere else.
 */
typedef size_t build_report_slot;
#define BUILD_REPORT_NO_SLOT ((build_report_slot) - 1)

/*
 * One unit has started compiling. The region names it until the `unit_done`
 * that matches. Safe from a worker thread, which is where it is called from.
 *
 * The strings are copied and not borrowed, for the reason the inventory copies
 * them: the lifetime that would make borrowing safe is an invariant spanning
 * three files that this header can neither state nor check. A fixed table also
 * means this cannot fail for memory and never calls the allocator with the
 * lock held.
 *
 * BUILD_REPORT_NO_SLOT when there is nothing to show — no report, no terminal,
 * or more compilations at once than the table allows for. None of those is a
 * failure: the bar and the count come from the total and the tally, not from
 * this table, so a unit that finds no room simply goes unnamed.
 */
[[nodiscard]] build_report_slot
build_report_unit_started(build_report *report, const build_unit_label *label, const char *display);

/* One unit has finished compiling, whether or not it succeeded. `slot` is what
   `unit_started` returned; BUILD_REPORT_NO_SLOT is valid and frees nothing.
   Safe to call from a worker thread, which is where it is called from. */
void build_report_unit_done(build_report *report, build_report_slot slot);

/* Say something while the region is up: it comes off the screen, the message
   is printed, and it goes back below. Anything a build writes to stderr
   between `begin` and `finish` goes through here, or it lands in the middle of
   the region.
 *
 * One rule holds the locking up: nothing called with the report's lock held
 * may re-enter the report. Composing a row does not call this. */
void build_report_message(build_report *report, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Take the bar down and say how it went.
 *
 * `exit_ok` says which profile it was and how long it took. `exit_build_failure`
 * closes with `error: build failed`, under the diagnostics that already said
 * which unit and why — one line for the whole build, however many units broke.
 *
 * Every other code says nothing, because it belongs to a step before any
 * compiler ran: a manifest that would not parse, a registry that would not
 * answer. Those have already been reported in their own words, and "build
 * failed" underneath them would be naming the wrong thing. */
void build_report_finish(build_report *report, const char *profile, molto_exit_code code);

#endif /* MOLTO_BUILD_REPORT_H */
