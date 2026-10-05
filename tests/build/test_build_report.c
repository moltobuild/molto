#include <moltest.h>

#include <molto/build/report.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * What a build says, read off a temporary file.
 *
 * A tmpfile is not a terminal, which is exactly the case worth pinning: no
 * bar, no escape sequences, and the same lines a person would have read. The
 * interactive half is the bar, and the bar's arithmetic is pinned in
 * test_progress.c where it has no I/O to hide behind.
 */

/* What viewport_clear writes per row when it takes the region off. */
#define ERASE_ROW "\033[2K"

static size_t captured(FILE *file, char *out, size_t out_size) {
    (void)fflush(file);
    rewind(file);
    const size_t read = fread(out, 1, out_size - 1, file);
    out[read] = '\0';
    return read;
}

static const build_unit_label sqlite = {
    .origin = build_origin_registry, .name = "sqlite3", .version = "3.50.3"};
static const build_unit_label network = {.origin = build_origin_module, .name = "network"};
static const build_unit_label own = {.origin = build_origin_project};
static const build_unit_label suite = {.origin = build_origin_tests};

MOLTEST(the_inventory_names_the_work_and_where_it_came_from) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_will_compile(report, &sqlite, NULL);
    build_report_will_compile(report, &network, NULL);
    build_report_will_compile(report, &own, "main.c");
    build_report_begin(report, 3);
    build_report_finish(report, "debug", exit_ok);

    char text[1024] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NOT_NULL(strstr(text, "registry"));
    EXPECT_NOT_NULL(strstr(text, "sqlite3 v3.50.3"));
    /* An origin that carries no version states none rather than an empty one. */
    EXPECT_NOT_NULL(strstr(text, "modules"));
    EXPECT_NOT_NULL(strstr(text, "network\n"));
    EXPECT_NOT_NULL(strstr(text, "project"));
    EXPECT_NOT_NULL(strstr(text, "main.c"));

    build_report_destroy(report);
    (void)fclose(out);
}

/* Grouped by origin, not by the order the passes were planned: a test build
   plans its dependencies, then its own code, then more dependencies. */
MOLTEST(the_inventory_is_grouped_by_origin) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_will_compile(report, &own, "main.c");
    build_report_will_compile(report, &suite, "test_main.c");
    build_report_will_compile(report, &sqlite, NULL);
    build_report_begin(report, 3);

    char text[1024] = "";
    (void)captured(out, text, sizeof text);
    const char *registry = strstr(text, "sqlite3");
    const char *project = strstr(text, "main.c");
    const char *tests = strstr(text, "test_main.c");
    ASSERT_NOT_NULL(registry);
    ASSERT_NOT_NULL(project);
    ASSERT_NOT_NULL(tests);
    EXPECT_TRUE(registry < project);
    EXPECT_TRUE(project < tests);

    build_report_destroy(report);
    (void)fclose(out);
}

/* A package with forty stale sources is one piece of work; two sources of the
   project's own are two, because that is the granularity of an edit. */
MOLTEST(a_package_is_one_line_however_many_sources_it_has) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_will_compile(report, &sqlite, "btree.c");
    build_report_will_compile(report, &sqlite, "pager.c");
    build_report_will_compile(report, &sqlite, "vdbe.c");
    build_report_will_compile(report, &own, "main.c");
    build_report_will_compile(report, &own, "game.c");
    build_report_begin(report, 5);

    char text[1024] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NOT_NULL(strstr(text, "sqlite3"));
    EXPECT_NULL(strstr(text, "btree.c"));
    EXPECT_NOT_NULL(strstr(text, "main.c"));
    EXPECT_NOT_NULL(strstr(text, "game.c"));

    size_t lines = 0;
    for(const char *at = text; *at != '\0'; at++)
        lines += *at == '\n' ? 1 : 0;
    /* Three lines of inventory, and the blank that separates them from what
       comes next. */
    EXPECT_EQ(4u, lines);

    build_report_destroy(report);
    (void)fclose(out);
}

/* What was already up to date is counted rather than listed: a hundred lines
   saying nothing happened is not a report. */
MOLTEST(what_was_not_compiled_is_counted_in_one_line) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    for(size_t i = 0; i < 20; i++)
        build_report_skipped(report);
    build_report_begin(report, 0);

    char text[1024] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NOT_NULL(strstr(text, "cached"));
    EXPECT_NOT_NULL(strstr(text, "20 files"));

    build_report_destroy(report);
    (void)fclose(out);
}

MOLTEST(one_cached_file_is_not_reported_in_the_plural) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_skipped(report);
    build_report_begin(report, 0);

    char text[256] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NOT_NULL(strstr(text, "1 file\n"));

    build_report_destroy(report);
    (void)fclose(out);
}

MOLTEST(a_build_that_cached_nothing_says_nothing_about_caching) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_will_compile(report, &own, "main.c");
    build_report_begin(report, 1);

    char text[256] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NULL(strstr(text, "cached"));

    build_report_destroy(report);
    (void)fclose(out);
}

/* A bar in a log file is noise and a bar in a pipe is corruption. Neither the
   bar nor the colour it would have been drawn in reaches a stream nobody is
   watching. */
MOLTEST(a_stream_nobody_watches_gets_no_bar_and_no_escapes) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_will_compile(report, &own, "main.c");
    build_report_begin(report, 1);
    /* A unit that starts and finishes: with nobody watching it is given no
       slot, and naming it on a region there is no room for writes nothing. */
    const build_report_slot slot = build_report_unit_started(report, &own, "main.c");
    EXPECT_EQ(BUILD_REPORT_NO_SLOT, slot);
    build_report_unit_done(report, slot);
    build_report_finish(report, "debug", exit_ok);

    char text[1024] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NULL(strchr(text, '\033'));
    EXPECT_NULL(strchr(text, '\r'));
    EXPECT_NULL(strchr(text, '%'));

    build_report_destroy(report);
    (void)fclose(out);
}

MOLTEST(a_finished_build_says_which_profile_and_how_long) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_begin(report, 0);
    build_report_finish(report, "release", exit_ok);

    char text[256] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NOT_NULL(strstr(text, "Finished `release` build in "));
    EXPECT_NOT_NULL(strstr(text, "s\n"));

    build_report_destroy(report);
    (void)fclose(out);
}

/* A build that failed has already said why, in the compiler's words. A tick
   under them would be the report contradicting them; what it closes with is
   the verdict on all of them, once, however many units broke. */
MOLTEST(a_failed_build_claims_nothing_and_says_so) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_will_compile(report, &own, "main.c");
    build_report_begin(report, 1);
    build_report_finish(report, "debug", exit_build_failure);

    char text[256] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NULL(strstr(text, "Finished"));
    EXPECT_NOT_NULL(strstr(text, "main.c"));
    EXPECT_NOT_NULL(strstr(text, "error: build failed\n"));

    build_report_destroy(report);
    (void)fclose(out);
}

/* A manifest that would not parse, or a registry that would not answer, is not
   a build that failed: no compiler ever ran. Both have already been reported in
   their own words, and "build failed" underneath them names the wrong thing. */
MOLTEST(a_failure_before_the_build_is_not_reported_as_the_builds) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_begin(report, 0);
    build_report_finish(report, "debug", exit_invalid_manifest);

    char text[256] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NULL(strstr(text, "build failed"));
    EXPECT_NULL(strstr(text, "Finished"));

    build_report_destroy(report);
    (void)fclose(out);
}

MOLTEST(a_message_reaches_the_stream_whether_or_not_a_bar_is_up) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_begin(report, 1);
    build_report_message(report, "molto: failed to compile '%s'\n", "src/main.c");

    char text[256] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NOT_NULL(strstr(text, "molto: failed to compile 'src/main.c'\n"));

    build_report_destroy(report);
    (void)fclose(out);
}

/* Whoever composes a block of text has to ask, because build_report_message
   cannot colour what it is handed. A tmpfile is nobody watching, and no report
   at all is nobody to ask. */
MOLTEST(a_stream_nobody_watches_wants_no_colour) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    EXPECT_FALSE(build_report_wants_colour(report));
    EXPECT_FALSE(build_report_wants_colour(NULL));

    build_report_destroy(report);
    (void)fclose(out);
}

/*
 * The region, drawn onto a temporary file because the report was told to draw
 * as though somebody were watching.
 *
 * The ioctl cannot answer for a stream with no terminal behind it, so COLUMNS
 * and LINES are what decide the size — which means these exercise the real
 * measuring path rather than going around it.
 */

static void screen(const char *columns, const char *lines) {
    (void)setenv("COLUMNS", columns, 1);
    (void)setenv("LINES", lines, 1);
}

static void screen_forget(void) {
    (void)unsetenv("COLUMNS");
    (void)unsetenv("LINES");
}

/* The region is about to name these one at a time as it compiles them, so
   listing them here as well fills the scrollback with what the region says
   better and then throws the region away. */
MOLTEST(a_terminal_counts_the_projects_own_sources_instead_of_listing_them) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);
    build_report_force_interactive(report);
    screen("100", "40");

    build_report_will_compile(report, &sqlite, "btree.c");
    build_report_will_compile(report, &own, "main.c");
    build_report_will_compile(report, &own, "game.c");
    build_report_will_compile(report, &own, "world.c");
    build_report_will_compile(report, &suite, "test_main.c");
    build_report_begin(report, 5);

    char text[4096] = "";
    (void)captured(out, text, sizeof text);
    /* A package still names itself: its line was already one piece of work. */
    EXPECT_NOT_NULL(strstr(text, "sqlite3 v3.50.3"));
    EXPECT_NOT_NULL(strstr(text, "3 files"));
    EXPECT_NOT_NULL(strstr(text, "1 file\n"));
    EXPECT_NULL(strstr(text, "main.c"));
    EXPECT_NULL(strstr(text, "game.c"));
    EXPECT_NULL(strstr(text, "test_main.c"));

    screen_forget();
    build_report_destroy(report);
    (void)fclose(out);
}

/* And the other half of the same rule: a pipe has no region to defer to, so
   the line per source is the whole of the record and stays. */
MOLTEST(a_stream_nobody_watches_lists_every_source_it_will_compile) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);

    build_report_will_compile(report, &own, "main.c");
    build_report_will_compile(report, &own, "game.c");
    build_report_will_compile(report, &own, "world.c");
    build_report_begin(report, 3);

    char text[1024] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NOT_NULL(strstr(text, "main.c"));
    EXPECT_NOT_NULL(strstr(text, "game.c"));
    EXPECT_NOT_NULL(strstr(text, "world.c"));
    EXPECT_NULL(strstr(text, "3 files"));

    build_report_destroy(report);
    (void)fclose(out);
}

MOLTEST(a_terminal_says_which_files_are_being_compiled_now) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);
    build_report_force_interactive(report);
    screen("100", "40");

    build_report_begin(report, 2);
    const build_report_slot first = build_report_unit_started(report, &own, "main.c");
    const build_report_slot second = build_report_unit_started(report, &sqlite, "btree.c");
    EXPECT_TRUE(first != BUILD_REPORT_NO_SLOT);
    EXPECT_TRUE(second != BUILD_REPORT_NO_SLOT);
    /* Starting a unit fills the table; a frame is what puts it on the screen,
       and a message is the one thing that draws one on demand. */
    build_report_message(report, "a diagnostic\n");

    char text[4096] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NOT_NULL(strstr(text, "main.c"));
    EXPECT_NOT_NULL(strstr(text, "btree.c"));
    /* A package names itself in the field the origin word holds otherwise. */
    EXPECT_NOT_NULL(strstr(text, "sqlite3"));

    build_report_unit_done(report, first);
    build_report_unit_done(report, second);
    build_report_finish(report, "debug", exit_ok);
    screen_forget();
    build_report_destroy(report);
    (void)fclose(out);
}

/* A row is worth having while the file is compiling and worth nothing after,
   which is the whole reason it lives in a region and not in the scrollback. */
MOLTEST(a_unit_is_forgotten_when_it_finishes) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);
    build_report_force_interactive(report);
    screen("100", "40");

    build_report_begin(report, 2);
    const build_report_slot slot = build_report_unit_started(report, &own, "vanishing.c");
    build_report_unit_done(report, slot);

    (void)fflush(out);
    const long mark = ftell(out);
    build_report_message(report, "a diagnostic\n"); /* forces a fresh frame */

    char text[4096] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NULL(strstr(text + mark, "vanishing.c"));

    build_report_finish(report, "debug", exit_ok);
    screen_forget();
    build_report_destroy(report);
    (void)fclose(out);
}

MOLTEST(a_region_that_overflows_counts_what_it_does_not_show) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);
    build_report_force_interactive(report);
    screen("100", "40");

    build_report_begin(report, 12);
    char name[32];
    for(size_t i = 0; i < 12; i++) {
        (void)snprintf(name, sizeof name, "unit%zu.c", i);
        (void)build_report_unit_started(report, &own, name);
    }
    build_report_message(report, "a diagnostic\n"); /* forces a fresh frame */

    char text[8192] = "";
    (void)captured(out, text, sizeof text);
    /* Eight rows on a forty-row screen, and the four that did not fit counted
       rather than dropped. */
    EXPECT_NOT_NULL(strstr(text, "and 4 more"));
    EXPECT_NOT_NULL(strstr(text, "unit0.c"));
    EXPECT_NOT_NULL(strstr(text, "unit7.c"));
    EXPECT_NULL(strstr(text, "unit8.c"));

    screen_forget();
    build_report_destroy(report);
    (void)fclose(out);
}

/* A screen too short to spare three rows keeps the bar and gives up the files:
   a region taller than the screen scrolls its own anchor away. */
MOLTEST(a_short_screen_gives_up_the_files_and_keeps_the_bar) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);
    build_report_force_interactive(report);
    screen("100", "2");

    build_report_begin(report, 4);
    (void)build_report_unit_started(report, &own, "invisible.c");
    build_report_message(report, "a diagnostic\n");

    char text[4096] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NULL(strstr(text, "invisible.c"));
    EXPECT_NOT_NULL(strchr(text, '%'));

    screen_forget();
    build_report_destroy(report);
    (void)fclose(out);
}

/* The bar gives up columns before it gives up the figure: a bar with no number
   beside it says only that something is happening, which its moving said. */
MOLTEST(a_narrow_terminal_shortens_the_bar_and_keeps_the_figure) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);
    build_report_force_interactive(report);
    screen("40", "24");

    build_report_begin(report, 210);
    for(size_t i = 0; i < 47; i++)
        build_report_unit_done(report, BUILD_REPORT_NO_SLOT);
    build_report_message(report, "a diagnostic\n");

    char text[4096] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_NOT_NULL(strstr(text, "47/210"));
    EXPECT_NOT_NULL(strstr(text, "22%"));

    screen_forget();
    build_report_destroy(report);
    (void)fclose(out);
}

/* Nothing of the region survives the build: what stays on the screen is the
   verdict, on the line the region was standing on. */
MOLTEST(a_finished_build_leaves_only_its_verdict) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    build_report *report = build_report_create(out);
    ASSERT_NOT_NULL(report);
    build_report_force_interactive(report);
    screen("100", "40");

    build_report_begin(report, 2);
    const build_report_slot slot = build_report_unit_started(report, &own, "main.c");
    (void)fflush(out);
    const long mark = ftell(out);
    build_report_unit_done(report, slot);
    build_report_finish(report, "debug", exit_ok);

    char text[4096] = "";
    (void)captured(out, text, sizeof text);

    /* Read from the last erase and not from the mark. The drawer is a thread on
       a 50 ms tick, so between `unit_done` returning and `finish` taking the
       lock it may paint one more frame — bar and all. That frame is erased by
       the teardown, which is what this test is about; asserting on the bytes
       before the erase is asserting on a race, and it is one CI wins often
       enough to matter. */
    const char *tail = text + mark;
    for(const char *erase = tail; (erase = strstr(erase, ERASE_ROW)) != NULL;
        erase += sizeof ERASE_ROW - 1)
        tail = erase + sizeof ERASE_ROW - 1;

    EXPECT_NULL(strstr(tail, "main.c"));
    /* And no bar over the tick, saying a second time what the tick says. */
    EXPECT_NULL(strchr(tail, '%'));
    EXPECT_NOT_NULL(strstr(tail, "Finished `debug` build in "));

    screen_forget();
    build_report_destroy(report);
    (void)fclose(out);
}

/* No report at all is how the test suite builds hundreds of projects in
   silence, and how a build that could not allocate one carries on. */
MOLTEST(no_report_is_a_report_that_says_nothing) {
    build_report_will_compile(NULL, &sqlite, NULL);
    build_report_skipped(NULL);
    build_report_begin(NULL, 4);
    EXPECT_EQ(BUILD_REPORT_NO_SLOT, build_report_unit_started(NULL, &sqlite, "main.c"));
    build_report_unit_done(NULL, BUILD_REPORT_NO_SLOT);
    build_report_finish(NULL, "debug", exit_ok);
    build_report_destroy(NULL);

    EXPECT_NULL(build_report_create(NULL));
}
