#include <moltest.h>

#include <molto/util/toml.h>

#include <stddef.h>
#include <string.h>

/* A struct bound from TOML, including a nested member. */
typedef struct {
    struct {
        char name[64];
    } package;
    long width;
    bool enabled;
} bound_config;

/* The document most of these tests parse. */
static const char *sample_document(void) {
    return
        "# a comment line\r\n"
        "[ package ]\n"
        "name = \"molto\"   # inline comment\n"
        "version = 'literal string'\n"
        "\n"
        "[settings]\n"
        "width = 1_000\n"
        "offset = -42\n"
        "enabled = true\n"
        "disabled = false\n"
        "note = \"tab\\there\"\n";
}

MOLTEST(toml_parses_sections_and_strings) {
    char err[256] = "";
    toml_document *doc = toml_parse(sample_document(), err, sizeof err);
    ASSERT_NOT_NULL(doc);

    char buffer[64];
    EXPECT_TRUE(toml_get_string(doc, "package", "name", buffer, sizeof buffer));
    EXPECT_STREQ("molto", buffer);            /* inline comment stripped */
    EXPECT_TRUE(toml_get_string(doc, "package", "version", buffer, sizeof buffer));
    EXPECT_STREQ("literal string", buffer);   /* literal string */
    EXPECT_TRUE(toml_get_string(doc, "settings", "note", buffer, sizeof buffer));
    EXPECT_STREQ("tab\there", buffer);        /* escape decoded */

    EXPECT_TRUE(toml_has_section(doc, "settings"));
    EXPECT_FALSE(toml_has_section(doc, "nope"));
    toml_free(doc);
}

MOLTEST(toml_parses_integers) {
    char err[256] = "";
    toml_document *doc = toml_parse(sample_document(), err, sizeof err);
    ASSERT_NOT_NULL(doc);

    long value = 0;
    EXPECT_TRUE(toml_get_int(doc, "settings", "width", &value));
    EXPECT_EQ(1000, value);                   /* '_' separator */
    EXPECT_TRUE(toml_get_int(doc, "settings", "offset", &value));
    EXPECT_EQ(-42, value);                    /* signed */
    toml_free(doc);
}

MOLTEST(toml_parses_booleans) {
    char err[256] = "";
    toml_document *doc = toml_parse(sample_document(), err, sizeof err);
    ASSERT_NOT_NULL(doc);

    bool flag = false;
    EXPECT_TRUE(toml_get_bool(doc, "settings", "enabled", &flag));
    EXPECT_TRUE(flag);
    EXPECT_TRUE(toml_get_bool(doc, "settings", "disabled", &flag));
    EXPECT_FALSE(flag);
    toml_free(doc);
}

MOLTEST(toml_getters_reject_wrong_type_or_missing_key) {
    char err[256] = "";
    toml_document *doc = toml_parse(sample_document(), err, sizeof err);
    ASSERT_NOT_NULL(doc);

    long value = 0;
    char buffer[64];
    EXPECT_FALSE(toml_get_int(doc, "package", "name", &value));   /* it is a string */
    EXPECT_FALSE(toml_get_string(doc, "package", "missing", buffer, sizeof buffer));
    toml_free(doc);
}

MOLTEST(toml_binds_a_schema_into_a_struct) {
    char err[256] = "";
    toml_document *doc = toml_parse(sample_document(), err, sizeof err);
    ASSERT_NOT_NULL(doc);

    bound_config config;
    memset(&config, 0, sizeof config);
    const toml_field schema[] = {
        TOML_STR(bound_config, "package", "name", package.name), /* nested member */
        TOML_INT(bound_config, "settings", "width", width),
        TOML_BOOL(bound_config, "settings", "enabled", enabled),
    };
    EXPECT_TRUE(toml_bind(doc, schema, sizeof schema / sizeof schema[0], &config,
                          err, sizeof err));
    EXPECT_STREQ("molto", config.package.name);
    EXPECT_EQ(1000, config.width);
    EXPECT_TRUE(config.enabled);
    toml_free(doc);
}

MOLTEST(toml_bind_keeps_defaults_and_rejects_mismatch) {
    char err[256] = "";
    toml_document *doc = toml_parse(sample_document(), err, sizeof err);
    ASSERT_NOT_NULL(doc);

    /* An absent key leaves the seeded default untouched. */
    bound_config defaulted;
    memset(&defaulted, 0, sizeof defaulted);
    strcpy(defaulted.package.name, "kept");
    const toml_field absent[] = {
        TOML_STR(bound_config, "package", "nonexistent", package.name),
    };
    EXPECT_TRUE(toml_bind(doc, absent, 1, &defaulted, err, sizeof err));
    EXPECT_STREQ("kept", defaulted.package.name);

    /* Binding a string key to an int field fails. */
    const toml_field mismatch[] = {
        TOML_INT(bound_config, "package", "name", width),
    };
    EXPECT_FALSE(toml_bind(doc, mismatch, 1, &defaulted, err, sizeof err));
    toml_free(doc);
}

MOLTEST(toml_parses_string_arrays) {
    char err[256] = "";
    toml_document *doc = toml_parse(
        "[target]\nlink = [\"m\", \"pthread\"]\nempty = []\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    str_list libs;
    str_list_init(&libs);
    EXPECT_TRUE(toml_get_array(doc, "target", "link", &libs));
    EXPECT_EQ(2, str_list_count(&libs));
    EXPECT_STREQ("m", str_list_get(&libs, 0));
    EXPECT_STREQ("pthread", str_list_get(&libs, 1));
    str_list_free(&libs);

    str_list empty;
    str_list_init(&empty);
    EXPECT_TRUE(toml_get_array(doc, "target", "empty", &empty));
    EXPECT_EQ(0, str_list_count(&empty));
    str_list_free(&empty);

    /* An array is not readable as a scalar string. */
    char buffer[64];
    EXPECT_FALSE(toml_get_string(doc, "target", "link", buffer, sizeof buffer));
    toml_free(doc);

    /* An unterminated array is a parse error. */
    EXPECT_NULL(toml_parse("[t]\nx = [\"a\", \"b\"\n", err, sizeof err));
}

MOLTEST(toml_reads_an_inline_table_as_a_subsection) {
    /* This used to be skipped in silence: the dependency was written, the file
       parsed, and nothing was there. It now reads back exactly as the
       equivalent [deps.http] header would. */
    char err[256] = "";
    toml_document *doc = toml_parse(
        "[package]\nname = \"x\"\n[deps]\nhttp = { path = \"m\" }\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    char buffer[64] = "";
    EXPECT_TRUE(toml_get_string(doc, "package", "name", buffer, sizeof buffer));
    EXPECT_STREQ("x", buffer);
    EXPECT_TRUE(toml_get_string(doc, "deps.http", "path", buffer, sizeof buffer));
    EXPECT_STREQ("m", buffer);
    toml_free(doc);
}

MOLTEST(toml_reads_every_member_of_an_inline_table) {
    char err[256] = "";
    toml_document *doc =
        toml_parse("[deps]\nsqlite = { git = \"https://x/y.git\", tag = \"3.53.4\" }\n", err,
                   sizeof err);
    ASSERT_NOT_NULL(doc);

    char git[64] = "";
    char tag[32] = "";
    EXPECT_TRUE(toml_get_string(doc, "deps.sqlite", "git", git, sizeof git));
    EXPECT_STREQ("https://x/y.git", git);
    EXPECT_TRUE(toml_get_string(doc, "deps.sqlite", "tag", tag, sizeof tag));
    EXPECT_STREQ("3.53.4", tag);
    toml_free(doc);
}

MOLTEST(toml_enumerates_an_inline_table_like_any_other_section) {
    /* A dependency reader has to discover the keys it was given, because which
       source a dependency uses is what the keys say. */
    char err[256] = "";
    toml_document *doc =
        toml_parse("[deps]\nhttp = { path = \"modules/http\" }\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    EXPECT_TRUE(toml_has_section(doc, "deps.http"));

    str_list keys;
    str_list_init(&keys);
    EXPECT_TRUE(toml_section_keys(doc, "deps.http", &keys));
    ASSERT_EQ(1u, keys.count);
    EXPECT_STREQ("path", keys.items[0]);
    str_list_free(&keys);
    toml_free(doc);
}

MOLTEST(toml_reads_values_of_every_type_inside_an_inline_table) {
    char err[256] = "";
    toml_document *doc = toml_parse(
        "[build]\ncfg = { jobs = true, level = 3, args = [\"--a\", \"--b\"] }\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    bool jobs = false;
    long level = 0;
    EXPECT_TRUE(toml_get_bool(doc, "build.cfg", "jobs", &jobs));
    EXPECT_TRUE(jobs);
    EXPECT_TRUE(toml_get_int(doc, "build.cfg", "level", &level));
    EXPECT_EQ(3L, level);

    /* The commas inside the array belong to the array, not to the table. */
    str_list args;
    str_list_init(&args);
    EXPECT_TRUE(toml_get_array(doc, "build.cfg", "args", &args));
    ASSERT_EQ(2u, args.count);
    EXPECT_STREQ("--b", args.items[1]);
    str_list_free(&args);
    toml_free(doc);
}

MOLTEST(toml_keeps_a_comma_inside_a_string_out_of_the_split) {
    char err[256] = "";
    toml_document *doc =
        toml_parse("[about]\nwho = { name = \"Doe, J\", role = \"author\" }\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    char name[32] = "";
    char role[32] = "";
    EXPECT_TRUE(toml_get_string(doc, "about.who", "name", name, sizeof name));
    EXPECT_STREQ("Doe, J", name);
    EXPECT_TRUE(toml_get_string(doc, "about.who", "role", role, sizeof role));
    EXPECT_STREQ("author", role);
    toml_free(doc);
}

MOLTEST(toml_reads_a_nested_inline_table) {
    char err[256] = "";
    toml_document *doc =
        toml_parse("[deps]\nhttp = { git = \"u\", opts = { tls = true } }\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    bool tls = false;
    EXPECT_TRUE(toml_get_bool(doc, "deps.http.opts", "tls", &tls));
    EXPECT_TRUE(tls);
    toml_free(doc);
}

MOLTEST(toml_refuses_an_empty_inline_table) {
    /* It declares nothing, so it leaves no entry behind, so no reader can see
       it — and the only thing it has ever been is a half-written dependency.
       Real TOML allows it; this parser is a subset that fails closed. */
    char err[256] = "";
    EXPECT_NULL(toml_parse("[deps]\nhttp = {}\nname = \"x\"\n", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "empty inline table"));
}

MOLTEST(toml_still_tolerates_a_trailing_comma) {
    /* Distinct from the case above: this table declares something. */
    char err[256] = "";
    toml_document *doc = toml_parse("[deps]\nhttp = { path = \"m\", }\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    char path[32] = "";
    EXPECT_TRUE(toml_get_string(doc, "deps.http", "path", path, sizeof path));
    EXPECT_STREQ("m", path);
    toml_free(doc);
}

MOLTEST(toml_section_members_lists_values_and_tables_in_declaration_order) {
    /* The reason this exists: `sqlite = { … }` stores nothing under "deps", so
       toml_section_keys sees only yyjson and a [deps] reader built on it would
       read half the manifest without complaining. */
    char err[256] = "";
    toml_document *doc = toml_parse("[deps]\n"
                                    "yyjson = \"1.2.32\"\n"
                                    "sqlite = { git = \"u\", tag = \"v\" }\n"
                                    "[deps.http]\n"
                                    "path = \"modules/http\"\n",
                                    err, sizeof err);
    ASSERT_NOT_NULL(doc);

    str_list members;
    str_list_init(&members);
    EXPECT_TRUE(toml_section_members(doc, "deps", &members));
    ASSERT_EQ(3u, members.count);
    EXPECT_STREQ("yyjson", members.items[0]);
    EXPECT_STREQ("sqlite", members.items[1]);
    EXPECT_STREQ("http", members.items[2]);
    str_list_free(&members);
    toml_free(doc);
}

MOLTEST(toml_section_members_lists_a_child_once_and_never_a_grandchild) {
    char err[256] = "";
    toml_document *doc =
        toml_parse("[deps]\nhttp = { git = \"u\", opts = { tls = true } }\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    str_list members;
    str_list_init(&members);
    EXPECT_TRUE(toml_section_members(doc, "deps", &members));
    ASSERT_EQ(1u, members.count);
    EXPECT_STREQ("http", members.items[0]);
    str_list_free(&members);
    toml_free(doc);
}

MOLTEST(toml_section_members_of_the_root_lists_keys_and_top_level_tables) {
    char err[256] = "";
    toml_document *doc =
        toml_parse("schema = 1\nform = \"source\"\n[source]\ngit = \"u\"\n[build]\nsystem = "
                   "\"none\"\n",
                   err, sizeof err);
    ASSERT_NOT_NULL(doc);

    str_list members;
    str_list_init(&members);
    EXPECT_TRUE(toml_section_members(doc, "", &members));
    ASSERT_EQ(4u, members.count);
    EXPECT_STREQ("schema", members.items[0]);
    EXPECT_STREQ("form", members.items[1]);
    EXPECT_STREQ("source", members.items[2]);
    EXPECT_STREQ("build", members.items[3]);
    str_list_free(&members);
    toml_free(doc);
}

MOLTEST(toml_section_members_is_empty_for_a_section_nobody_declared) {
    char err[256] = "";
    toml_document *doc = toml_parse("[package]\nname = \"x\"\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    str_list members;
    str_list_init(&members);
    EXPECT_TRUE(toml_section_members(doc, "deps", &members));
    EXPECT_EQ(0u, members.count);
    str_list_free(&members);
    toml_free(doc);
}

MOLTEST(toml_section_members_leaves_an_array_of_tables_to_its_own_accessor) {
    char err[256] = "";
    toml_document *doc = toml_parse("name = \"x\"\n[[tool]]\npath = \"a\"\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    str_list members;
    str_list_init(&members);
    EXPECT_TRUE(toml_section_members(doc, "", &members));
    ASSERT_EQ(1u, members.count);
    EXPECT_STREQ("name", members.items[0]);
    EXPECT_EQ(1u, toml_table_array_count(doc, "tool"));
    str_list_free(&members);
    toml_free(doc);
}

MOLTEST(toml_reports_an_inline_table_that_is_never_closed) {
    /* TOML forbids a newline inside an inline table, so this is not a value
       continued on the next line: it is one that never ends. Reporting it here
       is the difference between a named error and 'expected =' three lines
       further down. */
    char err[256] = "";
    EXPECT_NULL(toml_parse("[deps]\nhttp = { path = \"m\"\nname = \"x\"\n", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "unterminated inline table"));
}

MOLTEST(toml_reports_a_malformed_member_instead_of_dropping_it) {
    char err[256] = "";
    EXPECT_NULL(toml_parse("[deps]\nhttp = { path }\n", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "expected '='"));
}

MOLTEST(toml_reports_characters_after_an_inline_table) {
    char err[256] = "";
    EXPECT_NULL(toml_parse("[deps]\nhttp = { path = \"m\" } junk\n", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "trailing characters"));
}

MOLTEST(toml_reports_malformed_input_with_a_line) {
    char err[256] = "";
    EXPECT_NULL(toml_parse("[package\nname = \"x\"\n", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "Project.toml:1"));

    err[0] = '\0';
    EXPECT_NULL(toml_parse("[package]\nname \"x\"\n", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "Project.toml:2"));

    err[0] = '\0';
    EXPECT_NULL(toml_parse("[package]\nname = \"unterminated\n", err, sizeof err));

    err[0] = '\0';
    EXPECT_NULL(toml_parse("[package]\nn = 99999999999999999999999999\n", err, sizeof err));
}

MOLTEST(toml_accepts_an_empty_document) {
    char err[256] = "";
    toml_document *doc = toml_parse("# just a comment\n\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);
    EXPECT_FALSE(toml_has_section(doc, "package"));
    toml_free(doc);
}

/* The answer `pickup tools --format toml` gives, verbatim. Molto reads it to
   learn which formatter and linter this machine has, and where. */
static const char pickup_tools_answer[] =
    "[[tool]]\n"
    "kind = \"formatter\"\n"
    "name = \"clang-format\"\n"
    "path = \"/home/u/.pickup/toolchains/clang-22.1.8/bin/clang-format\"\n"
    "version = \"clang-format version 22.1.8\"\n"
    "source = \"pickup\"\n"
    "\n"
    "[[tool]]\n"
    "kind = \"linter\"\n"
    "name = \"clang-tidy\"\n"
    "path = \"/home/u/.pickup/toolchains/clang-22.1.8/bin/clang-tidy\"\n"
    "version = \"LLVM version 22.1.8\"\n"
    "source = \"pickup\"\n";

MOLTEST(toml_reads_the_answer_of_pickup_tools) {
    char err[256] = "";
    toml_document *doc = toml_parse(pickup_tools_answer, err, sizeof err);
    ASSERT_NOT_NULL(doc);

    ASSERT_EQ(2, (int)toml_table_array_count(doc, "tool"));

    /* Each [[tool]] is its own section, so the ordinary accessors read it. The
       second must not have replaced the first. */
    char section[TOML_SECTION_MAX];
    char value[256];

    ASSERT_TRUE(toml_table_array_section("tool", 0, section, sizeof section));
    EXPECT_TRUE(toml_get_string(doc, section, "kind", value, sizeof value));
    EXPECT_STREQ("formatter", value);
    EXPECT_TRUE(toml_get_string(doc, section, "name", value, sizeof value));
    EXPECT_STREQ("clang-format", value);

    ASSERT_TRUE(toml_table_array_section("tool", 1, section, sizeof section));
    EXPECT_TRUE(toml_get_string(doc, section, "kind", value, sizeof value));
    EXPECT_STREQ("linter", value);
    EXPECT_TRUE(toml_get_string(doc, section, "path", value, sizeof value));
    EXPECT_STREQ("/home/u/.pickup/toolchains/clang-22.1.8/bin/clang-tidy", value);

    toml_free(doc);
}

MOLTEST(toml_counts_no_tables_for_an_array_that_is_not_there) {
    char err[256] = "";
    toml_document *doc = toml_parse("[package]\nname = \"x\"\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);
    EXPECT_EQ(0, (int)toml_table_array_count(doc, "tool"));
    EXPECT_EQ(0, (int)toml_table_array_count(doc, "package"));
    toml_free(doc);
}

MOLTEST(toml_keeps_interleaved_arrays_of_tables_apart) {
    char err[256] = "";
    toml_document *doc = toml_parse("[[a]]\nx = 1\n[[b]]\nx = 2\n[[a]]\nx = 3\n",
                                    err, sizeof err);
    ASSERT_NOT_NULL(doc);
    EXPECT_EQ(2, (int)toml_table_array_count(doc, "a"));
    EXPECT_EQ(1, (int)toml_table_array_count(doc, "b"));

    /* Each name counts on its own: the second [[a]] is a[1], not a[2]. */
    char section[TOML_SECTION_MAX];
    long number = 0;
    ASSERT_TRUE(toml_table_array_section("a", 1, section, sizeof section));
    EXPECT_TRUE(toml_get_int(doc, section, "x", &number));
    EXPECT_EQ(3, (int)number);

    toml_free(doc);
}

MOLTEST(toml_reports_a_malformed_array_of_tables) {
    char err[256] = "";
    EXPECT_NULL(toml_parse("[[tool]\nkind = \"linter\"\n", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "Project.toml:1"));

    err[0] = '\0';
    EXPECT_NULL(toml_parse("[[]]\n", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "empty"));
}

MOLTEST(toml_reads_an_array_written_across_lines) {
    char err[256] = "";
    toml_document *doc = toml_parse("provides = [\n"
                                    "    \"constexpr\",\n"
                                    "    \"concepts\",\n"
                                    "]\n"
                                    "name = \"clang\"\n",
                                    err, sizeof err);
    ASSERT_NOT_NULL(doc);

    str_list values;
    str_list_init(&values);
    ASSERT_TRUE(toml_get_array(doc, "", "provides", &values));
    EXPECT_EQ(2, (int)str_list_count(&values));
    EXPECT_STREQ("constexpr", str_list_get(&values, 0));
    EXPECT_STREQ("concepts", str_list_get(&values, 1));
    str_list_free(&values);

    /* What follows the array is still read as a key, not as a leftover. */
    char name[32] = "";
    EXPECT_TRUE(toml_get_string(doc, "", "name", name, sizeof name));
    EXPECT_STREQ("clang", name);

    toml_free(doc);
}

MOLTEST(toml_keeps_a_section_after_a_multiline_array) {
    char err[256] = "";
    toml_document *doc = toml_parse("std = [\n  \"c17\"\n]\n[toolchain]\nvendor = \"clang\"\n",
                                    err, sizeof err);
    ASSERT_NOT_NULL(doc);

    char vendor[32] = "";
    EXPECT_TRUE(toml_get_string(doc, "toolchain", "vendor", vendor, sizeof vendor));
    EXPECT_STREQ("clang", vendor);

    toml_free(doc);
}

MOLTEST(toml_does_not_gather_lines_for_a_bracket_inside_a_string) {
    /* A '[' in a string opens nothing: the next line is its own key. */
    char err[256] = "";
    toml_document *doc = toml_parse("description = \"a [ thing\"\nname = \"clang\"\n",
                                    err, sizeof err);
    ASSERT_NOT_NULL(doc);

    char name[32] = "";
    EXPECT_TRUE(toml_get_string(doc, "", "name", name, sizeof name));
    EXPECT_STREQ("clang", name);

    toml_free(doc);
}

MOLTEST(toml_reports_an_array_that_is_never_closed) {
    char err[256] = "";
    EXPECT_NULL(toml_parse("provides = [\n  \"constexpr\",\n", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "unterminated array"));
}
