#include <moltest.h>

#include <molto/project/project_ctx.h>
#include <molto/services/manifest_service.h>

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
