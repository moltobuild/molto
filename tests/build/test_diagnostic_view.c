#include <moltest.h>

#include <molto/build/diagnostic_view.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The frame a diagnostic is drawn in.
 *
 * Every input here is a byte sequence gcc 12 or clang 14 actually produced,
 * pasted verbatim, because the whole point of this module is reading what a
 * compiler wrote and no test that invents that text proves anything about it.
 * Nothing below runs a compiler: the text is the fixture.
 */

/* A file whose contents an excerpt can be taken from. */
static bool write_source(char *path, size_t path_size, const char *content) {
    if(!moltest_temp_file("molto_view", path, path_size))
        return false;
    return fs_write_file(path, content);
}

/* Parse `text` and render it, saying which model the tool that wrote it
   counted its columns in. The parser cannot tell, so every caller does. */
static char *render_counted(const char *text, const diagnostic_context *ctx,
                            diagnostic_column_unit counted, bool colour) {
    diagnostic_list list;
    diagnostic_list_init(&list);
    if(!diagnostic_parse(text, &list)) {
        diagnostic_list_free(&list);
        return NULL;
    }
    diagnostic_list_set_columns(&list, counted);
    char *block = diagnostic_view_render(&list, ctx, colour);
    diagnostic_list_free(&list);
    return block;
}

/* Parse `text` and render it, with the excerpt taken from `path`. */
static char *render(const char *text, const diagnostic_context *ctx, bool colour) {
    return render_counted(text, ctx, diagnostic_columns_display, colour);
}

/* Parse `text` and draw it the way `molto lint` does: one block per file, over
   findings about however many files a whole run produced. */
static char *render_by_file(const char *text, const diagnostic_context *ctx) {
    diagnostic_list list;
    diagnostic_list_init(&list);
    if(!diagnostic_parse(text, &list)) {
        diagnostic_list_free(&list);
        return NULL;
    }
    char *block = diagnostic_view_render_by_file(&list, ctx, false);
    diagnostic_list_free(&list);
    return block;
}

/* What gcc 12 says about a member that does not exist, caret lines and all. */
#define GCC_MISSING_MEMBER(path)                                                                   \
    path ": In function ‘lookup’:\n" path                                                \
         ":1:13: error: ‘struct user’ has no member named ‘name’\n"             \
         "    1 |     return u->name;\n"                                                           \
         "      |             ^~\n"

MOLTEST(a_failed_unit_is_named_framed_and_accounted_for) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "    return u->name;\n"));
    char text[1024];
    snprintf(text, sizeof text, GCC_MISSING_MEMBER("%s"), path, path);

    const diagnostic_context ctx = {.unit = "src/database.c",
                                    .package = "database",
                                    .version = "1.2.0",
                                    .source = "modules/database",
                                    .compiler = "gcc 12.3.0"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "✗ Failed to compile `src/database.c`"));
    EXPECT_NOT_NULL(strstr(block, "error: compilation failed"));
    EXPECT_NOT_NULL(strstr(block, "┌─ "));
    EXPECT_NOT_NULL(strstr(block, "= dependency: database v1.2.0"));
    EXPECT_NOT_NULL(strstr(block, "= source: modules/database"));
    EXPECT_NOT_NULL(strstr(block, "= compiler: gcc 12.3.0"));

    free(block);
    (void)remove(path);
}

/* The caret goes under the character the compiler named. The compiler counts
   from one and from the start of the line; the frame counts from the rule it
   drew, and the two have to meet. */
MOLTEST(the_caret_lands_on_the_character_the_compiler_named) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "    return u->name;\n"));
    char text[1024];
    snprintf(text, sizeof text, GCC_MISSING_MEMBER("%s"), path, path);

    const diagnostic_context ctx = {.unit = "database.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    /* Column 13 is the '-' of "->": four spaces, "return", a space, "u". */
    EXPECT_NOT_NULL(strstr(block, " 1 │     return u->name;\n"));
    EXPECT_NOT_NULL(strstr(block, "   │             ^ ‘struct user’ has no member"));

    free(block);
    (void)remove(path);
}

/* A compiler drew its own excerpt and its own caret. Both are dropped, because
   this file draws them again from the source and its own are the ones that
   line up with the frame. */
MOLTEST(the_compilers_own_excerpt_and_caret_are_not_drawn_twice) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "    return u->name;\n"));
    char text[1024];
    snprintf(text, sizeof text, GCC_MISSING_MEMBER("%s"), path, path);

    const diagnostic_context ctx = {.unit = "database.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NULL(strstr(block, "^~"));  /* gcc's caret, with its range */
    EXPECT_NULL(strstr(block, "1 |"));  /* gcc's gutter */
    EXPECT_NULL(strstr(block, "In function")); /* and the function it announced */

    free(block);
    (void)remove(path);
}

/* clang draws the same pair without a gutter, so the caret is what identifies
   it and the line above it goes with it. */
MOLTEST(clangs_excerpt_and_caret_are_recognised_too) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "    return u->name;\n"));
    char text[1024];
    snprintf(text, sizeof text,
             "%s:1:15: error: no member named 'name' in 'struct user'\n"
             "    return u->name;\n"
             "           ~  ^\n"
             "1 error generated.\n",
             path);

    const diagnostic_context ctx = {.unit = "database.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NULL(strstr(block, "~  ^"));
    /* The source line appears once — inside the frame, with its rule. */
    EXPECT_NOT_NULL(strstr(block, " 1 │     return u->name;\n"));
    EXPECT_NULL(strstr(block, "\n    return u->name;\n"));

    free(block);
    (void)remove(path);
}

/* Which of your own files reached the header that broke is the one thing an
   include chain says, and it is worth saying. */
MOLTEST(an_include_chain_is_folded_into_the_locator) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "int x = \"oops\";\n"));
    char text[1024];
    snprintf(text, sizeof text,
             "In file included from diag/a.h:1,\n"
             "                 from diag/m.c:1:\n"
             "%s:1:9: error: initializer element is not computable at load time\n",
             path);

    const diagnostic_context ctx = {.unit = "src/m.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "included from diag/a.h:1\n"));
    EXPECT_NOT_NULL(strstr(block, "included from diag/m.c:1\n"));
    EXPECT_NULL(strstr(block, "In file included from"));

    free(block);
    (void)remove(path);
}

/* The ordinary case for `<command line>`, for `<built-in>`, and for a source
   generated and then removed: there is a location but nothing to read at it. */
MOLTEST(a_source_that_cannot_be_read_degrades_to_the_locator) {
    const char *text = "/tmp/molto_no_such_source_zzz.c:42:17: error: something went wrong\n";
    const diagnostic_context ctx = {.unit = "gone.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "molto_no_such_source_zzz.c:42:17"));
    EXPECT_NOT_NULL(strstr(block, "something went wrong"));
    EXPECT_NULL(strstr(block, "^"));

    free(block);
}

/* A unit that compiled is not a unit that failed, and the glyph says so before
   any of the words do. */
MOLTEST(a_unit_that_only_warned_is_not_reported_as_failed) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "    int tmp = 0;\n"));
    char text[1024];
    snprintf(text, sizeof text,
             "%s:1:9: warning: unused variable ‘tmp’ [-Wunused-variable]\n"
             "    1 |     int tmp = 0;\n"
             "      |         ^~~\n",
             path);

    const diagnostic_context ctx = {.unit = "src/parser.c", .compiler = "gcc 12.3.0"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "⚠ Warnings compiling `src/parser.c`"));
    EXPECT_NULL(strstr(block, "✗"));
    EXPECT_NOT_NULL(strstr(block, "warning[-Wunused-variable]: compiled with a warning"));

    free(block);
    (void)remove(path);
}

/* A project's own code belongs to no package, and a dependency in the shared
   cache has no path worth printing. Both lines are left out rather than
   printed empty. */
MOLTEST(the_footer_omits_what_it_has_nothing_to_say_about) {
    const char *text = "/tmp/molto_no_such_source_zzz.c:1:1: error: broken\n";
    const diagnostic_context ctx = {.unit = "src/main.c", .compiler = "gcc 12.3.0"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NULL(strstr(block, "= dependency:"));
    EXPECT_NULL(strstr(block, "= source:"));
    EXPECT_NOT_NULL(strstr(block, "= compiler: gcc 12.3.0"));

    free(block);
}

/* A package with no version — a path dependency, whose bytes are whatever is
   on disk — is named without one rather than with an empty one. */
MOLTEST(a_dependency_without_a_version_is_named_without_one) {
    const char *text = "/tmp/molto_no_such_source_zzz.c:1:1: error: broken\n";
    const diagnostic_context ctx = {.unit = "src/db.c", .package = "database"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "= dependency: database\n"));
    EXPECT_NULL(strstr(block, " v\n"));

    free(block);
}

/* A note explains the error above it, so it continues that box instead of
   announcing itself as a finding of its own. */
MOLTEST(a_note_continues_the_box_the_error_opened) {
    const char *text = "/tmp/molto_no_such_source_zzz.c:9:1: error: redefinition of 'f'\n"
                       "/tmp/molto_no_such_source_zzz.c:3:1: note: previous definition is here\n";
    const diagnostic_context ctx = {.unit = "src/main.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "┌─ ")); /* the error opened a box */
    EXPECT_NOT_NULL(strstr(block, "├─ note: ")); /* the note continued it */
    /* And it did not get a summary line of its own. */
    EXPECT_NULL(strstr(block, "note: compilation failed"));

    free(block);
}

/* A missing semicolon in C cascades. Forty frames is not more readable than
   forty lines, so past the limit the normalized form takes over — and it still
   says everything the frame would have. */
MOLTEST(past_the_frame_limit_the_one_line_form_takes_over) {
    char text[4096] = "";
    size_t used = 0;
    for(int i = 1; i <= 14; i++)
        used += (size_t)snprintf(text + used, sizeof text - used,
                                 "/tmp/molto_no_such_source_zzz.c:%d:1: error: cascade %d\n", i, i);

    const diagnostic_context ctx = {.unit = "src/main.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    size_t boxes = 0;
    for(const char *at = block; (at = strstr(at, "┌─ ")) != NULL; at += 3)
        boxes++;
    EXPECT_EQ(10u, boxes);

    /* Nothing was dropped: the fourteenth is still there, in one line. */
    EXPECT_NOT_NULL(strstr(block, "cascade 14"));
    EXPECT_NOT_NULL(strstr(block, "molto_no_such_source_zzz.c:14:1: error: cascade 14"));

    free(block);
}

/* A project that asked for -fdiagnostics-color=always gets its escapes back
   out here: they were chosen to stand out against a plain stream, and they
   fight a frame that colours itself. */
MOLTEST(escapes_the_compiler_was_told_to_emit_are_taken_back_out) {
    const char *text = "/tmp/molto_no_such_source_zzz.c:1:1: error: "
                       "\033[01;31mvery\033[0m bad\n";
    const diagnostic_context ctx = {.unit = "src/main.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NULL(strchr(block, '\033'));
    EXPECT_NOT_NULL(strstr(block, "very bad"));

    free(block);
}

MOLTEST(colour_is_the_callers_to_ask_for) {
    const char *text = "/tmp/molto_no_such_source_zzz.c:1:1: error: broken\n";
    const diagnostic_context ctx = {.unit = "src/main.c"};

    char *plain = render(text, &ctx, false);
    ASSERT_NOT_NULL(plain);
    EXPECT_NULL(strchr(plain, '\033'));
    free(plain);

    char *painted = render(text, &ctx, true);
    ASSERT_NOT_NULL(painted);
    EXPECT_NOT_NULL(strchr(painted, '\033'));
    free(painted);
}

/* Most units compile quietly, and a frame around nothing is not a report. */
MOLTEST(a_unit_with_nothing_to_say_is_drawn_as_nothing) {
    const diagnostic_context ctx = {.unit = "src/main.c"};
    EXPECT_NULL(render("", &ctx, false));
    EXPECT_NULL(render("1 warning generated.\n", &ctx, false));
    EXPECT_NULL(render("src/main.c: In function ‘f’:\n", &ctx, false));

    diagnostic_list list;
    diagnostic_list_init(&list);
    EXPECT_NULL(diagnostic_view_render(&list, &ctx, false));
    EXPECT_NULL(diagnostic_view_render(NULL, &ctx, false));
    diagnostic_list_free(&list);
}

/* A link is not a compile, and a block that said "Failed to compile" over a
   linker's words would be naming the wrong step. */
MOLTEST(a_link_is_reported_as_a_link) {
    diagnostic_list found;
    diagnostic_list_init(&found);
    ASSERT_TRUE(diagnostic_parse_link("/usr/bin/ld: /tmp/x.o: in function `main':\n"
                                      "main.c:(.text+0x9): undefined reference to `db_open'\n"
                                      "collect2: error: ld returned 1 exit status\n",
                                      &found));

    const diagnostic_context ctx = {
        .unit = "build/debug/demo", .action = diagnostic_view_linking, .compiler = "gcc 12.3.0"};
    char *block = diagnostic_view_render(&found, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "✗ Failed to link `build/debug/demo`"));
    EXPECT_NOT_NULL(strstr(block, "error: linking failed"));
    EXPECT_NULL(strstr(block, "Failed to compile"));
    EXPECT_NULL(strstr(block, "compilation failed"));
    /* Nowhere to point at, so the linker's own words are quoted instead. */
    EXPECT_NOT_NULL(strstr(block, "│ main.c:(.text+0x9): undefined reference to `db_open'"));
    EXPECT_NULL(strstr(block, "┌─"));
    EXPECT_NOT_NULL(strstr(block, "= compiler: gcc 12.3.0"));

    free(block);
    diagnostic_list_free(&found);
}

/* A compiler that knows what should have been written there says so under the
   caret, as bare text at the column it belongs to. Nothing on that line says
   it is a suggestion, and a reader who has not seen one before cannot tell
   `%zu` from noise. */
MOLTEST(a_fix_it_is_stated_as_a_suggestion) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "    printf(\"Vector {%d}\\n\", vec.size);\n"));
    char text[1024];
    snprintf(text, sizeof text,
             "%s:1:22: warning: format specifies type 'int' but the argument has type "
             "'size_t' [-Wformat]\n"
             "    1 |     printf(\"Vector {%%d}\\n\", vec.size);\n"
             "      |                     ~~      ^~~~~~~~\n"
             "      |                     %%zu\n",
             path);

    const diagnostic_context ctx = {.unit = "src/main.c", .compiler = "clang 22.1.8"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "= help: try `%zu`\n"));
    /* And it is no longer printed as the fragment the compiler drew. */
    EXPECT_NULL(strstr(block, "|                     %zu"));
    /* The suggestion belongs to the finding, so it comes before the footer. */
    EXPECT_TRUE(strstr(block, "= help:") < strstr(block, "= compiler:"));

    free(block);
    (void)remove(path);
}

/* gcc draws the same thing in the same place, gutter and all. */
MOLTEST(a_fix_it_is_read_out_of_gccs_gutter_too) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "    strcpy(dst, src);\n"));
    char text[1024];
    snprintf(text, sizeof text,
             "%s:1:5: warning: ‘strcpy’ writes past the end of ‘dst’ [-Wstringop-overflow=]\n"
             "    1 |     strcpy(dst, src);\n"
             "      |     ^~~~~~\n"
             "      |     strncpy\n",
             path);

    const diagnostic_context ctx = {.unit = "src/copy.c", .compiler = "gcc 12.3.0"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "= help: try `strncpy`\n"));

    free(block);
    (void)remove(path);
}

/* Two edits to one line arrive on one line, each under the column it applies
   to. They are two suggestions and they are said as two, because "( )" with
   eight spaces in it is not something anyone can type. */
MOLTEST(several_edits_on_one_line_are_said_one_by_one) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "    if (a = b) {\n"));
    char text[1024];
    snprintf(text, sizeof text,
             "%s:1:11: warning: using the result of an assignment as a condition "
             "[-Wparentheses]\n"
             "    1 |     if (a = b) {\n"
             "      |         ~~^~~\n"
             "      |         (     )\n",
             path);

    const diagnostic_context ctx = {.unit = "src/main.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "= help: try `(`\n"));
    EXPECT_NOT_NULL(strstr(block, "= help: try `)`\n"));

    free(block);
    (void)remove(path);
}

/* Only the caret above says that a line is a suggestion. Text that follows
   anything else is prose, and prose read out as an edit would be an invention
   the compiler never made. */
MOLTEST(an_indented_line_that_follows_no_caret_is_not_a_suggestion) {
    const char *text = "/tmp/molto_no_such_source_zzz.c:1:1: error: broken\n"
                       "    while compiling the template instantiated here\n";
    const diagnostic_context ctx = {.unit = "src/main.c"};
    char *block = render(text, &ctx, false);
    ASSERT_NOT_NULL(block);

    EXPECT_NULL(strstr(block, "= help:"));
    EXPECT_NOT_NULL(strstr(block, "while compiling the template instantiated here"));

    free(block);
}

/* The two compilers count the column they report differently, and a caret
   placed under the wrong model lands where the compiler was not pointing.
   Same source, same character, two numbers — and one caret column. */
MOLTEST(a_caret_lands_in_the_same_place_whichever_model_counted_it) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "\t\treturn nope;\n"));

    char from_gcc[512];
    snprintf(from_gcc, sizeof from_gcc, "%s:1:24: error: ‘nope’ undeclared\n", path);
    char from_clang[512];
    snprintf(from_clang, sizeof from_clang, "%s:1:10: error: undeclared identifier\n", path);

    const diagnostic_context ctx = {.unit = "main.c"};

    char *painted_by_gcc = render_counted(from_gcc, &ctx, diagnostic_columns_display, false);
    char *painted_by_clang = render_counted(from_clang, &ctx, diagnostic_columns_byte, false);
    ASSERT_NOT_NULL(painted_by_gcc);
    ASSERT_NOT_NULL(painted_by_clang);

    /* Two tabs are sixteen columns and "return " is seven more, so both carets
       sit at column 23 — under the `n` of `nope`. */
    EXPECT_NOT_NULL(strstr(painted_by_gcc, "   │                        ^ "));
    EXPECT_NOT_NULL(strstr(painted_by_clang, "   │                        ^ "));
    /* And the excerpt shows the tabs as the terminal will. */
    EXPECT_NOT_NULL(strstr(painted_by_gcc, " 1 │                 return nope;\n"));

    free(painted_by_gcc);
    free(painted_by_clang);
    (void)remove(path);
}

/* One list, two tools, two ways of counting the same character. `molto lint`
   runs a compiler pass and a clang-tidy pass over every file, and clang-tidy
   counts bytes whichever compiler the project builds with — so the model
   belongs to the diagnostic and not to the report that holds it. */
MOLTEST(one_report_can_hold_two_tools_that_counted_columns_differently) {
    char path[64];
    ASSERT_TRUE(write_source(path, sizeof path, "\treturn nope;\n"));

    char text[1024];
    snprintf(text, sizeof text,
             "%s:1:16: error: ‘nope’ undeclared\n"
             "%s:1:9: error: use of undeclared identifier [clang-diagnostic-error]\n",
             path, path);

    diagnostic_list list;
    diagnostic_list_init(&list);
    ASSERT_TRUE(diagnostic_parse(text, &list));
    ASSERT_EQ(2, diagnostic_list_count(&list));
    list.items[0].columns = diagnostic_columns_display;
    list.items[1].columns = diagnostic_columns_byte;

    const diagnostic_context ctx = {.unit = "src/main.c", .action = diagnostic_view_checking};
    char *block = diagnostic_view_render(&list, &ctx, false);
    ASSERT_NOT_NULL(block);

    /* A tab is eight columns and "return " is seven more, so both carets sit
       under the `n` of `nope`, at column 16. */
    char caret[64];
    snprintf(caret, sizeof caret, "   │%*s^ ", 16, "");
    const char *first = strstr(block, caret);
    ASSERT_NOT_NULL(first);
    EXPECT_NOT_NULL(strstr(first + 1, caret));

    free(block);
    diagnostic_list_free(&list);
    (void)remove(path);
}

/* --- one block per file --- */

/* The chain of includes that reached a broken header is written before the
   diagnostic it explains, on lines carrying no file of their own. It belongs
   to the file below it: attached to the one above, it would name the wrong
   source and never be drawn, since a chain is only shown when a frame opens. */
MOLTEST(an_include_chain_opens_the_block_of_the_file_it_leads_to) {
    const char *text = "src/main.c:4:9: warning: unused variable 'x' [-Wunused-variable]\n"
                       "In file included from src/deep/uses.c:1:\n"
                       "include/broken.h:2:11: error: expected ';' at end of declaration\n";
    const diagnostic_context ctx = {.action = diagnostic_view_checking};
    char *block = render_by_file(text, &ctx);
    ASSERT_NOT_NULL(block);

    /* Two files, two blocks, and neither of them nameless. */
    EXPECT_NOT_NULL(strstr(block, "Findings in `src/main.c`"));
    EXPECT_NOT_NULL(strstr(block, "Errors in `include/broken.h`"));
    EXPECT_NULL(strstr(block, "in ``"));

    /* And the chain survives, under the frame it explains. */
    const char *chain = strstr(block, "included from src/deep/uses.c:1");
    ASSERT_NOT_NULL(chain);
    EXPECT_TRUE(chain > strstr(block, "Errors in `include/broken.h`"));

    free(block);
}

/* The same, when there is no block above for it to be swallowed by: a run
   that opens without a file is named by the first finding that has one. */
MOLTEST(a_run_that_opens_without_a_file_is_named_by_what_it_introduces) {
    const char *text = "In file included from src/deep/uses.c:1:\n"
                       "include/broken.h:2:11: error: expected ';' at end of declaration\n";
    const diagnostic_context ctx = {.action = diagnostic_view_checking};
    char *block = render_by_file(text, &ctx);
    ASSERT_NOT_NULL(block);

    EXPECT_NOT_NULL(strstr(block, "Errors in `include/broken.h`"));
    /* Neither nameless nor named after nothing: `this unit` is what the view
       falls back to when the caller supplied no name and the findings carry
       none either. */
    EXPECT_NULL(strstr(block, "in ``"));
    EXPECT_NULL(strstr(block, "this unit"));
    EXPECT_NOT_NULL(strstr(block, "included from src/deep/uses.c:1"));

    free(block);
}

/* One blank line between blocks, and none before the first: the separator
   belongs between two drawings and nowhere else. */
MOLTEST(blocks_are_separated_from_each_other_and_not_from_the_top) {
    const char *text = "src/a.c:1:1: warning: first\n"
                       "src/b.c:1:1: warning: second\n";
    const diagnostic_context ctx = {.action = diagnostic_view_checking};
    char *block = render_by_file(text, &ctx);
    ASSERT_NOT_NULL(block);

    EXPECT_TRUE(block[0] != '\n');
    EXPECT_NOT_NULL(strstr(block, "Findings in `src/a.c`"));
    EXPECT_NOT_NULL(strstr(block, "Findings in `src/b.c`"));
    EXPECT_NULL(strstr(block, "\n\n\n"));

    free(block);
}

/* Findings about one file that arrive in two runs — the compiler pass, then
   the linter pass — are two blocks about that file rather than one, because
   what came between them was about another file. Each still names itself. */
MOLTEST(a_file_named_again_after_another_opens_a_second_block) {
    const char *text = "src/a.c:1:1: warning: from the compiler\n"
                       "src/b.c:1:1: warning: from the compiler\n"
                       "src/a.c:1:1: warning: from the linter [bugprone-branch-clone]\n";
    const diagnostic_context ctx = {.action = diagnostic_view_checking};
    char *block = render_by_file(text, &ctx);
    ASSERT_NOT_NULL(block);

    const char *first = strstr(block, "Findings in `src/a.c`");
    ASSERT_NOT_NULL(first);
    EXPECT_NOT_NULL(strstr(first + 1, "Findings in `src/a.c`"));
    EXPECT_NOT_NULL(strstr(block, "Findings in `src/b.c`"));

    free(block);
}

/* A suggestion is written under the caret it belongs to, and a tool's aside
   about the file it just processed comes after the findings in it. Both carry
   no file of their own, and reading every such entry as a preamble would hand
   the tail of one block to the next one. */
MOLTEST(a_suggestion_stays_with_the_finding_it_follows) {
    const char *text = "include/broken.h:2:11: error: expected ';' at end of declaration\n"
                       "    2 |         int x = 1\n"
                       "      |                  ^\n"
                       "      |                  ;\n"
                       "Error while processing src/deep/uses.c.\n"
                       "src/main.c:4:9: warning: unused variable 'x' [-Wunused-variable]\n";
    const diagnostic_context ctx = {.action = diagnostic_view_checking};
    char *block = render_by_file(text, &ctx);
    ASSERT_NOT_NULL(block);

    const char *opens_the_next_block = strstr(block, "Findings in `src/main.c`");
    ASSERT_NOT_NULL(opens_the_next_block);

    const char *help = strstr(block, "= help: try `;`");
    ASSERT_NOT_NULL(help);
    EXPECT_TRUE(help < opens_the_next_block);

    const char *aside = strstr(block, "Error while processing src/deep/uses.c.");
    ASSERT_NOT_NULL(aside);
    EXPECT_TRUE(aside < opens_the_next_block);

    free(block);
}
