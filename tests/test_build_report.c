#include <moltest.h>

#include <molto/build/report.h>

#include <stdio.h>
#include <string.h>

/*
 * What a build says, read off a temporary file.
 *
 * A tmpfile is not a terminal, which is exactly the case worth pinning: no
 * bar, no escape sequences, and the same lines a person would have read. The
 * interactive half is the bar, and the bar's arithmetic is pinned in
 * test_progress.c where it has no I/O to hide behind.
 */

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
