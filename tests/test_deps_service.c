#include <moltest.h>

#include <molto/project/project_ctx.h>
#include <molto/services/deps_service.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Preparing dependencies, without a network.
 *
 * A `path` dependency reaches every part of the machinery a registry one does
 * — the recipe it carries, the sources it names, the flags it exports — and is
 * the one source with nothing to fetch. What is not exercised here is
 * resolution itself, which test_resolve_service.c covers against canned
 * bodies. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];
} sandbox;

static bool sandbox_open(sandbox *at) {
    snprintf(at->root, sizeof at->root, "%s", "/tmp/molto_deps_XXXXXX");
    return mkdtemp(at->root) != NULL;
}

static void sandbox_close(const sandbox *at) { (void)fs_remove_tree(at->root); }

/* A dependency on disk: a recipe, a source it names, and one it does not. */
static bool make_dependency(const sandbox *at, const char *recipe) {
    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    if (!fs_format_path(dir, sizeof dir, "%s/yyjson", at->root) || !fs_make_dirs(dir))
        return false;

    if (!fs_format_path(file, sizeof file, "%s/recipe.toml", dir) ||
        !fs_write_file(file, recipe))
        return false;
    if (!fs_format_path(file, sizeof file, "%s/yyjson.c", dir) ||
        !fs_write_file(file, "int yyjson_answer(void) { return 1; }\n"))
        return false;
    /* The shape the `sources` list exists to keep out: another main(). */
    if (!fs_format_path(file, sizeof file, "%s/tool.c", dir) ||
        !fs_write_file(file, "int main(void) { return 0; }\n"))
        return false;
    return true;
}

/* A manifest whose only dependency is that directory. */
static bool parse_with_dep(const sandbox *at, project_ctx *out, char *err, size_t err_size) {
    char manifest[PATH_MAX_LEN * 2];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[deps]\nyyjson = { path = \"%s/yyjson\" }\n",
             at->root);
    return project_parse(manifest, out, err, err_size);
}

static const char *const RECIPE = "schema = 1\n"
                                  "form = \"source\"\n"
                                  "kind = \"package\"\n"
                                  "name = \"yyjson\"\n"
                                  "version = \"0.10.0\"\n"
                                  "target = \"any\"\n"
                                  "\n"
                                  "[artifacts]\n"
                                  "type = \"source\"\n"
                                  "sources = [\"yyjson.c\"]\n"
                                  "include = [\".\"]\n"
                                  "link = [\"m\"]\n"
                                  "defines = [\"YYJSON_STATIC=1\"]\n";

MOLTEST(deps_prepare_reduces_a_dependency_to_what_a_build_needs) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, RECIPE));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));

    /* Only what the recipe named: tool.c brings its own main() and would fail
       the link. */
    ASSERT_EQ(1u, deps.sources.count);
    EXPECT_NOT_NULL(strstr(deps.sources.items[0], "yyjson.c"));

    /* Absolute, because the source is not under the project root. */
    ASSERT_EQ(1u, deps.includes.count);
    EXPECT_EQ('/', deps.includes.items[0][0]);

    ASSERT_EQ(1u, deps.defines.count);
    EXPECT_STREQ("YYJSON_STATIC=1", deps.defines.items[0]);
    ASSERT_EQ(1u, deps.links.count);
    EXPECT_STREQ("m", deps.links.items[0]);

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

MOLTEST(deps_prepare_takes_every_source_when_the_recipe_names_none) {
    static const char *const everything = "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                          "name = \"yyjson\"\nversion = \"0.10.0\"\n"
                                          "target = \"any\"\n"
                                          "\n[artifacts]\ntype = \"source\"\n"
                                          "exclude = [\"tool.c\"]\n";
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, everything));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));

    /* Discovered, then filtered: exclude is what keeps the second main() out. */
    ASSERT_EQ(1u, deps.sources.count);
    EXPECT_NOT_NULL(strstr(deps.sources.items[0], "yyjson.c"));

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

MOLTEST(deps_prepare_reports_a_source_that_brings_no_recipe) {
    /* [deps] can say where the bytes are but not what to compile out of them,
       so the source has to say it itself. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    char dir[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(dir, sizeof dir, "%s/yyjson", at.root));
    ASSERT_TRUE(fs_make_dirs(dir));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    EXPECT_FALSE(deps_prepare(&ctx, &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "recipe.toml"));

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

MOLTEST(deps_prepare_reports_a_source_the_recipe_names_but_does_not_contain) {
    static const char *const missing = "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                       "name = \"yyjson\"\nversion = \"0.10.0\"\n"
                                       "target = \"any\"\n"
                                       "\n[artifacts]\ntype = \"source\"\n"
                                       "sources = [\"nowhere.c\"]\n";
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, missing));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    EXPECT_FALSE(deps_prepare(&ctx, &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "nowhere.c"));

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

MOLTEST(deps_prepare_refuses_an_artifact_it_cannot_consume) {
    /* A prebuilt library needs ar, -fPIC and a link step molto does not have. */
    static const char *const prebuilt = "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                        "name = \"yyjson\"\nversion = \"0.10.0\"\n"
                                        "target = \"any\"\n"
                                        "\n[artifacts]\ntype = \"static\"\n";
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, prebuilt));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    EXPECT_FALSE(deps_prepare(&ctx, &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "source"));

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

MOLTEST(deps_prepare_does_nothing_and_touches_nothing_without_deps) {
    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n", &ctx, err,
                              sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));
    EXPECT_EQ(0u, deps.sources.count);
    EXPECT_EQ(0u, deps.includes.count);
    prepared_deps_free(&deps);
}

MOLTEST(deps_prepare_names_the_dependency_that_failed) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    EXPECT_FALSE(deps_prepare(&ctx, &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "yyjson"));

    prepared_deps_free(&deps);
    sandbox_close(&at);
}
