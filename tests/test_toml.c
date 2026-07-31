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

MOLTEST(toml_skips_inline_tables) {
    char err[256] = "";
    toml_document *doc = toml_parse(
        "[package]\nname = \"x\"\n[deps]\nhttp = { path = \"m\" }\n", err, sizeof err);
    ASSERT_NOT_NULL(doc);

    /* The inline table is ignored, and the rest of the document still parses. */
    char buffer[64];
    EXPECT_TRUE(toml_get_string(doc, "package", "name", buffer, sizeof buffer));
    EXPECT_STREQ("x", buffer);
    EXPECT_FALSE(toml_get_string(doc, "deps", "http", buffer, sizeof buffer));
    toml_free(doc);
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
