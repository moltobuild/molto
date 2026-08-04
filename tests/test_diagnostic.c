#include <moltest.h>

#include <molto/build/diagnostic.h>

#include <stdio.h>
#include <string.h>

/* Real output, captured from the tools Molto runs. gcc, clang-tidy and
   clang-format --dry-run all print the same line, which is why one parser
   serves all three. */
static const char gcc_transcript[] =
    "lt.c: In function 'main':\n"
    "lt.c:1:20: warning: unused variable 'x' [-Wunused-variable]\n"
    "    1 | int main(void){int x;return 0;}\n"
    "      |                    ^\n";

static const char clang_tidy_transcript[] =
    "/tmp/c.c:1:11: warning: parameter name 'a' is too short [readability-identifier-length]\n"
    "/tmp/c.c:1:18: error: an assignment within an 'if' condition is bug-prone "
    "[bugprone-assignment-in-if-condition,-warnings-as-errors]\n"
    "/tmp/c.c:1:18: note: if it is meant to be an equality check, change '=' to '=='\n"
    "3 warnings generated.\n"
    "1 warning treated as error\n";

static void parse_one(const char *line, diagnostic *out) {
    diagnostic_parse_line(line, out);
}

MOLTEST(diagnostic_parses_a_gcc_warning) {
    diagnostic item;
    parse_one("lt.c:1:20: warning: unused variable 'x' [-Wunused-variable]", &item);

    EXPECT_STREQ("lt.c", item.file);
    EXPECT_EQ(1, (int)item.line);
    EXPECT_EQ(20, (int)item.column);
    EXPECT_EQ(diagnostic_severity_warning, item.severity);
    EXPECT_STREQ("unused variable 'x'", item.message);
    EXPECT_STREQ("unused_variable", item.rule);
    EXPECT_STREQ("-Wunused-variable", item.rule_native);
}

MOLTEST(diagnostic_parses_a_clang_format_violation) {
    diagnostic item;
    parse_one("a.c:1:4: error: code should be clang-formatted "
              "[-Wclang-format-violations]", &item);

    EXPECT_EQ(diagnostic_severity_error, item.severity);
    EXPECT_STREQ("clang_format_violations", item.rule);
}

MOLTEST(diagnostic_cuts_a_clang_tidy_rule_at_the_first_comma) {
    diagnostic item;
    parse_one("c.c:1:18: error: an assignment within an 'if' condition is bug-prone "
              "[bugprone-assignment-in-if-condition,-warnings-as-errors]", &item);

    /* The rule is what comes before the comma; the rest says why it was
       promoted, which is not part of its name. */
    EXPECT_STREQ("bugprone_assignment_in_if_condition", item.rule);
    EXPECT_STREQ("bugprone-assignment-in-if-condition,-warnings-as-errors",
                 item.rule_native);
    EXPECT_STREQ("an assignment within an 'if' condition is bug-prone", item.message);
}

MOLTEST(diagnostic_canonicalizes_a_werror_rule) {
    diagnostic item;
    parse_one("a.c:2:1: error: declaration shadows a variable [-Werror=shadow]", &item);
    EXPECT_STREQ("shadow", item.rule);
    EXPECT_STREQ("-Werror=shadow", item.rule_native);
}

MOLTEST(diagnostic_parses_a_fatal_error_as_an_error) {
    diagnostic item;
    /* Two words, which is why the severity is a table lookup and not a compare. */
    parse_one("a.c:1:10: fatal error: no_such.h: No such file or directory", &item);
    EXPECT_EQ(diagnostic_severity_error, item.severity);
    EXPECT_STREQ("no_such.h: No such file or directory", item.message);
}

MOLTEST(diagnostic_parses_a_note) {
    diagnostic item;
    parse_one("c.c:1:18: note: move it out of the 'if' condition", &item);
    EXPECT_EQ(diagnostic_severity_note, item.severity);
    EXPECT_STREQ("move it out of the 'if' condition", item.message);
}

MOLTEST(diagnostic_parses_a_line_without_a_column) {
    diagnostic item;
    /* gcc with -fno-diagnostics-show-column. */
    parse_one("a.c:7: warning: something happened", &item);

    EXPECT_STREQ("a.c", item.file);
    EXPECT_EQ(7, (int)item.line);
    EXPECT_EQ(0, (int)item.column);
    EXPECT_EQ(diagnostic_severity_warning, item.severity);
    EXPECT_STREQ("something happened", item.message);
}

MOLTEST(diagnostic_keeps_a_line_it_cannot_parse) {
    /* Everything a compiler interleaves around its diagnostics. None of it is
       discarded: it reaches the user in place, exactly as it was written. */
    const char *const opaque[] = {
        "lt.c: In function 'main':",
        "In file included from a.c:1:",
        "      |                    ^",
        "    1 | int main(void){int x;return 0;}",
        "ld: cannot find -lfoo",
    };
    for (size_t i = 0; i < sizeof opaque / sizeof opaque[0]; i++) {
        diagnostic item;
        parse_one(opaque[i], &item);
        EXPECT_EQ(diagnostic_severity_unknown, item.severity);
        EXPECT_STREQ("", item.file);
        EXPECT_STREQ(opaque[i], item.message);
    }
}

MOLTEST(diagnostic_tolerates_a_path_that_contains_a_colon) {
    diagnostic item;
    /* Scanning for "colon, digits, colon" rather than the first colon is what
       makes this work. */
    parse_one("/tmp/od:d/a.c:3:1: warning: hm", &item);
    EXPECT_STREQ("/tmp/od:d/a.c", item.file);
    EXPECT_EQ(3, (int)item.line);
}

MOLTEST(diagnostic_leaves_a_bracket_inside_a_message_alone) {
    diagnostic item;
    parse_one("a.c:1:1: warning: subscript [i] is out of range", &item);
    EXPECT_STREQ("subscript [i] is out of range", item.message);
    EXPECT_STREQ("", item.rule);
}

MOLTEST(diagnostic_parses_a_whole_transcript_in_order) {
    diagnostic_list list;
    diagnostic_list_init(&list);
    ASSERT_TRUE(diagnostic_parse(gcc_transcript, &list));

    ASSERT_EQ(4, (int)diagnostic_list_count(&list));
    EXPECT_EQ(diagnostic_severity_unknown, diagnostic_list_get(&list, 0)->severity);
    EXPECT_EQ(diagnostic_severity_warning, diagnostic_list_get(&list, 1)->severity);
    EXPECT_EQ(diagnostic_severity_unknown, diagnostic_list_get(&list, 2)->severity);
    EXPECT_EQ(diagnostic_severity_unknown, diagnostic_list_get(&list, 3)->severity);

    diagnostic_list_free(&list);
}

MOLTEST(diagnostic_drops_the_tally_a_linter_prints) {
    diagnostic_list list;
    diagnostic_list_init(&list);
    ASSERT_TRUE(diagnostic_parse(clang_tidy_transcript, &list));

    /* "3 warnings generated." is a count of the run, not something wrong with
       the code; keeping it would make the output change shape with the
       backend, which is what the normalization exists to prevent. */
    ASSERT_EQ(3, (int)diagnostic_list_count(&list));
    EXPECT_EQ(1, (int)diagnostic_count_severity(&list, diagnostic_severity_warning));
    EXPECT_EQ(1, (int)diagnostic_count_severity(&list, diagnostic_severity_error));
    EXPECT_EQ(1, (int)diagnostic_count_severity(&list, diagnostic_severity_note));

    diagnostic_list_free(&list);
}

MOLTEST(diagnostic_formats_a_path_relative_to_the_root) {
    diagnostic item;
    parse_one("/home/u/app/src/net.c:42:9: error: bad [naming]", &item);

    char line[512];
    ASSERT_TRUE(diagnostic_format(&item, "/home/u/app", line, sizeof line));
    EXPECT_STREQ("src/net.c:42:9: error: bad [naming]", line);

    /* A path outside the root stays as it is rather than being mangled. */
    ASSERT_TRUE(diagnostic_format(&item, "/elsewhere", line, sizeof line));
    EXPECT_STREQ("/home/u/app/src/net.c:42:9: error: bad [naming]", line);
}

MOLTEST(diagnostic_formats_what_it_could_not_parse_unchanged) {
    diagnostic item;
    parse_one("      |          ^", &item);

    char line[512];
    ASSERT_TRUE(diagnostic_format(&item, "/root", line, sizeof line));
    EXPECT_STREQ("      |          ^", line);
}

MOLTEST(diagnostic_formats_without_a_column_or_a_rule) {
    diagnostic item;
    parse_one("a.c:7: warning: plain", &item);

    char line[512];
    ASSERT_TRUE(diagnostic_format(&item, "", line, sizeof line));
    EXPECT_STREQ("a.c:7: warning: plain", line);
}

MOLTEST(diagnostic_writes_json_that_escapes_what_would_break_it) {
    diagnostic item;
    parse_one("a.c:1:1: error: say \"hi\" \\ now", &item);

    diagnostic_list list;
    diagnostic_list_init(&list);
    ASSERT_TRUE(diagnostic_list_push(&list, &item));

    FILE *scratch = tmpfile();
    ASSERT_NOT_NULL(scratch);
    diagnostic_write_json(scratch, &list, "");
    rewind(scratch);

    char text[1024] = "";
    size_t read = fread(text, 1, sizeof text - 1, scratch);
    text[read] = '\0';
    fclose(scratch);

    EXPECT_NOT_NULL(strstr(text, "\\\"hi\\\""));
    EXPECT_NOT_NULL(strstr(text, "\\\\"));
    EXPECT_NOT_NULL(strstr(text, "\"severity\": \"error\""));
    EXPECT_NOT_NULL(strstr(text, "\"line\": 1"));

    diagnostic_list_free(&list);
}

MOLTEST(diagnostic_appends_one_list_onto_another_in_order) {
    diagnostic first;
    diagnostic second;
    parse_one("a.c:1:1: error: one", &first);
    parse_one("b.c:2:1: error: two", &second);

    diagnostic_list left;
    diagnostic_list right;
    diagnostic_list_init(&left);
    diagnostic_list_init(&right);
    ASSERT_TRUE(diagnostic_list_push(&left, &first));
    ASSERT_TRUE(diagnostic_list_push(&right, &second));
    ASSERT_TRUE(diagnostic_list_append(&left, &right));

    ASSERT_EQ(2, (int)diagnostic_list_count(&left));
    EXPECT_STREQ("one", diagnostic_list_get(&left, 0)->message);
    EXPECT_STREQ("two", diagnostic_list_get(&left, 1)->message);
    EXPECT_NULL(diagnostic_list_get(&left, 2));

    diagnostic_list_free(&left);
    diagnostic_list_free(&right);
}

MOLTEST(diagnostic_json_leaves_out_what_a_machine_cannot_act_on) {
    diagnostic_list list;
    diagnostic_list_init(&list);
    /* A real finding, and the caret line under it. */
    ASSERT_TRUE(diagnostic_parse("a.c:1:1: warning: real [x]\n      |  ^\n", &list));
    ASSERT_EQ(2, (int)diagnostic_list_count(&list));

    FILE *scratch = tmpfile();
    ASSERT_NOT_NULL(scratch);
    diagnostic_write_json(scratch, &list, "");
    rewind(scratch);

    char text[1024] = "";
    size_t read = fread(text, 1, sizeof text - 1, scratch);
    text[read] = '\0';
    fclose(scratch);

    /* The caret is context for a person reading the line above it, not
       something a CI job could act on; the text form still prints it. */
    EXPECT_NOT_NULL(strstr(text, "\"message\": \"real\""));
    EXPECT_NULL(strstr(text, "^"));
    /* And the one entry that is left must not carry a trailing comma. */
    EXPECT_NOT_NULL(strstr(text, "}\n]"));

    diagnostic_list_free(&list);
}

/* --- storage form (RFC-0006) --- */

MOLTEST(diagnostics_survive_a_round_trip_through_the_store) {
    diagnostic_list original;
    diagnostic_list_init(&original);
    /* A parsed warning with a rule, a note, and a line the parser could not
       read — the three shapes that reach the store. */
    ASSERT_TRUE(diagnostic_parse("src/a.c:12:5: warning: unused variable 'x' [bugprone-unused]\n"
                                 "src/a.c:12:5: note: declared here\n"
                                 "      | int    x = 1;\t/* a tab and  spaces */\n",
                                 &original));
    ASSERT_EQ(3, diagnostic_list_count(&original));

    str_list values;
    str_list_init(&values);
    ASSERT_TRUE(diagnostic_list_to_values(&original, &values));

    diagnostic_list replayed;
    diagnostic_list_init(&replayed);
    ASSERT_TRUE(diagnostic_list_from_values(&values, &replayed));

    /* RFC-0006: a cached run must be indistinguishable from an uncached one.
       Compare what the user would see, field by field. */
    ASSERT_EQ(diagnostic_list_count(&original), diagnostic_list_count(&replayed));
    for (size_t i = 0; i < diagnostic_list_count(&original); i++) {
        const diagnostic *was = diagnostic_list_get(&original, i);
        const diagnostic *is = diagnostic_list_get(&replayed, i);
        EXPECT_STREQ(was->file, is->file);
        EXPECT_EQ(was->line, is->line);
        EXPECT_EQ(was->column, is->column);
        EXPECT_EQ((int)was->severity, (int)is->severity);
        EXPECT_STREQ(was->message, is->message);
        EXPECT_STREQ(was->rule, is->rule);
        EXPECT_STREQ(was->rule_native, is->rule_native);
    }

    /* Severity decides the exit code, and `unknown` must not come back as the
       `note` its name would suggest. */
    EXPECT_EQ((int)diagnostic_severity_unknown,
              (int)diagnostic_list_get(&replayed, 2)->severity);

    str_list_free(&values);
    diagnostic_list_free(&replayed);
    diagnostic_list_free(&original);
}

MOLTEST(an_empty_list_round_trips_as_an_empty_list) {
    diagnostic_list original;
    diagnostic_list_init(&original);
    str_list values;
    str_list_init(&values);
    ASSERT_TRUE(diagnostic_list_to_values(&original, &values));
    EXPECT_EQ(0, str_list_count(&values));

    diagnostic_list replayed;
    diagnostic_list_init(&replayed);
    /* A clean file records nothing and replays nothing, and that is a success:
       returning false here would re-analyse every clean file forever. */
    EXPECT_TRUE(diagnostic_list_from_values(&values, &replayed));
    EXPECT_EQ(0, diagnostic_list_count(&replayed));

    str_list_free(&values);
    diagnostic_list_free(&replayed);
    diagnostic_list_free(&original);
}

MOLTEST(a_malformed_record_is_refused_rather_than_half_read) {
    diagnostic_list out;
    diagnostic_list_init(&out);

    /* Not a whole number of records. */
    str_list truncated;
    str_list_init(&truncated);
    ASSERT_TRUE(str_list_push(&truncated, "src/a.c"));
    ASSERT_TRUE(str_list_push(&truncated, "12"));
    EXPECT_FALSE(diagnostic_list_from_values(&truncated, &out));

    /* A severity outside the enum would replay as some other severity, and
       severity is what decides whether the command fails. */
    str_list bad_severity;
    str_list_init(&bad_severity);
    const char *fields[] = {"src/a.c", "12", "5", "99", "rule", "rule", "message"};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
        ASSERT_TRUE(str_list_push(&bad_severity, fields[i]));
    EXPECT_FALSE(diagnostic_list_from_values(&bad_severity, &out));

    /* A number that is not one. */
    str_list bad_line;
    str_list_init(&bad_line);
    const char *junk[] = {"src/a.c", "12x", "5", "1", "rule", "rule", "message"};
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++)
        ASSERT_TRUE(str_list_push(&bad_line, junk[i]));
    EXPECT_FALSE(diagnostic_list_from_values(&bad_line, &out));

    str_list_free(&truncated);
    str_list_free(&bad_severity);
    str_list_free(&bad_line);
    diagnostic_list_free(&out);
}
