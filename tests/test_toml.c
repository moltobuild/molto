#include "test_framework.h"
#include "tests.h"

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

void suite_toml(void) {
    const char *text =
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

    char err[256] = "";
    toml_document *doc = toml_parse(text, err, sizeof err);
    CHECK(doc != NULL);
    if (doc == NULL)
        return;

    char buffer[64];
    CHECK(toml_get_string(doc, "package", "name", buffer, sizeof buffer));
    CHECK(strcmp(buffer, "molto") == 0);                 /* inline comment stripped */
    CHECK(toml_get_string(doc, "package", "version", buffer, sizeof buffer));
    CHECK(strcmp(buffer, "literal string") == 0);        /* literal string */
    CHECK(toml_get_string(doc, "settings", "note", buffer, sizeof buffer));
    CHECK(strcmp(buffer, "tab\there") == 0);             /* escape decoded */

    long integer = 0;
    CHECK(toml_get_int(doc, "settings", "width", &integer));
    CHECK(integer == 1000);                              /* '_' separator */
    CHECK(toml_get_int(doc, "settings", "offset", &integer));
    CHECK(integer == -42);                               /* signed */

    bool flag = false;
    CHECK(toml_get_bool(doc, "settings", "enabled", &flag) && flag == true);
    CHECK(toml_get_bool(doc, "settings", "disabled", &flag) && flag == false);

    /* Type mismatch and missing keys. */
    CHECK(!toml_get_int(doc, "package", "name", &integer));   /* it is a string */
    CHECK(!toml_get_string(doc, "package", "missing", buffer, sizeof buffer));
    CHECK(toml_has_section(doc, "settings"));
    CHECK(!toml_has_section(doc, "nope"));

    /* Schema binding, including a nested member (offsetof supports it). */
    bound_config config;
    memset(&config, 0, sizeof config);
    strcpy(config.package.name, "default");
    const toml_field schema[] = {
        TOML_STR(bound_config, "package", "name", package.name),
        TOML_INT(bound_config, "settings", "width", width),
        TOML_BOOL(bound_config, "settings", "enabled", enabled),
    };
    CHECK(toml_bind(doc, schema, sizeof schema / sizeof schema[0], &config, err, sizeof err));
    CHECK(strcmp(config.package.name, "molto") == 0);
    CHECK(config.width == 1000);
    CHECK(config.enabled == true);

    /* A field whose key is absent keeps the seeded default. */
    bound_config defaulted;
    memset(&defaulted, 0, sizeof defaulted);
    strcpy(defaulted.package.name, "kept");
    const toml_field absent[] = {
        TOML_STR(bound_config, "package", "nonexistent", package.name),
    };
    CHECK(toml_bind(doc, absent, 1, &defaulted, err, sizeof err));
    CHECK(strcmp(defaulted.package.name, "kept") == 0);

    /* Type mismatch in the schema fails. */
    const toml_field mismatch[] = {
        TOML_INT(bound_config, "package", "name", width), /* name is a string */
    };
    CHECK(!toml_bind(doc, mismatch, 1, &defaulted, err, sizeof err));

    /* String arrays are parsed and read back with toml_get_array. */
    toml_document *arr_doc = toml_parse(
        "[target]\nlink = [\"m\", \"pthread\"]\nempty = []\n", err, sizeof err);
    CHECK(arr_doc != NULL);
    if (arr_doc != NULL) {
        str_list libs;
        str_list_init(&libs);
        CHECK(toml_get_array(arr_doc, "target", "link", &libs));
        CHECK(str_list_count(&libs) == 2);
        CHECK(strcmp(str_list_get(&libs, 0), "m") == 0);
        CHECK(strcmp(str_list_get(&libs, 1), "pthread") == 0);
        str_list_free(&libs);

        /* Empty array is valid. */
        str_list none_arr;
        str_list_init(&none_arr);
        CHECK(toml_get_array(arr_doc, "target", "empty", &none_arr));
        CHECK(str_list_count(&none_arr) == 0);
        str_list_free(&none_arr);

        /* An array key is not readable as a scalar string. */
        CHECK(!toml_get_string(arr_doc, "target", "link", buffer, sizeof buffer));
        toml_free(arr_doc);
    }

    /* An unterminated array is a parse error. */
    CHECK(toml_parse("[t]\nx = [\"a\", \"b\"\n", err, sizeof err) == NULL);

    /* [deps] with an inline table is skipped, not an error. */
    toml_document *with_deps = toml_parse(
        "[package]\nname = \"x\"\n[deps]\nhttp = { path = \"m\" }\n", err, sizeof err);
    CHECK(with_deps != NULL);
    if (with_deps != NULL) {
        CHECK(toml_get_string(with_deps, "package", "name", buffer, sizeof buffer));
        CHECK(!toml_get_string(with_deps, "deps", "http", buffer, sizeof buffer));
        toml_free(with_deps);
    }

    toml_free(doc);

    /* Malformed inputs fail closed with a line-tagged message. */
    err[0] = '\0';
    CHECK(toml_parse("[package\nname = \"x\"\n", err, sizeof err) == NULL);
    CHECK(strstr(err, "Project.toml:1") != NULL);

    err[0] = '\0';
    CHECK(toml_parse("[package]\nname \"x\"\n", err, sizeof err) == NULL);
    CHECK(strstr(err, "Project.toml:2") != NULL);

    err[0] = '\0';
    CHECK(toml_parse("[package]\nname = \"unterminated\n", err, sizeof err) == NULL);

    err[0] = '\0';
    CHECK(toml_parse("[package]\nn = 99999999999999999999999999\n", err, sizeof err) == NULL);

    /* Empty / comment-only input is valid and yields no entries. */
    err[0] = '\0';
    toml_document *empty = toml_parse("# just a comment\n\n", err, sizeof err);
    CHECK(empty != NULL);
    if (empty != NULL) {
        CHECK(!toml_has_section(empty, "package"));
        toml_free(empty);
    }
}
