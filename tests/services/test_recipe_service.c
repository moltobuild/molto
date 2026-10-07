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

/* Runs `check` against one recipe written in both encodings. */
static void for_both(const char *toml_text, const char *json_text, void (*check)(doc_view)) {
    char err[256] = "";
    toml_document *as_toml = toml_parse(toml_text, err, sizeof err);
    ASSERT_NOT_NULL(as_toml);
    json_document *as_json = json_parse(json_text);
    ASSERT_NOT_NULL(as_json);

    check(doc_from_toml(as_toml));
    check(doc_from_json(json_root(as_json)));

    toml_free(as_toml);
    json_free(as_json);
}

/* Runs `check` against the same recipe read from TOML and from JSON. */
static void for_both_encodings(void (*check)(doc_view)) {
    for_both(SQLITE_TOML, SQLITE_JSON, check);
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

DESCRIBE(recipe_reads_its_coordinate_from_either_encoding) { for_both_encodings(check_coordinate); }

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

DESCRIBE(recipe_reads_the_artifacts_table_from_either_encoding) {
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

static bool read_provide_of(const char *text, recipe_provide *out, char *err, size_t err_size) {
    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    if (doc == NULL) {
        snprintf(err, err_size, "%s", parse_err);
        return false;
    }
    const bool ok = recipe_read_provide(doc_from_toml(doc), out, err, err_size);
    toml_free(doc);
    return ok;
}

static bool read_build_of(const char *text, recipe_build *out, char *err, size_t err_size) {
    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    if (doc == NULL) {
        snprintf(err, err_size, "%s", parse_err);
        return false;
    }
    const bool ok = recipe_read_build(doc_from_toml(doc), out, err, err_size);
    toml_free(doc);
    return ok;
}

#define MINIMUM                                                                                    \
    "kind = \"package\"\nname = \"x\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"

DESCRIBE(recipe_assumes_what_a_recipe_without_the_new_keys_meant) {
    /* schema and form are both new, and the recipes published before they
       existed cannot be made to declare them. */
    recipe_coordinate coordinate;
    char err[256] = "";
    ASSERT_TRUE(read_coordinate_of(MINIMUM, &coordinate, err, sizeof err));

    EXPECT_EQ(1L, coordinate.schema);
    EXPECT_EQ(recipe_form_binary, coordinate.form);
}

DESCRIBE(recipe_rejects_a_schema_it_cannot_read) {
    /* A later schema may give an existing key a new meaning, so reading it
       optimistically is reading it wrong. Written against the ceiling rather
       than a number, so raising it does not quietly turn this into a test of
       nothing. */
    char text[256];
    snprintf(text, sizeof text, "schema = %d\n" MINIMUM, RECIPE_SCHEMA_MAX + 1);

    recipe_coordinate coordinate;
    char err[256] = "";
    EXPECT_FALSE(read_coordinate_of(text, &coordinate, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "upgrade molto"));
}

DESCRIBE(recipe_reads_the_schema_a_plugin_declares) {
    /* Schema 2 is `[plugin]` (RFC-0014), and a plugin recipe is required to
       declare it — so a reader that refused it could not read what the
       registry serves. */
    recipe_coordinate coordinate;
    char err[256] = "";
    EXPECT_TRUE(read_coordinate_of("schema = 2\n" MINIMUM, &coordinate, err, sizeof err));
    EXPECT_EQ(2, coordinate.schema);
}

DESCRIBE(recipe_rejects_an_unknown_form) {
    recipe_coordinate coordinate;
    char err[256] = "";
    EXPECT_FALSE(read_coordinate_of("form = \"recipe\"\n" MINIMUM, &coordinate, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "unknown recipe form"));
}

DESCRIBE(recipe_reports_a_coordinate_that_is_incomplete) {
    recipe_coordinate coordinate;
    char err[256] = "";
    EXPECT_FALSE(read_coordinate_of("kind = \"package\"\nname = \"x\"\n", &coordinate, err,
                                    sizeof err));
    EXPECT_NOT_NULL(strstr(err, "version"));
}

DESCRIBE(recipe_defaults_the_artifact_type_to_static) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of("[artifacts]\ninclude = [\"include\"]\n", &artifacts, err,
                                  sizeof err));
    EXPECT_EQ(recipe_artifact_static, artifacts.type);
}

DESCRIBE(recipe_rejects_an_artifact_type_nothing_can_build) {
    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of("[artifacts]\ntype = \"header_only\"\n", &artifacts, err,
                                   sizeof err));
    EXPECT_NOT_NULL(strstr(err, "source, static or shared"));
}

DESCRIBE(recipe_without_an_artifacts_table_is_not_an_error) {
    /* A binary recipe describes itself with [package] instead. */
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of(MINIMUM, &artifacts, err, sizeof err));
    EXPECT_EQ(0u, artifacts.source_count);
    EXPECT_EQ(recipe_artifact_static, artifacts.type);
}

DESCRIBE(recipe_refuses_a_sources_list_that_is_not_a_list) {
    /* Silently reading nothing here compiles nothing, and the link failure
       that follows names none of it. */
    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of("[artifacts]\nsources = \"sqlite3.c\"\n", &artifacts, err,
                                   sizeof err));
    EXPECT_NOT_NULL(strstr(err, "list of strings"));
}

DESCRIBE(recipe_refuses_more_sources_than_it_can_hold) {
    /* One per line, as a recipe that long is written: one physical line holds
       a few dozen at most. */
    char text[8192] = "[artifacts]\nsources = [\n";
    for (int i = 0; i < RECIPE_MAX_SOURCES + 1; i++) {
        char entry[32];
        snprintf(entry, sizeof entry, "  \"f%d.c\",\n", i);
        strcat(text, entry);
    }
    strcat(text, "]\n");

    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of(text, &artifacts, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "more than"));
}

DESCRIBE(recipe_compiles_only_the_sources_it_names) {
    /* The reason `sources` exists: the amalgamation ships shell.c, which has
       its own main(), and compiling the whole drop links two of them. */
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of("[artifacts]\nsources = [\"sqlite3.c\"]\n", &artifacts, err,
                                  sizeof err));

    EXPECT_TRUE(recipe_artifacts_wants(&artifacts, "sqlite3.c"));
    EXPECT_FALSE(recipe_artifacts_wants(&artifacts, "shell.c"));
}

DESCRIBE(recipe_takes_everything_when_it_names_no_sources) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of("[artifacts]\ninclude = [\".\"]\n", &artifacts, err, sizeof err));

    EXPECT_TRUE(recipe_artifacts_wants(&artifacts, "anything.c"));
}

DESCRIBE(recipe_applies_exclude_after_sources) {
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

/* --- what a package keeps to itself --- */

/* A library needs flags to build that its callers must not inherit: an
   internal include directory, a define naming its own config header, a warning
   it has decided to live with. The two tables are what tells them apart. */
static const char *const SCOPED_TOML = "schema = 1\n"
                                       "form = \"source\"\n"
                                       "kind = \"package\"\n"
                                       "name = \"zlib\"\n"
                                       "version = \"1.3.1\"\n"
                                       "target = \"any\"\n"
                                       "\n"
                                       "[artifacts]\n"
                                       "type = \"source\"\n"
                                       "include = [\".\"]\n"
                                       "defines = [\"ZLIB_CONST=1\"]\n"
                                       "\n"
                                       "[artifacts.private]\n"
                                       "include = [\"internal\"]\n"
                                       "defines = [\"HAVE_UNISTD_H\"]\n"
                                       "flags = [\"-fno-strict-aliasing\", \"-Wno-implicit-fallthrough\"]\n";

static const char *const SCOPED_JSON =
    "{\"schema\":1,\"form\":\"source\",\"kind\":\"package\",\"name\":\"zlib\","
    "\"version\":\"1.3.1\",\"target\":\"any\","
    "\"artifacts\":{\"type\":\"source\",\"include\":[\".\"],\"defines\":[\"ZLIB_CONST=1\"],"
    "\"private\":{\"include\":[\"internal\"],\"defines\":[\"HAVE_UNISTD_H\"],"
    "\"flags\":[\"-fno-strict-aliasing\",\"-Wno-implicit-fallthrough\"]}}}";

static void check_scoped(doc_view doc) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(recipe_read_artifacts(doc, &artifacts, err, sizeof err));

    /* The interface: what every consumer compiles with. */
    ASSERT_EQ(1u, artifacts.options.include_count);
    EXPECT_STREQ(".", artifacts.options.include[0]);
    ASSERT_EQ(1u, artifacts.options.define_count);
    EXPECT_STREQ("ZLIB_CONST=1", artifacts.options.defines[0]);
    EXPECT_EQ(0u, artifacts.options.flag_count);

    /* And what stays behind. The two never mix: a caller reading `options`
       cannot pick up a flag the package meant for itself. */
    ASSERT_EQ(1u, artifacts.private_options.include_count);
    EXPECT_STREQ("internal", artifacts.private_options.include[0]);
    ASSERT_EQ(1u, artifacts.private_options.define_count);
    EXPECT_STREQ("HAVE_UNISTD_H", artifacts.private_options.defines[0]);
    ASSERT_EQ(2u, artifacts.private_options.flag_count);
    EXPECT_STREQ("-fno-strict-aliasing", artifacts.private_options.flags[0]);
}

DESCRIBE(a_recipe_keeps_its_private_options_out_of_its_interface) {
    for_both(SCOPED_TOML, SCOPED_JSON, check_scoped);
}

/* A recipe that only ever needed to silence one warning declares nothing
   directly under `[artifacts]`. Asking whether that table exists would skip the
   whole read and lose the one thing the recipe was written to say. */
DESCRIBE(a_recipe_whose_only_statement_is_private_is_still_read) {
    static const char *const private_only = "schema = 1\nform = \"source\"\n" MINIMUM
                                            "[artifacts.private]\nflags = [\"-Wno-unused\"]\n";
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of(private_only, &artifacts, err, sizeof err));

    ASSERT_EQ(1u, artifacts.private_options.flag_count);
    EXPECT_STREQ("-Wno-unused", artifacts.private_options.flags[0]);
    /* And the absent table still means what it always meant. */
    EXPECT_EQ(recipe_artifact_static, artifacts.type);
}

/* Nothing private is the ordinary case, and it has to read as empty rather
   than as whatever the interface said. */
DESCRIBE(a_recipe_without_the_private_table_keeps_nothing_back) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of(SQLITE_TOML, &artifacts, err, sizeof err));

    EXPECT_EQ(3u, artifacts.options.define_count);
    EXPECT_EQ(0u, artifacts.private_options.define_count);
    EXPECT_EQ(0u, artifacts.private_options.include_count);
    EXPECT_EQ(0u, artifacts.private_options.flag_count);
}

/* --- what differs by operating system --- */

/* libxml2's whole platform difference: `-lm` where there is a libm, and
   BCryptGenRandom's import library on Windows. */
static const char *const PER_OS_TOML = "schema = 4\n"
                                       "form = \"source\"\n" MINIMUM "[artifacts]\n"
                                       "type = \"source\"\n"
                                       "sources = [\"dict.c\"]\n"
                                       "defines = [\"LIBXML_STATIC\"]\n"
                                       "[artifacts.linux]\n"
                                       "link = [\"m\"]\n"
                                       "[artifacts.macos]\n"
                                       "link = [\"m\"]\n"
                                       "[artifacts.windows]\n"
                                       "link = [\"bcrypt\"]\n"
                                       "sources = [\"win32.c\"]\n"
                                       "defines = [\"WIN32_LEAN_AND_MEAN\"]\n"
                                       "[artifacts.private.windows]\n"
                                       "defines = [\"_CRT_SECURE_NO_WARNINGS\"]\n";

static const char *const PER_OS_JSON =
    "{\"schema\":4,\"form\":\"source\",\"kind\":\"package\",\"name\":\"x\","
    "\"version\":\"1.0.0\",\"target\":\"any\","
    "\"artifacts\":{\"type\":\"source\",\"sources\":[\"dict.c\"],\"defines\":[\"LIBXML_STATIC\"],"
    "\"linux\":{\"link\":[\"m\"]},\"macos\":{\"link\":[\"m\"]},"
    "\"windows\":{\"link\":[\"bcrypt\"],\"sources\":[\"win32.c\"],"
    "\"defines\":[\"WIN32_LEAN_AND_MEAN\"]},"
    "\"private\":{\"windows\":{\"defines\":[\"_CRT_SECURE_NO_WARNINGS\"]}}}}";

static void check_per_os_on_windows(doc_view doc) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(recipe_read_artifacts(doc, &artifacts, err, sizeof err));
    ASSERT_TRUE(recipe_artifacts_select_os(&artifacts, recipe_os_windows, err, sizeof err));

    /* Appended after what every platform gets, never in its place. */
    ASSERT_EQ(1u, artifacts.link_count);
    EXPECT_STREQ("bcrypt", artifacts.link[0]);
    ASSERT_EQ(2u, artifacts.source_count);
    EXPECT_STREQ("dict.c", artifacts.sources[0]);
    EXPECT_STREQ("win32.c", artifacts.sources[1]);
    ASSERT_EQ(2u, artifacts.options.define_count);
    EXPECT_STREQ("LIBXML_STATIC", artifacts.options.defines[0]);
    EXPECT_STREQ("WIN32_LEAN_AND_MEAN", artifacts.options.defines[1]);
    ASSERT_EQ(1u, artifacts.private_options.define_count);
    EXPECT_STREQ("_CRT_SECURE_NO_WARNINGS", artifacts.private_options.defines[0]);
}

DESCRIBE(a_per_os_table_adds_to_artifacts_on_its_own_os) {
    for_both(PER_OS_TOML, PER_OS_JSON, check_per_os_on_windows);
}

static void check_per_os_on_linux(doc_view doc) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(recipe_read_artifacts(doc, &artifacts, err, sizeof err));
    ASSERT_TRUE(recipe_artifacts_select_os(&artifacts, recipe_os_linux, err, sizeof err));

    ASSERT_EQ(1u, artifacts.link_count);
    EXPECT_STREQ("m", artifacts.link[0]);
    EXPECT_EQ(1u, artifacts.source_count);
    EXPECT_EQ(1u, artifacts.options.define_count);
    EXPECT_EQ(0u, artifacts.private_options.define_count);
}

DESCRIBE(a_per_os_table_adds_nothing_on_another_os) {
    for_both(PER_OS_TOML, PER_OS_JSON, check_per_os_on_linux);
}

/* An OS no table names gets `[artifacts]` alone, and selecting twice does not
   append twice. */
DESCRIBE(selecting_an_os_is_done_once) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of(PER_OS_TOML, &artifacts, err, sizeof err));
    ASSERT_TRUE(recipe_artifacts_select_os(&artifacts, recipe_os_none, err, sizeof err));
    EXPECT_EQ(0u, artifacts.link_count);
    ASSERT_TRUE(recipe_artifacts_select_os(&artifacts, recipe_os_windows, err, sizeof err));
    EXPECT_EQ(0u, artifacts.link_count);
}

/* A molto that predates the tables ignores them, so a recipe using one must
   declare the schema that makes such a molto refuse it instead. */
DESCRIBE(a_per_os_table_below_its_schema_is_refused) {
    static const char *const old = "schema = 3\nform = \"source\"\n" MINIMUM
                                   "[artifacts.windows]\nlink = [\"bcrypt\"]\n";
    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of(old, &artifacts, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[artifacts.windows] needs schema 4"));
}

/* With no `sources`, every file is compiled; adding one on Windows would mean
   only that one there. */
DESCRIBE(per_os_sources_need_a_sources_list_to_add_to) {
    static const char *const all = "schema = 4\nform = \"source\"\n" MINIMUM
                                   "[artifacts.windows]\nsources = [\"win32.c\"]\n";
    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of(all, &artifacts, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[artifacts.windows].sources"));
}

DESCRIBE(the_os_a_triple_names) {
    EXPECT_EQ(recipe_os_windows, recipe_os_for_platform("x86_64-w64-mingw32"));
    EXPECT_EQ(recipe_os_windows, recipe_os_for_platform("x86_64-pc-windows-msvc"));
    EXPECT_EQ(recipe_os_macos, recipe_os_for_platform("aarch64-apple-darwin"));
    EXPECT_EQ(recipe_os_linux, recipe_os_for_platform("x86_64-unknown-linux-gnu"));
    EXPECT_EQ(recipe_os_none, recipe_os_for_platform("x86_64-unknown-freebsd"));
#if defined(_WIN32)
    EXPECT_EQ(recipe_os_windows, recipe_os_for_platform(NULL));
#elif defined(__APPLE__)
    EXPECT_EQ(recipe_os_macos, recipe_os_for_platform(""));
#else
    EXPECT_EQ(recipe_os_linux, recipe_os_for_platform(""));
#endif
}

/* --- the standard a package's own sources compile with --- */

static const char *const STD_TOML = "schema = 1\n"
                                    "form = \"source\"\n"
                                    "kind = \"package\"\n"
                                    "name = \"legacy\"\n"
                                    "version = \"1.0.0\"\n"
                                    "target = \"any\"\n"
                                    "\n"
                                    "[artifacts]\n"
                                    "type = \"source\"\n"
                                    "std = \"c99\"\n"
                                    "cpp_std = \"c++17\"\n";

static const char *const STD_JSON =
    "{\"schema\":1,\"form\":\"source\",\"kind\":\"package\",\"name\":\"legacy\","
    "\"version\":\"1.0.0\",\"target\":\"any\","
    "\"artifacts\":{\"type\":\"source\",\"std\":\"c99\",\"cpp_std\":\"c++17\"}}";

static void check_std(doc_view doc) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(recipe_read_artifacts(doc, &artifacts, err, sizeof err));
    EXPECT_STREQ("c99", artifacts.std);
    EXPECT_STREQ("c++17", artifacts.cpp_std);
}

DESCRIBE(a_recipe_may_name_the_standard_its_sources_compile_with) {
    for_both(STD_TOML, STD_JSON, check_std);
}

/* Saying nothing is how a package says it never had an opinion, and an empty
   standard is what makes it inherit the consumer's. */
DESCRIBE(a_recipe_that_names_no_standard_inherits_one) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of(SQLITE_TOML, &artifacts, err, sizeof err));
    EXPECT_STREQ("", artifacts.std);
    EXPECT_STREQ("", artifacts.cpp_std);
}

/* Each language decides separately: naming one leaves the other inherited,
   which is what a C library with a single C++ shim needs. */
DESCRIBE(one_standard_named_leaves_the_other_alone) {
    recipe_artifacts artifacts;
    char err[256] = "";
    ASSERT_TRUE(read_artifacts_of("[artifacts]\nstd = \"c11\"\n", &artifacts, err, sizeof err));
    EXPECT_STREQ("c11", artifacts.std);
    EXPECT_STREQ("", artifacts.cpp_std);
}

/* A recipe is written by one person and read by everyone who depends on them,
   so the typo has to fail here rather than in their build, under a compiler
   option none of them wrote. */
DESCRIBE(a_recipe_rejects_a_standard_molto_cannot_place) {
    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of("[artifacts]\nstd = \"C99\"\n", &artifacts, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[artifacts].std"));
    EXPECT_NOT_NULL(strstr(err, "C99"));
}

/* `c++20` is a real standard and still the wrong answer to `std`. Checking each
   key against its own language is what catches it. */
DESCRIBE(a_recipe_rejects_a_standard_of_the_other_language) {
    recipe_artifacts artifacts;
    char err[256] = "";
    EXPECT_FALSE(read_artifacts_of("[artifacts]\nstd = \"c++20\"\n", &artifacts, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "c++20"));

    char other[256] = "";
    EXPECT_FALSE(read_artifacts_of("[artifacts]\ncpp_std = \"c11\"\n", &artifacts, other,
                                   sizeof other));
    EXPECT_NOT_NULL(strstr(other, "[artifacts].cpp_std"));
}

/* --- the build system a source recipe's sources need --- */

/* Absent is `none`, which is the rule `schema` and `form` already follow and is
   what every source recipe published before this key meant. */
DESCRIBE(a_recipe_with_no_build_table_needs_no_build_system) {
    recipe_build build;
    char err[256] = "";
    EXPECT_TRUE(read_build_of("[artifacts]\ntype = \"source\"\n", &build, err, sizeof err));
    EXPECT_EQ(recipe_build_none, build.system);
}

DESCRIBE(a_recipe_reads_every_build_system_the_format_names) {
    static const struct {
        const char *name;
        recipe_build_system system;
    } CASES[] = {
        {"none", recipe_build_none},
        {"make", recipe_build_make},
        {"cmake", recipe_build_cmake},
        {"autotools", recipe_build_autotools},
        {"meson", recipe_build_meson},
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char text[64];
        snprintf(text, sizeof text, "[build]\nsystem = \"%s\"\n", CASES[i].name);

        recipe_build build;
        char err[256] = "";
        EXPECT_TRUE(read_build_of(text, &build, err, sizeof err));
        EXPECT_EQ(CASES[i].system, build.system);
        EXPECT_STREQ(CASES[i].name, recipe_build_system_name(build.system));
    }
}

/* Neither `sh -c` on a stranger's word nor a quiet fall back to `none`: one
   would run what the recipe named and the other would compile sources that were
   told they need configuring first (RFC-0009). */
/* --- a delegated configure --- */

/* libpq's: upstream's configure with its arguments, a variable for it, and a
   header a Makefile rule writes. */
DESCRIBE(a_delegated_autotools_build_reads_what_configure_is_given) {
    recipe_build build;
    char err[256] = "";
    ASSERT_TRUE(read_build_of("[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                              "args = [\"--without-icu\", \"--without-zlib\"]\n"
                              "env = { PKG_CONFIG = \"true\" }\n"
                              "targets = [\"src/port/pg_config_paths.h\"]\n",
                              &build, err, sizeof err));
    EXPECT_TRUE(recipe_build_configures(&build));
    ASSERT_EQ(2u, build.arg_count);
    EXPECT_STREQ("--without-zlib", build.args[1]);
    ASSERT_EQ(1u, build.env_count);
    EXPECT_STREQ("PKG_CONFIG=true", build.env[0]);
    ASSERT_EQ(1u, build.target_count);
    EXPECT_STREQ("src/port/pg_config_paths.h", build.targets[0]);
}

/* --- sources read from the build (RFC-0025) --- */

/* FFmpeg's: make says what it compiles, for the libraries named. */
DESCRIBE(a_delegated_build_may_say_what_it_compiles) {
    recipe_build build;
    char err[256] = "";
    ASSERT_TRUE(read_build_of("schema = 5\n[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                              "args = [\"--cc={cc}\"]\nsources = \"make\"\n"
                              "goals = [\"libavutil/libavutil.a\"]\n",
                              &build, err, sizeof err));
    EXPECT_EQ(recipe_sources_make, build.sources);
    ASSERT_EQ(1u, build.goal_count);
    EXPECT_STREQ("libavutil/libavutil.a", build.goals[0]);

    ASSERT_TRUE(read_build_of("schema = 5\n[build]\nsystem = \"cmake\"\nvia = \"delegate\"\n"
                              "sources = \"cmake\"\n",
                              &build, err, sizeof err));
    EXPECT_EQ(recipe_sources_cmake, build.sources);
}

DESCRIBE(a_recipe_lists_its_own_sources_by_default) {
    recipe_build build;
    char err[256] = "";
    ASSERT_TRUE(read_build_of("[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n", &build, err,
                              sizeof err));
    EXPECT_EQ(recipe_sources_recipe, build.sources);
}

/* An older molto would read no [artifacts].sources and compile the tarball. */
DESCRIBE(sources_from_the_build_need_schema_5) {
    recipe_build build;
    char err[256] = "";
    EXPECT_FALSE(read_build_of("[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                               "sources = \"make\"\n",
                               &build, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "schema 5"));
}

/* FFmpeg's: zlib built before configure, as the -lz its probe links. */
DESCRIBE(a_configuration_may_see_a_dependency_built) {
    recipe_build build;
    char err[256] = "";
    ASSERT_TRUE(read_build_of("schema = 5\n[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                              "libraries = { zlib = \"z\" }\n",
                              &build, err, sizeof err));
    ASSERT_EQ(1u, build.library_count);
    EXPECT_STREQ("zlib", build.libraries[0].package);
    EXPECT_STREQ("z", build.libraries[0].library);

    EXPECT_FALSE(read_build_of("[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                               "libraries = { zlib = \"z\" }\n",
                               &build, err, sizeof err));
    EXPECT_FALSE(read_build_of("schema = 5\n[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                               "libraries = { zlib = \"-lz\" }\n",
                               &build, err, sizeof err));
}

DESCRIBE(sources_from_the_build_are_refused_where_they_cannot_be_answered) {
    recipe_build build;
    char err[256] = "";
    /* cmake writes no Makefile to ask, and make no database. */
    EXPECT_FALSE(read_build_of("schema = 5\n[build]\nsystem = \"cmake\"\nvia = \"delegate\"\n"
                               "sources = \"make\"\n",
                               &build, err, sizeof err));
    /* Nothing configured, nothing to ask. */
    EXPECT_FALSE(read_build_of("schema = 5\n[build]\nsystem = \"autotools\"\nsources = \"make\"\n",
                               &build, err, sizeof err));
    /* Two answers to one question. */
    EXPECT_FALSE(read_build_of("schema = 5\n[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                               "sources = \"make\"\n[artifacts]\nsources = [\"a.c\"]\n",
                               &build, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[artifacts].sources"));
    /* goals are make's. */
    EXPECT_FALSE(read_build_of("schema = 5\n[build]\nsystem = \"cmake\"\nvia = \"delegate\"\n"
                               "sources = \"cmake\"\ngoals = [\"all\"]\n",
                               &build, err, sizeof err));
    EXPECT_FALSE(read_build_of("schema = 5\n[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                               "sources = \"make\"\ngoals = [\"../up\"]\n",
                               &build, err, sizeof err));
}

/* Only autotools, delegated, is a build molto runs; the system alone is not. */
DESCRIBE(an_autotools_build_that_does_not_delegate_is_not_run) {
    recipe_build build;
    char err[256] = "";
    ASSERT_TRUE(read_build_of("[build]\nsystem = \"autotools\"\n", &build, err, sizeof err));
    EXPECT_FALSE(recipe_build_configures(&build));
}

DESCRIBE(configure_arguments_need_a_delegated_build) {
    recipe_build build;
    char err[256] = "";
    EXPECT_FALSE(read_build_of("[build]\nsystem = \"autotools\"\nargs = [\"--x\"]\n", &build, err,
                               sizeof err));
    EXPECT_NOT_NULL(strstr(err, "delegate"));
}

DESCRIBE(a_configure_target_stays_inside_the_source) {
    recipe_build build;
    char err[256] = "";
    EXPECT_FALSE(read_build_of("[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                               "targets = [\"../outside.h\"]\n",
                               &build, err, sizeof err));
    EXPECT_FALSE(read_build_of("[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                               "targets = [\"-f evil\"]\n",
                               &build, err, sizeof err));
}

DESCRIBE(a_configure_variable_is_a_variable) {
    recipe_build build;
    char err[256] = "";
    EXPECT_FALSE(read_build_of("[build]\nsystem = \"autotools\"\nvia = \"delegate\"\n"
                               "env = { \"A B\" = \"x\" }\n",
                               &build, err, sizeof err));
}

DESCRIBE(a_recipe_rejects_a_build_system_molto_does_not_know) {
    recipe_build build;
    char err[256] = "";
    EXPECT_FALSE(read_build_of("[build]\nsystem = \"scons\"\n", &build, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "scons"));
}

DESCRIBE(a_recipe_rejects_a_build_system_that_is_not_a_string) {
    recipe_build build;
    char err[256] = "";
    EXPECT_FALSE(read_build_of("[build]\nsystem = 3\n", &build, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[build].system"));
}

/* --- the files a recipe copies into place --- */

DESCRIBE(a_recipe_with_no_provide_list_copies_nothing) {
    recipe_provide provide;
    char err[256] = "";
    EXPECT_TRUE(read_provide_of("[build]\nsystem = \"none\"\n", &provide, err, sizeof err));
    EXPECT_EQ(0u, provide.count);
}

DESCRIBE(a_recipe_reads_what_it_provides_in_order) {
    recipe_provide provide;
    char err[256] = "";
    ASSERT_TRUE(read_provide_of("[[provide]]\nfile = \"pnglibconf.h\"\n"
                                "from = \"scripts/pnglibconf.h.prebuilt\"\n"
                                "[[provide]]\nfile = \"config.h\"\nfrom = \"config.h.generic\"\n",
                                &provide, err, sizeof err));
    ASSERT_EQ(2u, provide.count);
    EXPECT_STREQ("pnglibconf.h", provide.items[0].file);
    EXPECT_STREQ("scripts/pnglibconf.h.prebuilt", provide.items[0].from);
    EXPECT_STREQ("config.h", provide.items[1].file);
}

/* Half an entry says half a thing, and guessing the other half is how a recipe
   comes to mean something nobody wrote. */
DESCRIBE(a_recipe_rejects_a_provision_missing_either_half) {
    recipe_provide provide;
    char err[256] = "";
    EXPECT_FALSE(read_provide_of("[[provide]]\nfile = \"config.h\"\n", &provide, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "from"));

    char other[256] = "";
    EXPECT_FALSE(read_provide_of("[[provide]]\nfrom = \"config.h.in\"\n", &provide, other,
                                 sizeof other));
    EXPECT_NOT_NULL(strstr(other, "file"));
}

/* Reported rather than truncated: an entry silently dropped is a header that is
   never written and a compiler error naming neither the recipe nor the key. */
DESCRIBE(a_recipe_rejects_more_provisions_than_the_format_allows) {
    char text[1024] = "";
    size_t used = 0;
    for (int i = 0; i < RECIPE_MAX_PROVIDE + 1; i++) {
        used += (size_t)snprintf(text + used, sizeof text - used,
                                 "[[provide]]\nfile = \"h%d.h\"\nfrom = \"in%d.h\"\n", i, i);
    }

    recipe_provide provide;
    char err[256] = "";
    EXPECT_FALSE(read_provide_of(text, &provide, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "at most"));
}
