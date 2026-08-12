#include <moltest.h>

#include <molto/util/doc.h>
#include <molto/util/json.h>
#include <molto/util/toml.h>

#include <string.h>

/* One recipe, two encodings, one set of assertions.
 *
 * The sqlite recipe below is written twice: as the TOML its author wrote, and
 * as the JSON the registry serves back inside an artifact's `metadata`. Both
 * go through `assert_recipe`, so a backend that starts reading the document
 * differently from the other fails here rather than in whichever half of the
 * ecosystem happened to exercise it. */

static const char *const RECIPE_TOML = "schema = 1\n"
                                       "form = \"source\"\n"
                                       "kind = \"package\"\n"
                                       "name = \"sqlite\"\n"
                                       "version = \"3.53.4\"\n"
                                       "target = \"any\"\n"
                                       "\n"
                                       "[source]\n"
                                       "archive = \"https://sqlite.org/a.zip\"\n"
                                       "sha256 = \"1e71ddf9\"\n"
                                       "strip_prefix = \"sqlite-amalgamation-3530400\"\n"
                                       "\n"
                                       "[build]\n"
                                       "system = \"none\"\n"
                                       "jobs = true\n"
                                       "\n"
                                       "[artifacts]\n"
                                       "type = \"source\"\n"
                                       "sources = [\"sqlite3.c\"]\n"
                                       "include = [\".\"]\n"
                                       "link = [\"m\", \"dl\", \"pthread\"]\n";

static const char *const RECIPE_JSON = "{"
                                       "\"schema\":1,"
                                       "\"form\":\"source\","
                                       "\"kind\":\"package\","
                                       "\"name\":\"sqlite\","
                                       "\"version\":\"3.53.4\","
                                       "\"target\":\"any\","
                                       "\"source\":{"
                                       "\"archive\":\"https://sqlite.org/a.zip\","
                                       "\"sha256\":\"1e71ddf9\","
                                       "\"strip_prefix\":\"sqlite-amalgamation-3530400\"},"
                                       "\"build\":{\"system\":\"none\",\"jobs\":true},"
                                       "\"artifacts\":{"
                                       "\"type\":\"source\","
                                       "\"sources\":[\"sqlite3.c\"],"
                                       "\"include\":[\".\"],"
                                       "\"link\":[\"m\",\"dl\",\"pthread\"]}"
                                       "}";

/* Everything a reader of this recipe would ask, asked once. */
static void assert_recipe(doc_view doc) {
    char text[128] = "";
    long number = 0;
    bool flag = false;

    /* The coordinate lives at the top level, which is "" on both sides. */
    EXPECT_TRUE(doc_get_int(doc, "", "schema", &number));
    EXPECT_EQ(1L, number);
    EXPECT_TRUE(doc_get_string(doc, "", "form", text, sizeof text));
    EXPECT_STREQ("source", text);
    EXPECT_TRUE(doc_get_string(doc, "", "name", text, sizeof text));
    EXPECT_STREQ("sqlite", text);

    EXPECT_TRUE(doc_has_table(doc, "source"));
    EXPECT_TRUE(doc_get_string(doc, "source", "archive", text, sizeof text));
    EXPECT_STREQ("https://sqlite.org/a.zip", text);
    EXPECT_TRUE(doc_get_string(doc, "source", "strip_prefix", text, sizeof text));
    EXPECT_STREQ("sqlite-amalgamation-3530400", text);

    EXPECT_TRUE(doc_get_bool(doc, "build", "jobs", &flag));
    EXPECT_TRUE(flag);

    str_list link;
    str_list_init(&link);
    EXPECT_TRUE(doc_get_array(doc, "artifacts", "link", &link));
    ASSERT_EQ(3u, link.count);
    EXPECT_STREQ("m", link.items[0]);
    EXPECT_STREQ("pthread", link.items[2]);
    str_list_free(&link);

    /* An absent key is absent on both sides, and says so the same way. */
    EXPECT_FALSE(doc_has_key(doc, "source", "git"));
    EXPECT_FALSE(doc_get_string(doc, "source", "git", text, sizeof text));
    EXPECT_FALSE(doc_has_table(doc, "toolchain"));
}

MOLTEST(doc_reads_the_same_recipe_from_toml_and_json) {
    char err[256] = "";
    toml_document *as_toml = toml_parse(RECIPE_TOML, err, sizeof err);
    ASSERT_NOT_NULL(as_toml);
    json_document *as_json = json_parse(RECIPE_JSON);
    ASSERT_NOT_NULL(as_json);

    assert_recipe(doc_from_toml(as_toml));
    assert_recipe(doc_from_json(json_root(as_json)));

    toml_free(as_toml);
    json_free(as_json);
}

MOLTEST(doc_lists_a_table_s_members_the_same_way_on_both_sides) {
    char err[256] = "";
    toml_document *as_toml = toml_parse(RECIPE_TOML, err, sizeof err);
    ASSERT_NOT_NULL(as_toml);
    json_document *as_json = json_parse(RECIPE_JSON);
    ASSERT_NOT_NULL(as_json);

    str_list from_toml;
    str_list from_json;
    str_list_init(&from_toml);
    str_list_init(&from_json);
    EXPECT_TRUE(doc_table_members(doc_from_toml(as_toml), "artifacts", &from_toml));
    EXPECT_TRUE(doc_table_members(doc_from_json(json_root(as_json)), "artifacts", &from_json));

    ASSERT_EQ(4u, from_toml.count);
    ASSERT_EQ(from_toml.count, from_json.count);
    for (size_t i = 0; i < from_toml.count; i++)
        EXPECT_STREQ(from_toml.items[i], from_json.items[i]);

    str_list_free(&from_toml);
    str_list_free(&from_json);
    toml_free(as_toml);
    json_free(as_json);
}

MOLTEST(doc_reads_a_nested_table) {
    char err[256] = "";
    toml_document *as_toml =
        toml_parse("[toolchain.c]\nstd = [\"c17\"]\n", err, sizeof err);
    ASSERT_NOT_NULL(as_toml);
    json_document *as_json = json_parse("{\"toolchain\":{\"c\":{\"std\":[\"c17\"]}}}");
    ASSERT_NOT_NULL(as_json);

    for (int i = 0; i < 2; i++) {
        const doc_view doc =
            i == 0 ? doc_from_toml(as_toml) : doc_from_json(json_root(as_json));
        str_list std;
        str_list_init(&std);
        EXPECT_TRUE(doc_has_table(doc, "toolchain.c"));
        EXPECT_TRUE(doc_get_array(doc, "toolchain.c", "std", &std));
        ASSERT_EQ(1u, std.count);
        EXPECT_STREQ("c17", std.items[0]);
        str_list_free(&std);
    }

    toml_free(as_toml);
    json_free(as_json);
}

MOLTEST(doc_leaves_the_output_untouched_when_a_key_is_absent) {
    /* So a caller can seed a default and read over it. */
    json_document *as_json = json_parse("{\"source\":{}}");
    ASSERT_NOT_NULL(as_json);
    const doc_view doc = doc_from_json(json_root(as_json));

    char text[32] = "unchanged";
    EXPECT_FALSE(doc_get_string(doc, "source", "git", text, sizeof text));
    EXPECT_STREQ("unchanged", text);

    json_free(as_json);
}

MOLTEST(doc_refuses_a_value_of_the_wrong_type) {
    json_document *as_json = json_parse("{\"schema\":\"one\",\"name\":42}");
    ASSERT_NOT_NULL(as_json);
    const doc_view doc = doc_from_json(json_root(as_json));

    long number = 0;
    char text[32] = "";
    EXPECT_FALSE(doc_get_int(doc, "", "schema", &number));
    EXPECT_FALSE(doc_get_string(doc, "", "name", text, sizeof text));

    /* Declared, though — which is what tells a caller to complain rather than
       fall back to a default. */
    EXPECT_TRUE(doc_has_key(doc, "", "schema"));

    json_free(as_json);
}

MOLTEST(doc_refuses_a_number_that_does_not_fit) {
    json_document *as_json = json_parse("{\"schema\":99999999999999999999}");
    ASSERT_NOT_NULL(as_json);

    long number = 0;
    EXPECT_FALSE(doc_get_int(doc_from_json(json_root(as_json)), "", "schema", &number));

    json_free(as_json);
}

MOLTEST(doc_refuses_an_array_holding_something_that_is_not_a_string) {
    /* Half a list is worse than none, because half a list compiles. */
    json_document *as_json = json_parse("{\"artifacts\":{\"link\":[\"m\",7]}}");
    ASSERT_NOT_NULL(as_json);

    str_list link;
    str_list_init(&link);
    EXPECT_FALSE(doc_get_array(doc_from_json(json_root(as_json)), "artifacts", "link", &link));
    str_list_free(&link);

    json_free(as_json);
}

MOLTEST(doc_has_table_is_false_for_a_scalar_of_the_same_name) {
    json_document *as_json = json_parse("{\"source\":\"not a table\"}");
    ASSERT_NOT_NULL(as_json);

    EXPECT_FALSE(doc_has_table(doc_from_json(json_root(as_json)), "source"));

    json_free(as_json);
}

MOLTEST(doc_read_strings_fills_a_fixed_destination) {
    char err[256] = "";
    toml_document *as_toml = toml_parse(RECIPE_TOML, err, sizeof err);
    ASSERT_NOT_NULL(as_toml);

    char link[8][16];
    size_t count = 0;
    ASSERT_TRUE(doc_read_strings(doc_from_toml(as_toml), "artifacts", "link", link[0], 8,
                                 sizeof link[0], &count, err, sizeof err));
    ASSERT_EQ(3u, count);
    EXPECT_STREQ("m", link[0]);
    EXPECT_STREQ("dl", link[1]);
    EXPECT_STREQ("pthread", link[2]);

    toml_free(as_toml);
}

MOLTEST(doc_read_strings_is_silent_about_a_key_that_is_not_there) {
    char err[256] = "";
    toml_document *as_toml = toml_parse("[artifacts]\ntype = \"source\"\n", err, sizeof err);
    ASSERT_NOT_NULL(as_toml);

    char defines[4][16];
    size_t count = 0;
    EXPECT_TRUE(doc_read_strings(doc_from_toml(as_toml), "artifacts", "defines", defines[0], 4,
                                 sizeof defines[0], &count, err, sizeof err));
    EXPECT_EQ(0u, count);

    toml_free(as_toml);
}

MOLTEST(doc_read_strings_reports_a_key_that_is_there_but_is_not_a_list) {
    /* The case the probe exists for: without it this compiles nothing and says
       nothing. */
    char err[256] = "";
    toml_document *as_toml =
        toml_parse("[artifacts]\nsources = \"sqlite3.c\"\n", err, sizeof err);
    ASSERT_NOT_NULL(as_toml);

    char sources[4][16];
    size_t count = 0;
    err[0] = '\0';
    EXPECT_FALSE(doc_read_strings(doc_from_toml(as_toml), "artifacts", "sources", sources[0], 4,
                                  sizeof sources[0], &count, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "list of strings"));

    toml_free(as_toml);
}

MOLTEST(doc_read_strings_refuses_more_entries_than_fit) {
    char err[256] = "";
    toml_document *as_toml =
        toml_parse("[artifacts]\nlink = [\"a\", \"b\", \"c\"]\n", err, sizeof err);
    ASSERT_NOT_NULL(as_toml);

    char link[2][16];
    size_t count = 0;
    err[0] = '\0';
    EXPECT_FALSE(doc_read_strings(doc_from_toml(as_toml), "artifacts", "link", link[0], 2,
                                  sizeof link[0], &count, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "more than 2 entries"));

    toml_free(as_toml);
}

MOLTEST(doc_read_strings_refuses_an_entry_that_is_too_long) {
    char err[256] = "";
    toml_document *as_toml =
        toml_parse("[artifacts]\nlink = [\"pthread\"]\n", err, sizeof err);
    ASSERT_NOT_NULL(as_toml);

    char link[4][4]; /* "pthread" does not fit, and truncating it links nothing */
    size_t count = 0;
    err[0] = '\0';
    EXPECT_FALSE(doc_read_strings(doc_from_toml(as_toml), "artifacts", "link", link[0], 4,
                                  sizeof link[0], &count, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "longer than 3 characters"));

    toml_free(as_toml);
}
