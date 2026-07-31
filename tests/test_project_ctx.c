#include <moltest.h>

#include <molto/project/project_ctx.h>

#include <string.h>

/* A manifest exercising the package and profile tables. */
static const char *full_manifest(void) {
    return
        "[package]\n"
        "name = \"demo_app\"      # the package\n"
        "version = \"1.2.3\"\n"
        "artifact = \"shared\"\n"
        "\n"
        "[profile.release]\n"
        "opt_level = 2\n"
        "debug_info = true\n";
}

MOLTEST(project_parses_package_fields) {
    char err[256] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse(full_manifest(), &ctx, err, sizeof err));

    EXPECT_STREQ("demo_app", ctx.project_name);
    EXPECT_STREQ("1.2.3", ctx.version);
    EXPECT_EQ(artifact_shared, ctx.artifact);
}

MOLTEST(project_applies_declared_profile) {
    char err[256] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse(full_manifest(), &ctx, err, sizeof err));

    /* The declared profile overrides the built-in defaults. */
    EXPECT_EQ(2, ctx.profile.release.opt_level);
    EXPECT_TRUE(ctx.profile.release.debug_info);
}

MOLTEST(project_keeps_builtin_profile_defaults) {
    char err[256] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse(full_manifest(), &ctx, err, sizeof err));

    /* Profiles that were not declared keep their built-in values. */
    EXPECT_EQ(0, ctx.profile.debug.opt_level);
    EXPECT_TRUE(ctx.profile.debug.debug_info);
    EXPECT_EQ(3, ctx.profile.bench.opt_level);
    EXPECT_FALSE(ctx.profile.bench.debug_info);
    EXPECT_EQ(2, ctx.profile.custom.opt_level);
    EXPECT_TRUE(ctx.profile.custom.debug_info);
}

MOLTEST(project_defaults_optional_fields) {
    char err[256] = "";
    project_ctx minimal;
    ASSERT_TRUE(project_parse("[package]\nname = \"tiny\"\n", &minimal, err, sizeof err));

    EXPECT_STREQ("0.0.0", minimal.version);
    EXPECT_EQ(artifact_static, minimal.artifact);
    /* No [target] declared: autodetect (empty) and no link libraries. */
    EXPECT_STREQ("", minimal.target.compiler);
    EXPECT_STREQ("", minimal.target.std);
    EXPECT_EQ(0, minimal.target.link_count);
}

MOLTEST(project_rejects_invalid_manifests) {
    char err[256] = "";
    project_ctx ctx;

    /* A missing or non-snake_case package name is an error. */
    EXPECT_FALSE(project_parse("[package]\nversion = \"1.0.0\"\n", &ctx, err, sizeof err));
    EXPECT_FALSE(project_parse("[package]\nname = \"Bad_Name\"\n", &ctx, err, sizeof err));

    /* Unknown artifact kind and unknown compiler are errors. */
    EXPECT_FALSE(project_parse("[package]\nname = \"x\"\nartifact = \"weird\"\n",
                               &ctx, err, sizeof err));
    EXPECT_FALSE(project_parse("[package]\nname = \"x\"\n[target]\ncompiler = \"turbo\"\n",
                               &ctx, err, sizeof err));

    /* A malformed manifest surfaces the parser's line-tagged error. */
    err[0] = '\0';
    EXPECT_FALSE(project_parse("[package\nname = \"x\"\n", &ctx, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "Project.toml:1"));
}

MOLTEST(project_reads_target_table) {
    char err[256] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse(
        "[package]\nname = \"app\"\n"
        "[target]\n"
        "compiler = \"clang\"\n"
        "std = \"c23\"\n"
        "cpp_std = \"c++20\"\n"
        "link = [\"m\", \"pthread\"]\n",
        &ctx, err, sizeof err));

    EXPECT_STREQ("clang", ctx.target.compiler);
    EXPECT_STREQ("c23", ctx.target.std);
    EXPECT_STREQ("c++20", ctx.target.cpp_std);
    EXPECT_EQ(2, ctx.target.link_count);
    EXPECT_STREQ("m", ctx.target.link[0]);
    EXPECT_STREQ("pthread", ctx.target.link[1]);
}

MOLTEST(project_reads_options_base_and_per_profile) {
    char err[256] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse(
        "[package]\nname = \"app\"\n"
        "[target]\ndefines = [\"BASE=1\"]\ninclude = [\"vendor\"]\nflags = [\"-Wall\"]\n"
        "[profile.release]\nflags = [\"-flto\"]\n",
        &ctx, err, sizeof err));

    /* Base options from [target] apply to every profile. */
    EXPECT_EQ(1, ctx.target.options.define_count);
    EXPECT_STREQ("BASE=1", ctx.target.options.defines[0]);
    EXPECT_EQ(1, ctx.target.options.include_count);
    EXPECT_STREQ("vendor", ctx.target.options.include[0]);
    EXPECT_EQ(1, ctx.target.options.flag_count);
    EXPECT_STREQ("-Wall", ctx.target.options.flags[0]);

    /* Per-profile options are added on top, only for that profile. */
    EXPECT_EQ(1, ctx.profile_options.release.flag_count);
    EXPECT_STREQ("-flto", ctx.profile_options.release.flags[0]);
    EXPECT_EQ(0, ctx.profile_options.debug.flag_count);
}

/* Build a manifest whose [target].defines array holds `count` entries. */
static void manifest_with_defines(char *out, size_t out_size, size_t count) {
    int pos = snprintf(out, out_size, "[package]\nname = \"app\"\n[target]\ndefines = [");
    for (size_t i = 0; i < count; i++)
        pos += snprintf(out + pos, out_size - (size_t)pos, "%s\"D%zu=1\"",
                        i > 0 ? ", " : "", i);
    snprintf(out + pos, out_size - (size_t)pos, "]\n");
}

MOLTEST(project_rejects_more_options_than_it_can_hold) {
    char err[256] = "";
    char manifest[2048];
    project_ctx ctx;

    /* Exactly at capacity is fine. */
    manifest_with_defines(manifest, sizeof manifest, PROJECT_MAX_OPTS);
    EXPECT_TRUE(project_parse(manifest, &ctx, err, sizeof err));
    EXPECT_EQ(PROJECT_MAX_OPTS, ctx.target.options.define_count);

    /* One more used to be dropped in silence, producing a green build with
       fewer defines than the manifest asked for. */
    manifest_with_defines(manifest, sizeof manifest, PROJECT_MAX_OPTS + 1);
    err[0] = '\0';
    EXPECT_FALSE(project_parse(manifest, &ctx, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[target].defines"));
}

MOLTEST(project_rejects_an_overlong_option) {
    char err[256] = "";
    char manifest[512];
    char value[PROJECT_OPT_LEN + 8];
    memset(value, 'x', sizeof value - 1);
    value[sizeof value - 1] = '\0';
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\n[target]\nflags = [\"%s\"]\n", value);

    project_ctx ctx;
    EXPECT_FALSE(project_parse(manifest, &ctx, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[target].flags"));
}

MOLTEST(project_rejects_more_link_libraries_than_it_can_hold) {
    char err[256] = "";
    char manifest[2048];
    int pos = snprintf(manifest, sizeof manifest,
                       "[package]\nname = \"app\"\n[target]\nlink = [");
    for (size_t i = 0; i < PROJECT_MAX_LINK + 1; i++)
        pos += snprintf(manifest + pos, sizeof manifest - (size_t)pos, "%s\"lib%zu\"",
                        i > 0 ? ", " : "", i);
    snprintf(manifest + pos, sizeof manifest - (size_t)pos, "]\n");

    project_ctx ctx;
    EXPECT_FALSE(project_parse(manifest, &ctx, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[target].link"));
}
