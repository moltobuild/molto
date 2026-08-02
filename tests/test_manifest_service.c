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
