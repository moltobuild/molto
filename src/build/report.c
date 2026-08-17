#include <molto/build/report.h>

#include <molto/util/progress.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

/* Columns of the bar. Wide enough for a percentage to be visible in it, narrow
   enough to leave the figure room on an eighty-column terminal. */
#define BAR_CELLS 32

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

#define ANSI_RESET "\033[0m"
#define ANSI_DIM "\033[2m"
#define ANSI_GREEN "\033[32m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"

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

    /* Held around every write to `out`, because taking the bar off the line,
       writing, and putting it back is one act and the drawer is another
       thread. */
    mtx_t lock;
    bool bar_up; /* the cursor is sitting on a drawn bar */

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

/* One inventory line: a glyph in the origin's colour, the origin word in its
   padded field, the name, and a version where there is one.

   The padding is counted over the word alone and never over the composed line,
   because a glyph is three bytes and one column and an escape sequence is
   several bytes and no columns at all. */
static void write_line(build_report *report, const char *colour, const char *glyph,
                       const char *word, const char *name, const char *version) {
    const size_t width = strlen(word);
    const size_t pad = width < ORIGIN_FIELD ? ORIGIN_FIELD - width : 1;
    char spaces[ORIGIN_FIELD + 1];
    memset(spaces, ' ', pad);
    spaces[pad] = '\0';

    char tail[VERSION_FIELD_MAX] = "";
    if(version != NULL)
        (void)snprintf(tail, sizeof tail, "%s%s%s", paint(report, ANSI_DIM), version,
                       paint(report, ANSI_RESET));

    char line[LINE_MAX];
    (void)snprintf(line, sizeof line, " %s%s%s %s%s%s%s%s%s\n", paint(report, colour), glyph,
                   paint(report, ANSI_RESET), paint(report, ANSI_DIM), word,
                   paint(report, ANSI_RESET), spaces, name, tail);
    (void)fputs(line, report->out);
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

/* --- the bar --- */

/* Draw the bar where the cursor is, assuming the lock is held. */
static void draw_bar_locked(build_report *report) {
    char cells[PROGRESS_BAR_SIZE(BAR_CELLS)];
    const size_t done = atomic_load(&report->done);
    (void)progress_bar_render(cells, sizeof cells, done, report->total, BAR_CELLS);

    char line[LINE_MAX];
    (void)snprintf(line, sizeof line, " %s %s%3zu%%%s", cells, paint(report, ANSI_DIM),
                   progress_bar_percent(done, report->total), paint(report, ANSI_RESET));
    progress_erase_line(report->out);
    (void)fputs(line, report->out);
    (void)fflush(report->out);
    report->bar_up = true;
}

/* Take the bar off the line, assuming the lock is held. */
static void erase_bar_locked(build_report *report) {
    if(!report->bar_up)
        return;
    progress_erase_line(report->out);
    report->bar_up = false;
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
        draw_bar_locked(report);
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
    /* A terminal is what makes a bar worth drawing; NO_COLOR is the person at
       that terminal saying they would rather read it plain. */
    report->colour = report->interactive && getenv("NO_COLOR") == NULL;
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
    mtx_destroy(&report->lock);
    free(report);
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
        for(size_t i = 0; i < report->entry_count; i++) {
            if(report->entries[i].origin == (build_origin)origin)
                write_entry(report, &report->entries[i]);
        }
    }
    if(report->skipped > 0)
        write_cached(report);
    if(report->entry_count > 0 || report->skipped > 0)
        (void)fputc('\n', report->out);
    (void)fflush(report->out);

    const bool draw = report->interactive && total > 0;
    if(draw) {
        draw_bar_locked(report);
        atomic_store(&report->drawing, true);
        report->running = thrd_create(&report->drawer, drawer_run, report) == thrd_success;
        if(!report->running)
            atomic_store(&report->drawing, false);
    }
    (void)mtx_unlock(&report->lock);
}

void build_report_unit_done(build_report *report) {
    if(report != NULL)
        (void)atomic_fetch_add(&report->done, 1);
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
    const bool redraw = report->bar_up;
    erase_bar_locked(report);
    (void)vfprintf(report->out, format, args);
    if(redraw)
        draw_bar_locked(report);
    else
        (void)fflush(report->out);
    (void)mtx_unlock(&report->lock);
    va_end(args);
}

void build_report_finish(build_report *report, const char *profile, bool ok) {
    if(report == NULL)
        return;
    stop_drawer(report);

    (void)mtx_lock(&report->lock);
    if(ok && report->bar_up) {
        /* One last frame, so what stays on the screen is the finished bar and
           not whichever fraction the last tick happened to catch. */
        draw_bar_locked(report);
        (void)fputs("\n\n", report->out);
        report->bar_up = false;
    } else {
        /* A build that failed leaves no bar behind: every unit but one may
           well have compiled, and a full bar over a failure reads as a claim
           that it worked. */
        erase_bar_locked(report);
    }
    if(ok) {
        char line[LINE_MAX];
        (void)snprintf(line, sizeof line, " %s✓%s Finished `%s` build in %.2fs\n",
                       paint(report, ANSI_GREEN), paint(report, ANSI_RESET), profile,
                       elapsed_seconds(report));
        (void)fputs(line, report->out);
    }
    (void)fflush(report->out);
    (void)mtx_unlock(&report->lock);
}
