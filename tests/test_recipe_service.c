#include <moltest.h>

#include <molto/services/recipe_service.h>
#include <molto/util/json.h>
#include <molto/util/toml.h>

#include <stdio.h>
#include <string.h>

/* The real sqlite recipe, in both encodings, so what is asserted is what the
   ecosystem actually publishes rather than a shape invented for the test. */

static const char *const SQLITE_TOML =
    "schema = 1\n"
    "form = \"source\"\n"
    "kind = \"package\"\n"
    "name = \"sqlite\"\n"
    "version = \"3.53.4\"\n"
    "target = \"any\"\n"
    "\n"
    "[artifacts]\n"
    "type = \"source\"\n"
    "sources = [\"sqlite3.c\"]\n"
    "include = [\".\"]\n"
    "link = [\"m\", \"dl\", \"pthread\"]\n"
    "defines = [\"SQLITE_THREADSAFE=1\", \"SQLITE_ENABLE_FTS5\", \"SQLITE_ENABLE_RTREE\"]\n";

static const char *const SQLITE_JSON =
    "{\"schema\":1,\"form\":\"source\",\"kind\":\"package\",\"name\":\"sqlite\","
    "\"version\":\"3.53.4\",\"target\":\"any\","
    "\"artifacts\":{\"type\":\"source\",\"sources\":[\"sqlite3.c\"],\"include\":[\".\"],"
    "\"link\":[\"m\",\"dl\",\"pthread\"],"
    "\"defines\":[\"SQLITE_THREADSAFE=1\",\"SQLITE_ENABLE_FTS5\",\"SQLITE_ENABLE_RTREE\"]}}";

/* Runs `check` against the same recipe read from TOML and from JSON. */
static void for_both_encodings(void (*check)(doc_view)) {
    char err[256] = "";
    toml_document *as_toml = toml_parse(SQLITE_TOML, err, sizeof err);
    ASSERT_NOT_NULL(as_toml);
    json_document *as_json = json_parse(SQLITE_JSON);
    ASSERT_NOT_NULL(as_json);

    check(doc_from_toml(as_toml));
    check(doc_from_json(json_root(as_json)));

    toml_free(as_toml);
    json_free(as_json);
}

static void check_coordinate(doc_view doc) {
    recipe_coordinate coordinate;
    char err[256] = "";
    ASSERT_TRUE(recipe_read_coordinate(doc, &coordinate, err, sizeof err));

    EXPECT_EQ(1L, coordinate.schema);
    EXPECT_EQ(recipe_form_source, coordinate.form);
    EXPECT_STREQ("package", coordinate.kind);
    EXPECT_STREQ("sqlite", coordinate.name);
    EXPECT_STREQ("3.53.4", coordinate.version);
    EXPECT_STREQ("any", coordinate.target);
}

MOLTEST(recipe_reads_its_coordinate_from_either_encoding) { for_both_encodings(check_coordinate); }

static void check_artifacts(doc_view doc) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(recipe_read_artifacts(doc, &artifacts, err, sizeof err));

    EXPECT_EQ(recipe_artifact_source, artifacts.type);
    ASSERT_EQ(1u, artifacts.source_count);
    EXPECT_STREQ("sqlite3.c", artifacts.sources[0]);

    ASSERT_EQ(3u, artifacts.link_count);
    EXPECT_STREQ("m", artifacts.link[0]);
    EXPECT_STREQ("pthread", artifacts.link[2]);

    /* They land in the manifest's own option type, so the existing
       compile_flags_push_options consumes them unchanged. */
    ASSERT_EQ(1u, artifacts.options.include_count);
    EXPECT_STREQ(".", artifacts.options.include[0]);
    ASSERT_EQ(3u, artifacts.options.define_count);
    EXPECT_STREQ("SQLITE_THREADSAFE=1", artifacts.options.defines[0]);
    EXPECT_EQ(0u, artifacts.options.flag_count);
}

MOLTEST(recipe_reads_the_artifacts_table_from_either_encoding) {
    for_both_encodings(check_artifacts);
}

/* Reads a recipe out of TOML text, for the cases that only need one encoding. */
static bool read_coordinate_of(const char *text, recipe_coordinate *out, char *err,
                               size_t err_size) {
    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    if (doc == NULL) {
        snprintf(err, err_size, "%s", parse_err);
        return false;
    }
    const bool ok = recipe_read_coordinate(doc_from_toml(doc), out, err, err_size);
    toml_free(doc);
    return ok;
}

static bool read_artifacts_of(const char *text, recipe_artifacts *out, char *err,
                              size_t err_size) {
    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    if (doc == NULL) {
        snprintf(err, err_size, "%s", parse_err);
        return false;
    }
    const bool ok = recipe_read_artifacts(doc_from_toml(doc), out, err, err_size);
    toml_free(doc);
    return ok;
}

#define MINIMUM                                                                                    \
    "kind = \"package\"\nname = \"x\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"

MOLTEST(recipe_assumes_what_a_recipe_without_the_new_keys_meant) {
    /* schema and form are both new, and the recipes published before they
       existed cannot be made to declare them. */
    recipe_coordinate coordinate;
    char err[256] = "";
    ASSERT_TRUE(read_coordinate_of(MINIMUM, &coordinate, err, sizeof err));

    EXPECT_EQ(1L, coordinate.schema);
    EXPECT_EQ(recipe_form_binary, coordinate.form);
}

MOLTEST(recipe_rejects_a_schema_it_cannot_read) {
    /* A later schema may give an existing key a new meaning, so reading it
       optimistically is reading it wrong. */
    recipe_coordinate coordinate;
    char err[256] = "";
    EXPECT_FALSE(read_coordinate_of("schema = 2\n" MINIMUM, &coordinate, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "upgrade molto"));
}

MOLTEST(recipe_rejects_an_unknown_form) {
    recipe_coordinate coordinate;
    char err[256] = "";
    EXPECT_FALSE(read_coordinate_of("form = \"recipe\"\n" MINIMUM, &coordinate, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "unknown recipe form"));
}

MOLTEST(recipe_reports_a_coordinate_that_is_incomplete) {
    recipe_coordinate coordinate;
    char err[256] = "";
    EXPECT_FALSE(read_coordinate_of("kind = \"package\"\nname = \"x\"\n", &coordinate, err,
                                    sizeof err));
    EXPECT_NOT_NULL(strstr(err, "version"));
}

MOLTEST(recipe_defaults_the_artifact_type_to_static) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of("[artifacts]\ninclude = [\"include\"]\n", &artifacts, err,
                                  sizeof err));
    EXPECT_EQ(recipe_artifact_static, artifacts.type);
}

MOLTEST(recipe_rejects_an_artifact_type_nothing_can_build) {
    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of("[artifacts]\ntype = \"header_only\"\n", &artifacts, err,
                                   sizeof err));
    EXPECT_NOT_NULL(strstr(err, "source, static or shared"));
}

MOLTEST(recipe_without_an_artifacts_table_is_not_an_error) {
    /* A binary recipe describes itself with [package] instead. */
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of(MINIMUM, &artifacts, err, sizeof err));
    EXPECT_EQ(0u, artifacts.source_count);
    EXPECT_EQ(recipe_artifact_static, artifacts.type);
}

MOLTEST(recipe_refuses_a_sources_list_that_is_not_a_list) {
    /* Silently reading nothing here compiles nothing, and the link failure
       that follows names none of it. */
    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of("[artifacts]\nsources = \"sqlite3.c\"\n", &artifacts, err,
                                   sizeof err));
    EXPECT_NOT_NULL(strstr(err, "list of strings"));
}

MOLTEST(recipe_refuses_more_sources_than_it_can_hold) {
    char text[8192] = "[artifacts]\nsources = [";
    for (int i = 0; i < RECIPE_MAX_SOURCES + 1; i++) {
        char entry[32];
        snprintf(entry, sizeof entry, "%s\"f%d.c\"", i > 0 ? ", " : "", i);
        strcat(text, entry);
    }
    strcat(text, "]\n");

    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of(text, &artifacts, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "more than"));
}

MOLTEST(recipe_compiles_only_the_sources_it_names) {
    /* The reason `sources` exists: the amalgamation ships shell.c, which has
       its own main(), and compiling the whole drop links two of them. */
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of("[artifacts]\nsources = [\"sqlite3.c\"]\n", &artifacts, err,
                                  sizeof err));

    EXPECT_TRUE(recipe_artifacts_wants(&artifacts, "sqlite3.c"));
    EXPECT_FALSE(recipe_artifacts_wants(&artifacts, "shell.c"));
}

MOLTEST(recipe_takes_everything_when_it_names_no_sources) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of("[artifacts]\ninclude = [\".\"]\n", &artifacts, err, sizeof err));

    EXPECT_TRUE(recipe_artifacts_wants(&artifacts, "anything.c"));
}

MOLTEST(recipe_applies_exclude_after_sources) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of("[artifacts]\nexclude = [\"shell.c\"]\n", &artifacts, err,
                                  sizeof err));

    EXPECT_TRUE(recipe_artifacts_wants(&artifacts, "sqlite3.c"));
    EXPECT_FALSE(recipe_artifacts_wants(&artifacts, "shell.c"));

    /* Named in both: exclude wins, so a recipe can narrow a list it inherited
       rather than restate it. */
    ASSERT_TRUE(read_artifacts_of("[artifacts]\nsources = [\"a.c\", \"b.c\"]\nexclude = "
                                  "[\"b.c\"]\n",
                                  &artifacts, err, sizeof err));
    EXPECT_TRUE(recipe_artifacts_wants(&artifacts, "a.c"));
    EXPECT_FALSE(recipe_artifacts_wants(&artifacts, "b.c"));
}
