#include <moltest.h>

#include <molto/build/diff.h>

#include <stdio.h>
#include <string.h>

/* Render a diff into a buffer, so what it wrote can be asserted on. */
static bool render(const char *original, const char *formatted, const char *path,
                   char *out, size_t out_size, bool *changed) {
    FILE *scratch = tmpfile();
    if (scratch == NULL)
        return false;
    bool ok = diff_unified(original, formatted, path, DIFF_CONTEXT_LINES,
                           scratch, changed);
    rewind(scratch);
    size_t read = fread(out, 1, out_size - 1, scratch);
    out[read] = '\0';
    fclose(scratch);
    return ok;
}

MOLTEST(diff_writes_nothing_for_identical_text) {
    char out[1024] = "x";
    bool changed = true;
    ASSERT_TRUE(render("a\nb\n", "a\nb\n", "src/a.c", out, sizeof out, &changed));

    EXPECT_STREQ("", out);
    /* `--check` reports on exactly this. */
    EXPECT_FALSE(changed);
}

MOLTEST(diff_labels_the_hunk_with_the_path) {
    char out[1024] = "";
    bool changed = false;
    ASSERT_TRUE(render("a\n", "b\n", "src/net.c", out, sizeof out, &changed));

    EXPECT_TRUE(changed);
    EXPECT_NOT_NULL(strstr(out, "--- a/src/net.c"));
    EXPECT_NOT_NULL(strstr(out, "+++ b/src/net.c"));
    EXPECT_NOT_NULL(strstr(out, "@@"));
}

MOLTEST(diff_reports_a_changed_line_as_a_removal_and_an_addition) {
    char out[1024] = "";
    ASSERT_TRUE(render("one\ntwo\nthree\n", "one\nTWO\nthree\n",
                       "a.c", out, sizeof out, NULL));

    EXPECT_NOT_NULL(strstr(out, "-two"));
    EXPECT_NOT_NULL(strstr(out, "+TWO"));
    /* The lines around it are context, not changes. */
    EXPECT_NOT_NULL(strstr(out, " one"));
    EXPECT_NOT_NULL(strstr(out, " three"));
}

MOLTEST(diff_reports_an_added_line) {
    char out[1024] = "";
    ASSERT_TRUE(render("a\nc\n", "a\nb\nc\n", "a.c", out, sizeof out, NULL));

    EXPECT_NOT_NULL(strstr(out, "+b"));
    EXPECT_NULL(strstr(out, "-a"));
    EXPECT_NULL(strstr(out, "-c"));
}

MOLTEST(diff_reports_a_removed_line) {
    char out[1024] = "";
    ASSERT_TRUE(render("a\nb\nc\n", "a\nc\n", "a.c", out, sizeof out, NULL));

    EXPECT_NOT_NULL(strstr(out, "-b"));
    EXPECT_NULL(strstr(out, "+b"));
}

MOLTEST(diff_numbers_the_hunk_from_where_the_change_is) {
    char out[2048] = "";
    /* Ten unchanged lines, then a change: the hunk must start at the context
       before the change, not at the top of the file. */
    ASSERT_TRUE(render("1\n2\n3\n4\n5\n6\n7\n8\n9\nold\n",
                       "1\n2\n3\n4\n5\n6\n7\n8\n9\nnew\n",
                       "a.c", out, sizeof out, NULL));

    EXPECT_NOT_NULL(strstr(out, "@@ -7,4 +7,4 @@"));
    EXPECT_NULL(strstr(out, " 1\n"));
}

MOLTEST(diff_keeps_two_distant_changes_in_separate_hunks) {
    char out[4096] = "";
    ASSERT_TRUE(render("A\n1\n2\n3\n4\n5\n6\n7\n8\n9\nB\n",
                       "a\n1\n2\n3\n4\n5\n6\n7\n8\n9\nb\n",
                       "a.c", out, sizeof out, NULL));

    /* Far apart: repeating the untouched middle in one hunk would be noise. */
    int hunks = 0;
    for (const char *p = out; (p = strstr(p, "@@ -")) != NULL; p += 4)
        hunks++;
    EXPECT_EQ(2, hunks);
}

MOLTEST(diff_joins_two_nearby_changes_into_one_hunk) {
    char out[2048] = "";
    ASSERT_TRUE(render("A\n1\n2\nB\n", "a\n1\n2\nb\n", "a.c", out, sizeof out, NULL));

    int hunks = 0;
    for (const char *p = out; (p = strstr(p, "@@ -")) != NULL; p += 4)
        hunks++;
    EXPECT_EQ(1, hunks);
}

MOLTEST(diff_handles_a_file_without_a_trailing_newline) {
    char out[1024] = "";
    bool changed = false;
    /* "a\n" is one line, not two: a trailing newline terminates a line rather
       than starting an empty one. */
    ASSERT_TRUE(render("a\nb", "a\nB", "a.c", out, sizeof out, &changed));

    EXPECT_TRUE(changed);
    EXPECT_NOT_NULL(strstr(out, "-b"));
    EXPECT_NOT_NULL(strstr(out, "+B"));
}

MOLTEST(diff_handles_an_empty_side) {
    char out[1024] = "";
    bool changed = false;
    ASSERT_TRUE(render("", "a\nb\n", "a.c", out, sizeof out, &changed));
    EXPECT_TRUE(changed);
    EXPECT_NOT_NULL(strstr(out, "+a"));
    EXPECT_NOT_NULL(strstr(out, "+b"));

    out[0] = '\0';
    ASSERT_TRUE(render("a\nb\n", "", "a.c", out, sizeof out, &changed));
    EXPECT_NOT_NULL(strstr(out, "-a"));
    EXPECT_NOT_NULL(strstr(out, "-b"));
}

MOLTEST(diff_reports_the_reindentation_a_formatter_produces) {
    char out[2048] = "";
    /* The shape of what clang-format actually does to a file. */
    ASSERT_TRUE(render("int main(void) {\n  int x;\n  return 0;\n}\n",
                       "int main(void) {\n    int x;\n    return 0;\n}\n",
                       "src/main.c", out, sizeof out, NULL));

    EXPECT_NOT_NULL(strstr(out, "-  int x;"));
    EXPECT_NOT_NULL(strstr(out, "+    int x;"));
    EXPECT_NOT_NULL(strstr(out, " int main(void) {"));
}
