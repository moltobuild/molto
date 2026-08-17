#include <molto/build/report.h>

#include <molto/util/ansi.h>
#include <molto/util/progress.h>
#include <molto/util/viewport.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

/* The widest the bar is drawn. Wide enough for a percentage to be visible in
   it, narrow enough to leave the figure room on an eighty-column terminal —
   and on a narrower one it gives up columns rather than the figure, because
   what the bar is worth is the number beside it. */
#define BAR_CELLS 32

/* Columns the figure to the right of the bar needs: " 100%  9999/9999". */
#define BAR_FIGURE_COLUMNS 18

/* Below this the bar is a token rather than a measurement, but a terminal that
   narrow has bigger problems than a short bar. */
#define BAR_MIN_CELLS 4

/* Rows of in-flight work the region will show at once, however many are
   running. Eight is enough to see the build breathing and short enough to
   leave the screen to whatever was on it. */
#define FRAME_FILES_MAX 8

/* Those, the line that counts what did not fit, and the bar. */
#define FRAME_ROWS_MAX (FRAME_FILES_MAX + 2)

/* One row of the region, before the terminal's width cuts it down. A source
   path shown relative to the project fits many times over. */
#define FRAME_LINE_MAX 512

/* How often the bar is redrawn while the build runs. It is also what repairs
   the line after a compiler diagnostic scrolled over it, so it ticks on a
   build where nothing is finishing. */
#define DRAW_INTERVAL_NS 50000000L /* 50 ms */

/* Columns the origin word occupies, padding included, so every name in the
   inventory starts in the same place. The longest word is "registry". */
#define ORIGIN_FIELD 11

/* A line holds a source path, which the filesystem allows to be long. Longer
   than this is truncated: an inventory is a report, and a report that refuses
   to print is worse than one that abbreviates. */
#define LINE_MAX 4352

/* " v" and a version, with room for the escapes that dim it. */
#define VERSION_FIELD_MAX 128

/* What every origin looks like on a line. Indexed by build_origin, so adding
   one to the enum without a look here fails to compile rather than printing
   nothing. */
static const struct {
    const char *glyph;
    const char *word;
    const char *colour;
} origin_style[] = {
    [build_origin_registry] = {"◆", "registry", ANSI_CYAN},
    [build_origin_module] = {"◇", "modules", ANSI_MAGENTA},
    [build_origin_project] = {"●", "project", ANSI_GREEN},
    [build_origin_tests] = {"◐", "tests", ANSI_BLUE},
};

#define ORIGIN_COUNT (sizeof origin_style / sizeof origin_style[0])

/* The line that stands for everything this build did not have to do. */
#define CACHED_GLYPH "○"
#define CACHED_WORD "cached"

/* One line of the inventory. The strings are copied rather than borrowed: the
   report outlives nothing in particular, and a build plan that freed itself
   early would otherwise be discovered as a printed line of rubbish. */
typedef struct {
    build_origin origin;
    char *name;
    char *version; /* NULL when the origin carries none */
} report_entry;

/* How many compilations the region will keep track of at once. Fixed rather
   than taken from `-j`, because the report is created by three commands that
   do not know it, and what is actually running at once is bounded by the
   cores of the machine. A build wider than this loses nothing but the names:
   the bar counts the total, not this table. */
#define INFLIGHT_MAX 64

/* One unit being compiled right now. Copied and not borrowed, for the reason
   the inventory copies — and so that finding a slot never has to allocate
   while the lock is held. `busy` false is a free slot. */
typedef struct {
    bool busy;
    build_origin origin;
    char name[128];    /* the package, or "" for the project's own code */
    char display[256]; /* the source, as a person would name it */
    uint64_t sequence; /* the order it arrived in, so a row does not jump */
} inflight_slot;

struct build_report {
    FILE *out;
    /* Whether a person is watching, and whether they want colour. The bar is
       the first; the escapes are both. */
    bool interactive;
    bool colour;

    report_entry *entries;
    size_t entry_count;
    size_t skipped;

    size_t total;
    atomic_size_t done;

    /* What is being compiled at this instant. Guarded by the lock below: the
       workers write it and the drawer reads it. `high` is one past the
       furthest slot ever used, so a frame sweeps as many slots as the build is
       wide rather than as many as the table holds. */
    inflight_slot inflight[INFLIGHT_MAX];
    size_t inflight_high;
    size_t inflight_busy;
    uint64_t inflight_next;

    /* Held around every write to `out`, because taking the bar off the line,
       writing, and putting it back is one act and the drawer is another
       thread. */
    mtx_t lock;

    viewport view;
    bool frame_up; /* the region is on the screen */

    atomic_bool drawing; /* cleared to ask the drawer to stop */
    bool running;        /* whether there is a drawer to join */
    thrd_t drawer;

    struct timespec started;
};

/* --- the pieces of a line --- */

static const char *paint(const build_report *report, const char *colour) {
    return report->colour ? colour : "";
}

/* Seconds since the report was created, on the clock that does not jump. */
static double elapsed_seconds(const build_report *report) {
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;
    const double seconds = (double)(now.tv_sec - report->started.tv_sec);
    const double nanos = (double)(now.tv_nsec - report->started.tv_nsec);
    return seconds + nanos / 1e9;
}

/* One line with the shape of an inventory line: a glyph in the origin's
   colour, the origin word in its padded field, the name, and a version where
   there is one. Returns the bytes written, terminator excluded.

   The padding is counted over the word alone and never over the composed line,
   because a glyph is three bytes and one column and an escape sequence is
   several bytes and no columns at all.

   Composed rather than printed, because the same shape is wanted in two
   places: the inventory writes it out, and the region collects a handful of
   them into a frame. No newline, for the same reason — the inventory adds one
   and a row of the region must not have one. */
static size_t compose_line(const build_report *report, char *out, size_t out_size,
                           const char *colour, const char *glyph, const char *word,
                           const char *name, const char *version) {
    const size_t width = strlen(word);
    const size_t pad = width < ORIGIN_FIELD ? ORIGIN_FIELD - width : 1;
    char spaces[ORIGIN_FIELD + 1];
    memset(spaces, ' ', pad);
    spaces[pad] = '\0';

    char tail[VERSION_FIELD_MAX] = "";
    if(version != NULL)
        (void)snprintf(tail, sizeof tail, "%s%s%s", paint(report, ANSI_DIM), version,
                       paint(report, ANSI_RESET));

    const int written = snprintf(out, out_size, " %s%s%s %s%s%s%s%s%s", paint(report, colour),
                                 glyph, paint(report, ANSI_RESET), paint(report, ANSI_DIM), word,
                                 paint(report, ANSI_RESET), spaces, name, tail);
    if(written < 0)
        return 0;
    return (size_t)written < out_size ? (size_t)written : out_size - 1;
}

static void write_line(build_report *report, const char *colour, const char *glyph,
                       const char *word, const char *name, const char *version) {
    char line[LINE_MAX];
    (void)compose_line(report, line, sizeof line, colour, glyph, word, name, version);
    (void)fputs(line, report->out);
    (void)fputc('\n', report->out);
}

static void write_entry(build_report *report, const report_entry *entry) {
    char version[VERSION_FIELD_MAX] = "";
    if(entry->version != NULL)
        (void)snprintf(version, sizeof version, " v%s", entry->version);
    write_line(report, origin_style[entry->origin].colour, origin_style[entry->origin].glyph,
               origin_style[entry->origin].word, entry->name,
               entry->version != NULL ? version : NULL);
}

static void write_cached(build_report *report) {
    char count[64];
    (void)snprintf(count, sizeof count, "%zu file%s", report->skipped,
                   report->skipped == 1 ? "" : "s");
    write_line(report, ANSI_DIM, CACHED_GLYPH, CACHED_WORD, count, NULL);
}

/*
 * Whether an origin is listed a source at a time or counted in one line.
 *
 * The two that are listed are the two whose lines are sources rather than
 * packages, and they are exactly the two the region is about to name one by
 * one as it compiles them. Saying them twice is filling the scrollback with
 * what the region says better and then throwing the region away.
 *
 * Only on a terminal. A pipe and a log file have no region to defer to, and
 * there the line per source is the whole of the record.
 */
static bool counted_not_listed(const build_report *report, build_origin origin) {
    if(!report->interactive)
        return false;
    return origin == build_origin_project || origin == build_origin_tests;
}

/* One line for everything an origin contributes, in the shape the `cached`
   line already had — which is the sign that counting rather than listing is
   something this report was already doing. */
static void write_origin_count(build_report *report, build_origin origin, size_t files) {
    char count[64];
    (void)snprintf(count, sizeof count, "%zu file%s", files, files == 1 ? "" : "s");
    write_line(report, origin_style[origin].colour, origin_style[origin].glyph,
               origin_style[origin].word, count, NULL);
}

/* --- the region --- */

/* Cells the bar may have on a terminal this wide. It gives up columns before
   it gives up the figure: a bar with no percentage beside it says only that
   something is happening, which the fact that it is moving already said. */
static size_t bar_cells(size_t columns) {
    if(columns <= BAR_FIGURE_COLUMNS + BAR_MIN_CELLS)
        return BAR_MIN_CELLS;
    const size_t room = columns - BAR_FIGURE_COLUMNS - 1;
    return room < BAR_CELLS ? room : BAR_CELLS;
}

/* The `want` oldest units still compiling, oldest first, assuming the lock is
   held. Ordered by arrival and not by slot, so a row does not jump to a
   different file because the slot beside it was reused. An insertion into a
   list of at most eight beats sorting sixty-four. */
static size_t oldest_busy(const build_report *report, const inflight_slot **picked, size_t want) {
    size_t count = 0;
    for(size_t i = 0; i < report->inflight_high; i++) {
        const inflight_slot *entry = &report->inflight[i];
        if(!entry->busy)
            continue;
        size_t at = count;
        while(at > 0 && picked[at - 1]->sequence > entry->sequence)
            at--;
        if(at >= want)
            continue;
        for(size_t j = count < want ? count : want - 1; j > at; j--)
            picked[j] = picked[j - 1];
        picked[at] = entry;
        if(count < want)
            count++;
    }
    return count;
}

/* Every row the region shows this frame — what is being compiled now, what did
   not fit, and the bar — assuming the lock is held. It composes and does not
   write, which is also why it may be called with the lock: nothing here
   re-enters the report. */
static size_t compose_frame(build_report *report, char rows[][FRAME_LINE_MAX], viewport_size size) {
    const size_t height = viewport_height(report->inflight_busy, size.rows, FRAME_FILES_MAX);
    const inflight_slot *picked[FRAME_FILES_MAX];
    const size_t shown = oldest_busy(report, picked, height);

    size_t used = 0;
    for(; used < shown; used++) {
        const inflight_slot *entry = picked[used];
        /* The field says who the file belongs to: a package by name, and the
           project's own code by the word the inventory gave it. */
        const char *word = entry->name[0] != '\0' ? entry->name : origin_style[entry->origin].word;
        (void)compose_line(report, rows[used], FRAME_LINE_MAX, origin_style[entry->origin].colour,
                           origin_style[entry->origin].glyph, word, entry->display, NULL);
    }

    if(report->inflight_busy > shown) {
        (void)snprintf(rows[used], FRAME_LINE_MAX, " %s… and %zu more%s", paint(report, ANSI_DIM),
                       report->inflight_busy - shown, paint(report, ANSI_RESET));
        used++;
    }

    char cells[PROGRESS_BAR_SIZE(BAR_CELLS)];
    const size_t done = atomic_load(&report->done);
    (void)progress_bar_render(cells, sizeof cells, done, report->total, bar_cells(size.columns));
    (void)snprintf(rows[used], FRAME_LINE_MAX, " %s %s%3zu%%  %zu/%zu%s", cells,
                   paint(report, ANSI_DIM), progress_bar_percent(done, report->total), done,
                   report->total, paint(report, ANSI_RESET));
    return used + 1;
}

/* Draw the region where the last one was, assuming the lock is held. The
   terminal is measured every frame rather than once: a window dragged narrower
   halfway through a build is ordinary, and a row that wraps takes the region's
   arithmetic with it. */
static void draw_frame_locked(build_report *report) {
    if(!report->interactive || report->total == 0)
        return;
    const viewport_size size = viewport_measure(report->out);
    char rows[FRAME_ROWS_MAX][FRAME_LINE_MAX];
    const char *lines[FRAME_ROWS_MAX];
    const size_t count = compose_frame(report, rows, size);
    for(size_t i = 0; i < count; i++)
        lines[i] = rows[i];
    viewport_paint(&report->view, lines, count, size);
    report->frame_up = count > 0;
}

/* Take the region off the screen, assuming the lock is held. */
static void erase_frame_locked(build_report *report) {
    if(!report->frame_up)
        return;
    viewport_clear(&report->view);
    report->frame_up = false;
}

/*
 * The drawer.
 *
 * It never decides it is finished. A worker that was never submitted, or a pass
 * that a failure stopped, leaves the counter short of the total for good — so
 * "done == total" is not a termination condition, and the only way out is the
 * flag `build_report_finish` clears.
 */
static int drawer_run(void *arg) {
    build_report *report = arg;
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = DRAW_INTERVAL_NS};
    while(atomic_load(&report->drawing)) {
        (void)mtx_lock(&report->lock);
        draw_frame_locked(report);
        (void)mtx_unlock(&report->lock);
        (void)thrd_sleep(&interval, NULL);
    }
    return 0;
}

static void stop_drawer(build_report *report) {
    if(!report->running)
        return;
    atomic_store(&report->drawing, false);
    (void)thrd_join(report->drawer, NULL);
    report->running = false;
}

/* --- the report --- */

build_report *build_report_create(FILE *out) {
    if(out == NULL)
        return NULL;
    build_report *report = calloc(1, sizeof *report);
    if(report == NULL)
        return NULL;
    if(mtx_init(&report->lock, mtx_plain) != thrd_success) {
        free(report);
        return NULL;
    }
    report->out = out;
    report->interactive = progress_is_interactive(out);
    /* A terminal is what makes a region worth drawing; NO_COLOR is the person
       at that terminal saying they would rather read it plain. */
    report->colour = report->interactive && getenv("NO_COLOR") == NULL;
    viewport_init(&report->view, out);
    atomic_init(&report->done, 0);
    atomic_init(&report->drawing, false);
    (void)clock_gettime(CLOCK_MONOTONIC, &report->started);
    return report;
}

void build_report_destroy(build_report *report) {
    if(report == NULL)
        return;
    stop_drawer(report);
    for(size_t i = 0; i < report->entry_count; i++) {
        free(report->entries[i].name);
        free(report->entries[i].version);
    }
    free(report->entries);
    viewport_free(&report->view);
    mtx_destroy(&report->lock);
    free(report);
}

bool build_report_wants_colour(const build_report *report) {
    return report != NULL && report->colour;
}

void build_report_force_interactive(build_report *report) {
    if(report != NULL)
        report->interactive = true;
}

/* Whether this package already has a line. Only a package can: two sources are
   two pieces of work even when they belong to the same thing. */
static bool already_listed(const build_report *report, const build_unit_label *label) {
    if(label->name == NULL)
        return false;
    for(size_t i = 0; i < report->entry_count; i++) {
        const report_entry *entry = &report->entries[i];
        if(entry->origin == label->origin && strcmp(entry->name, label->name) == 0)
            return true;
    }
    return false;
}

void build_report_will_compile(build_report *report, const build_unit_label *label,
                               const char *display) {
    if(report == NULL || label == NULL)
        return;
    if(already_listed(report, label))
        return;
    const char *name = label->name != NULL ? label->name : display;
    if(name == NULL)
        return;

    report_entry *grown = realloc(report->entries, (report->entry_count + 1) * sizeof *grown);
    if(grown == NULL)
        return; /* an inventory is a report; losing a line of it fails nothing */
    report->entries = grown;
    report_entry *entry = &report->entries[report->entry_count];
    entry->origin = label->origin;
    entry->name = strdup(name);
    entry->version = label->version != NULL ? strdup(label->version) : NULL;
    if(entry->name == NULL) {
        free(entry->version);
        return;
    }
    report->entry_count++;
}

void build_report_skipped(build_report *report) {
    if(report != NULL)
        report->skipped++;
}

void build_report_begin(build_report *report, size_t total) {
    if(report == NULL)
        return;
    report->total = total;
    atomic_store(&report->done, 0);

    (void)mtx_lock(&report->lock);
    /* Grouped by origin rather than printed in the order the passes were
       planned: what a dependency contributes and what the tests do are two
       different things to read, and the build's own order interleaves them. */
    for(size_t origin = 0; origin < ORIGIN_COUNT; origin++) {
        size_t files = 0;
        for(size_t i = 0; i < report->entry_count; i++) {
            if(report->entries[i].origin != (build_origin)origin)
                continue;
            files++;
            if(!counted_not_listed(report, (build_origin)origin))
                write_entry(report, &report->entries[i]);
        }
        if(files > 0 && counted_not_listed(report, (build_origin)origin))
            write_origin_count(report, (build_origin)origin, files);
    }
    if(report->skipped > 0)
        write_cached(report);
    if(report->entry_count > 0 || report->skipped > 0)
        (void)fputc('\n', report->out);
    (void)fflush(report->out);

    const bool draw = report->interactive && total > 0;
    if(draw) {
        draw_frame_locked(report);
        atomic_store(&report->drawing, true);
        report->running = thrd_create(&report->drawer, drawer_run, report) == thrd_success;
        if(!report->running)
            atomic_store(&report->drawing, false);
    }
    (void)mtx_unlock(&report->lock);
}

/* The first free slot, or INFLIGHT_MAX when the build is wider than the table.
   Assumes the lock is held. */
static size_t free_slot(const build_report *report) {
    size_t slot = 0;
    while(slot < INFLIGHT_MAX && report->inflight[slot].busy)
        slot++;
    return slot;
}

build_report_slot build_report_unit_started(build_report *report, const build_unit_label *label,
                                            const char *display) {
    /* Nobody watching is nothing to show: a stream with no region does not pay
       for the copy, the lock, or the table. */
    if(report == NULL || !report->interactive)
        return BUILD_REPORT_NO_SLOT;

    (void)mtx_lock(&report->lock);
    const size_t slot = free_slot(report);
    if(slot < INFLIGHT_MAX) {
        inflight_slot *entry = &report->inflight[slot];
        entry->busy = true;
        entry->origin = label != NULL ? label->origin : build_origin_project;
        (void)snprintf(entry->name, sizeof entry->name, "%s",
                       label != NULL && label->name != NULL ? label->name : "");
        (void)snprintf(entry->display, sizeof entry->display, "%s", display != NULL ? display : "");
        entry->sequence = report->inflight_next++;
        report->inflight_busy++;
        if(slot + 1 > report->inflight_high)
            report->inflight_high = slot + 1;
    }
    (void)mtx_unlock(&report->lock);
    return slot < INFLIGHT_MAX ? slot : BUILD_REPORT_NO_SLOT;
}

void build_report_unit_done(build_report *report, build_report_slot slot) {
    if(report == NULL)
        return;
    /* Under the lock, which this used not to need: the slot it gives back is
       one the drawer reads. What it costs is a mutex nobody is holding, next
       to a compilation that has just finished forking a compiler. */
    (void)mtx_lock(&report->lock);
    if(slot < INFLIGHT_MAX && report->inflight[slot].busy) {
        report->inflight[slot].busy = false;
        report->inflight_busy--;
    }
    /* Counted with the slot already cleared, so no frame can show a unit
       tallied as done and still listed as running. */
    (void)atomic_fetch_add(&report->done, 1);
    (void)mtx_unlock(&report->lock);
}

void build_report_message(build_report *report, const char *format, ...) {
    va_list args;
    va_start(args, format);
    /* No report is still a message: what this carries is content, and only the
       bar around it was ever decoration. */
    if(report == NULL) {
        (void)vfprintf(stderr, format, args);
        va_end(args);
        return;
    }
    (void)mtx_lock(&report->lock);
    const bool redraw = report->frame_up;
    erase_frame_locked(report);
    (void)vfprintf(report->out, format, args);
    if(redraw)
        draw_frame_locked(report);
    else
        (void)fflush(report->out);
    (void)mtx_unlock(&report->lock);
    va_end(args);
}

void build_report_finish(build_report *report, const char *profile, molto_exit_code code) {
    if(report == NULL)
        return;
    stop_drawer(report);
    const bool ok = code == exit_ok;

    (void)mtx_lock(&report->lock);
    /* The region goes, whether the build worked or not.
     *
     * A list of files being compiled stops being true the instant the build
     * stops, and a bar left behind reads as a claim about the run: a full one
     * over a failure claims it worked, and a full one over the tick below says
     * a second time what the tick already said. What survives is the verdict,
     * on the line the region was standing on. */
    erase_frame_locked(report);
    char line[LINE_MAX];
    if(ok) {
        (void)snprintf(line, sizeof line, " %s✓%s Finished `%s` build in %.2fs\n",
                       paint(report, ANSI_GREEN), paint(report, ANSI_RESET), profile,
                       elapsed_seconds(report));
        (void)fputs(line, report->out);
    } else if(code == exit_build_failure) {
        /* Flush left, unlike everything above it, because it is not one more
           finding: it is the verdict on all of them. */
        (void)snprintf(line, sizeof line, "%s%serror%s: build failed\n", paint(report, ANSI_BOLD),
                       paint(report, ANSI_RED), paint(report, ANSI_RESET));
        (void)fputs(line, report->out);
    }
    (void)fflush(report->out);
    (void)mtx_unlock(&report->lock);
}
