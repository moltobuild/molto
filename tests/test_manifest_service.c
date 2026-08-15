#include <moltest.h>

#include <molto/project/project_ctx.h>
#include <molto/services/manifest_service.h>
#include <molto/util/doc.h>
#include <molto/util/json.h>
#include <molto/util/toml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MOLTEST(manifest_service) {
    /* Valid snake_case names. */
    EXPECT_TRUE(manifest_is_valid_name("my_app"));
    EXPECT_TRUE(manifest_is_valid_name("http2"));
    EXPECT_TRUE(manifest_is_valid_name("a"));

    /* Invalid names. */
    EXPECT_TRUE(!manifest_is_valid_name(NULL));
    EXPECT_TRUE(!manifest_is_valid_name(""));
    EXPECT_TRUE(!manifest_is_valid_name("My_App")); /* uppercase */
    EXPECT_TRUE(!manifest_is_valid_name("1abc"));   /* leading digit */
    EXPECT_TRUE(!manifest_is_valid_name("a-b"));    /* hyphen */
    EXPECT_TRUE(!manifest_is_valid_name("a/b"));    /* slash */

    /* Rendering a valid manifest. */
    char *toml = manifest_render_default("my_app");
    EXPECT_TRUE(toml != NULL);
    if (toml != NULL) {
        EXPECT_TRUE(strstr(toml, "name = \"my_app\"") != NULL);
        EXPECT_TRUE(strstr(toml, "[package]") != NULL);
        EXPECT_TRUE(strstr(toml, "[profile.release]") != NULL);
        free(toml);
    }

    /* Invalid name yields no manifest. */
    EXPECT_TRUE(manifest_render_default("Bad Name") == NULL);
}

MOLTEST(manifest_accepts_an_exact_version) {
    char operator_found[8] = "";
    EXPECT_TRUE(manifest_is_exact_version("3.53.4", operator_found, sizeof operator_found));
    EXPECT_TRUE(manifest_is_exact_version("0.1.0", operator_found, sizeof operator_found));
    EXPECT_TRUE(manifest_is_exact_version("1.0.0-rc.1", operator_found, sizeof operator_found));
    EXPECT_TRUE(manifest_is_exact_version("1.0.0+build.5", operator_found, sizeof operator_found));
    EXPECT_STREQ("", operator_found);
}

MOLTEST(manifest_rejects_a_version_range) {
    /* RFC-0008: a range is a standing authorisation to run code that does not
       exist yet. The operator is reported so the message can name it. */
    static const struct {
        const char *version;
        const char *expected;
    } ranges[] = {
        { "^3.5.0", "^" },   { "~3.5.0", "~" },  { ">=1.0.0", ">=" },
        { "<=2.0.0", "<=" }, { ">1.0.0", ">" },  { "<2.0.0", "<" },
        { "*", "*" },        { "1.0.0, <2.0.0", "," },
    };

    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
        char operator_found[8] = "";
        EXPECT_FALSE(manifest_is_exact_version(ranges[i].version, operator_found,
                                               sizeof operator_found));
        EXPECT_STREQ(ranges[i].expected, operator_found);
    }
}

MOLTEST(manifest_rejects_a_version_that_is_not_one) {
    /* Semver validates as well as orders, so a typo is caught here rather than
       compared byte by byte against whatever a registry serves. */
    static const char *const bad[] = { "", "3.5", "3", "latest", "v3.5.0", "3.5.x", "a.b.c",
                                       "3.5.0-" };

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        EXPECT_FALSE(manifest_is_exact_version(bad[i], NULL, 0));

    EXPECT_FALSE(manifest_is_exact_version(NULL, NULL, 0));
}

MOLTEST(manifest_declares_a_language_standard) {
    char *toml = manifest_render_default("my_app");
    ASSERT_NOT_NULL(toml);

    /* Left to the compiler, the standard varies by toolchain and version, so a
       generated project pins its own instead of inheriting the machine's. */
    EXPECT_NOT_NULL(strstr(toml, "[target]"));
    EXPECT_NOT_NULL(strstr(toml, "std = \"c17\""));

    /* The other keys ship commented out: documentation, not settings. */
    EXPECT_NOT_NULL(strstr(toml, "# compiler = "));
    EXPECT_NOT_NULL(strstr(toml, "# link = "));

    /* Whatever the template says, it has to be a manifest Molto can read, and
       the commented keys must stay inert. */
    char err[256] = "";
    project_ctx ctx;
    EXPECT_TRUE(project_parse(toml, &ctx, err, sizeof err));
    EXPECT_STREQ("c17", ctx.target.std);
    EXPECT_STREQ("", ctx.target.compiler);
    EXPECT_EQ(0, ctx.target.link_count);
    EXPECT_EQ(0, ctx.target.options.flag_count);

    free(toml);
}

MOLTEST(manifest_declares_the_project_include_directory) {
    char *toml = manifest_render_default("my_app");
    ASSERT_NOT_NULL(toml);

    /* `include` is the one [target] key that ships active. A header under
       include/ is the normal layout for a C project, and a commented-out key
       teaches nothing: the build fails on the first #include and the reason is
       a line the user never read. `molto new` creates the directory it points
       at, so the manifest never names something that is not there. */
    EXPECT_NOT_NULL(strstr(toml, "include = [\"include\"]"));
    EXPECT_NULL(strstr(toml, "# include = "));

    char err[256] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse(toml, &ctx, err, sizeof err));
    ASSERT_EQ(1, ctx.target.options.include_count);
    EXPECT_STREQ("include", ctx.target.options.include[0]);

    free(toml);
}

/* --- the publishing metadata (RFC-0003 [package], RFC-0009 [about]) --- */

static bool read_about_toml(const char *text, const char *table, manifest_about *out, char *err,
                            size_t err_size) {
    toml_document *doc = toml_parse(text, err, err_size);
    if (doc == NULL)
        return false;
    bool ok = manifest_read_about(doc_from_toml(doc), table, out, err, err_size);
    toml_free(doc);
    return ok;
}

MOLTEST(manifest_reads_about_from_either_table) {
    /* One reader, two names. A manifest writes this under [package] and a
       recipe under [about] (RFC-0009), and the two must agree about what the
       keys mean — which they cannot do if each has its own reader. */
    static const char *const TABLES[] = { "package", "about" };

    for (size_t i = 0; i < sizeof TABLES / sizeof TABLES[0]; i++) {
        char text[512];
        snprintf(text, sizeof text,
                 "[%s]\n"
                 "name = \"http\"\n"
                 "version = \"0.2.0\"\n"
                 "description = \"A small HTTP client\"\n"
                 "license = \"MIT\"\n"
                 "homepage = \"https://example.dev/http\"\n"
                 "repository = \"https://github.com/example/http\"\n"
                 "authors = [\"Ada <ada@example.dev>\", \"Grace\"]\n",
                 TABLES[i]);

        manifest_about about;
        char err[256] = "";
        ASSERT_TRUE(read_about_toml(text, TABLES[i], &about, err, sizeof err));
        EXPECT_STREQ("A small HTTP client", about.description);
        EXPECT_STREQ("MIT", about.license);
        EXPECT_STREQ("https://example.dev/http", about.homepage);
        EXPECT_STREQ("https://github.com/example/http", about.repository);
        ASSERT_EQ(2, about.author_count);
        EXPECT_STREQ("Ada <ada@example.dev>", about.authors[0]);
        EXPECT_STREQ("Grace", about.authors[1]);
    }
}

MOLTEST(manifest_reads_about_from_a_registry_answer) {
    /* The same recipe arrives as TOML from disk and as JSON inside a registry's
       answer (RFC-0010). doc_view is what keeps that one document rather than
       two, and this is the half that never has a local file to diff against. */
    json_document *doc = json_parse("{\"name\":\"http\",\"about\":{"
                                    "\"description\":\"A small HTTP client\","
                                    "\"license\":\"MIT\",\"authors\":[\"Ada\"]}}");
    ASSERT_NOT_NULL(doc);

    manifest_about about;
    char err[256] = "";
    ASSERT_TRUE(
        manifest_read_about(doc_from_json(json_root(doc)), "about", &about, err, sizeof err));
    EXPECT_STREQ("A small HTTP client", about.description);
    EXPECT_STREQ("MIT", about.license);
    ASSERT_EQ(1, about.author_count);
    EXPECT_STREQ("Ada", about.authors[0]);

    json_free(doc);
}

MOLTEST(manifest_about_is_entirely_optional) {
    manifest_about about;
    char err[256] = "";

    ASSERT_TRUE(read_about_toml("[package]\nname = \"http\"\n", "package", &about, err,
                                sizeof err));
    EXPECT_STREQ("", about.description);
    EXPECT_STREQ("", about.license);
    EXPECT_STREQ("", about.homepage);
    EXPECT_STREQ("", about.repository);
    EXPECT_EQ(0, about.author_count);

    /* A table that is not there at all says nothing about itself, which is
       allowed: a recipe published before these keys existed has no [about]. */
    ASSERT_TRUE(read_about_toml("[artifacts]\ntype = \"source\"\n", "about", &about, err,
                                sizeof err));
    EXPECT_STREQ("", about.license);
}

MOLTEST(manifest_about_refuses_a_value_that_does_not_fit) {
    /* Truncating would record a description nobody wrote. The manifest's rule
       everywhere else is that a limit is an error, and this is no different. */
    char long_value[MANIFEST_DESCRIPTION_MAX + 8];
    memset(long_value, 'x', sizeof long_value - 1);
    long_value[sizeof long_value - 1] = '\0';

    char text[sizeof long_value + 64];
    snprintf(text, sizeof text, "[package]\ndescription = \"%s\"\n", long_value);

    manifest_about about;
    char err[256] = "";
    EXPECT_FALSE(read_about_toml(text, "package", &about, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "description"));
}

MOLTEST(manifest_about_refuses_a_value_of_the_wrong_type) {
    /* Declared and not a string is not the same as absent: the first is a
       mistake worth naming, the second is the default. */
    manifest_about about;
    char err[256] = "";
    EXPECT_FALSE(read_about_toml("[package]\ndescription = 5\n", "package", &about, err,
                                 sizeof err));
    EXPECT_NOT_NULL(strstr(err, "description"));
}

MOLTEST(manifest_about_refuses_more_authors_than_it_holds) {
    char list[MANIFEST_MAX_AUTHORS * 8 + 16] = "";
    for (size_t i = 0; i <= MANIFEST_MAX_AUTHORS; i++)
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s\"a\"", i == 0 ? "" : ", ");

    char text[sizeof list + 64];
    snprintf(text, sizeof text, "[package]\nauthors = [%s]\n", list);

    manifest_about about;
    char err[256] = "";
    EXPECT_FALSE(read_about_toml(text, "package", &about, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "authors"));
}

MOLTEST(manifest_accepts_an_spdx_expression) {
    static const char *const good[] = {
        "MIT",
        "Apache-2.0",
        "GPL-3.0-or-later",
        "GPL-2.0+",
        "MIT OR Apache-2.0",
        "Apache-2.0 WITH LLVM-exception",
        "(MIT OR BSD-3-Clause) AND ISC",
        "blessing", /* what sqlite publishes */
    };

    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++)
        EXPECT_TRUE(manifest_is_valid_license(good[i]));
}

MOLTEST(manifest_rejects_a_license_that_is_not_an_expression) {
    /* Syntax only. The SPDX identifier list is deliberately not embedded — it
       would be a list that expires — so what is caught here is the shape: a
       dangling operator, an unbalanced paren, two identifiers with nothing
       joining them. */
    static const char *const bad[] = {
        "", "MIT OR", "OR MIT", "MIT AND AND ISC", "(MIT", "MIT)", "()", "MIT Apache-2.0",
        "MIT/Apache-2.0",
    };

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        EXPECT_FALSE(manifest_is_valid_license(bad[i]));
    EXPECT_FALSE(manifest_is_valid_license(NULL));
}

MOLTEST(manifest_about_refuses_a_malformed_license) {
    manifest_about about;
    char err[256] = "";
    EXPECT_FALSE(read_about_toml("[package]\nlicense = \"MIT OR\"\n", "package", &about, err,
                                 sizeof err));
    EXPECT_NOT_NULL(strstr(err, "license"));
}
