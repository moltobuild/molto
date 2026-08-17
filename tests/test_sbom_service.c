#include <moltest.h>

#include <molto/project/project_ctx.h>
#include <molto/services/dep_graph.h>
#include <molto/services/fs_service.h>
#include <molto/services/sbom_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reducing a resolved graph to a bill of materials, without a formatter in the
 * way.
 *
 * Path dependencies throughout, so the whole thing runs without a network: a
 * package on disk carries its own recipe, and that recipe is where the licence
 * comes from. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];
} sandbox;

static bool sandbox_open(sandbox *at) {
    snprintf(at->root, sizeof at->root, "%s", "/tmp/molto_sbom_XXXXXX");
    return mkdtemp(at->root) != NULL;
}

static void sandbox_close(const sandbox *at) { (void)fs_remove_tree(at->root); }

/* One package on disk. `tail` is appended to its recipe verbatim, which is how
   a test gives it an [about] table or dependencies of its own. */
static bool make_package(const sandbox *at, const char *name, const char *tail) {
    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    if (!fs_format_path(dir, sizeof dir, "%s/%s", at->root, name) || !fs_make_dirs(dir))
        return false;

    char recipe[PATH_MAX_LEN * 4];
    snprintf(recipe, sizeof recipe,
             "schema = 1\nform = \"source\"\nkind = \"package\"\n"
             "name = \"%s\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
             "[artifacts]\ntype = \"source\"\nsources = [\"%s.c\"]\ninclude = [\".\"]\n%s",
             name, name, tail == NULL ? "" : tail);
    if (!fs_format_path(file, sizeof file, "%s/recipe.toml", dir) || !fs_write_file(file, recipe))
        return false;

    if (!fs_format_path(file, sizeof file, "%s/%s.c", dir, name))
        return false;
    return fs_write_file(file, "int answer(void) { return 1; }\n");
}

/* A manifest naming `deps` under [deps] and `dev` under [dev-deps], each by
   path. Either may be NULL. */
static bool parse_root(const sandbox *at, const char *deps, const char *dev, project_ctx *out,
                       char *err, size_t err_size) {
    char manifest[PATH_MAX_LEN * 4];
    int used = snprintf(manifest, sizeof manifest, "[package]\nname = \"app\"\n"
                                                   "version = \"1.2.3\"\n"
                                                   "description = \"A demo\"\n"
                                                   "license = \"MIT\"\n");
    if (deps != NULL)
        used += snprintf(manifest + used, sizeof manifest - (size_t)used,
                         "[deps]\n%s = { path = \"%s/%s\" }\n", deps, at->root, deps);
    if (dev != NULL)
        used += snprintf(manifest + used, sizeof manifest - (size_t)used,
                         "[dev-deps]\n%s = { path = \"%s/%s\" }\n", dev, at->root, dev);
    return project_parse(manifest, out, err, err_size);
}

static const sbom_component *component_named(const sbom_document *document, const char *name) {
    for (size_t i = 0; i < document->count; i++) {
        if (strcmp(document->components[i].name, name) == 0)
            return &document->components[i];
    }
    return NULL;
}

MOLTEST(the_document_describes_the_package_and_what_it_links) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    EXPECT_TRUE(make_package(&at, "png",
                             "[about]\n"
                             "description = \"The PNG reference library\"\n"
                             "license = \"libpng-2.0\"\n"
                             "homepage = \"http://www.libpng.org\"\n"));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_root(&at, "png", NULL, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    sbom_document document;
    const sbom_options options = {.include_dev = false};
    ASSERT_TRUE(sbom_collect(&ctx, graph, &options, &document));

    /* The package itself, from [package] and the metadata it declares. */
    EXPECT_STREQ("app", document.name);
    EXPECT_STREQ("1.2.3", document.version);
    EXPECT_STREQ("MIT", document.about->license);
    ASSERT_EQ(1u, str_list_count(&document.root_dependencies));
    EXPECT_STREQ("png", str_list_get(&document.root_dependencies, 0));

    /* And what it links, licence included. */
    ASSERT_EQ(1u, document.count);
    const sbom_component *png = &document.components[0];
    EXPECT_STREQ("png", png->name);
    EXPECT_STREQ("libpng-2.0", png->license);
    EXPECT_STREQ("The PNG reference library", png->description);
    EXPECT_STREQ("http://www.libpng.org", png->homepage);
    EXPECT_TRUE(png->ships);

    sbom_document_free(&document);
    dep_graph_free(graph);
    sandbox_close(&at);
}

MOLTEST(a_path_dependency_is_reported_as_unverified) {
    /* Its bytes are whatever is on disk, so there is no checksum to state. The
       component still belongs in the document — leaving it out would be a
       document that quietly describes less than the build contains — but it
       has to be distinguishable from one that was verified. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    EXPECT_TRUE(make_package(&at, "http", NULL));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_root(&at, "http", NULL, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    sbom_document document;
    const sbom_options options = {.include_dev = false};
    ASSERT_TRUE(sbom_collect(&ctx, graph, &options, &document));

    ASSERT_EQ(1u, document.count);
    EXPECT_FALSE(document.components[0].verified);
    EXPECT_STREQ("", document.components[0].checksum);
    EXPECT_EQ(1u, sbom_unverified_count(&document));

    /* A recipe with no [about] says nothing, and that is not a licence. */
    EXPECT_STREQ("", document.components[0].license);

    sbom_document_free(&document);
    dep_graph_free(graph);
    sandbox_close(&at);
}

MOLTEST(a_development_dependency_stays_out_unless_it_is_asked_for) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    EXPECT_TRUE(make_package(&at, "png", "[about]\nlicense = \"libpng-2.0\"\n"));
    EXPECT_TRUE(make_package(&at, "moltest", "[about]\nlicense = \"MIT\"\n"));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_root(&at, "png", "moltest", &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    /* A bill of materials describes what ships. A test framework does not. */
    sbom_document shipped;
    const sbom_options without_dev = {.include_dev = false};
    ASSERT_TRUE(sbom_collect(&ctx, graph, &without_dev, &shipped));
    ASSERT_EQ(1u, shipped.count);
    EXPECT_STREQ("png", shipped.components[0].name);
    EXPECT_NULL(component_named(&shipped, "moltest"));
    sbom_document_free(&shipped);

    /* Asked for, it appears, and it appears marked as not shipping. */
    sbom_document everything;
    const sbom_options with_dev = {.include_dev = true};
    ASSERT_TRUE(sbom_collect(&ctx, graph, &with_dev, &everything));
    ASSERT_EQ(2u, everything.count);
    const sbom_component *moltest = component_named(&everything, "moltest");
    ASSERT_NOT_NULL(moltest);
    EXPECT_FALSE(moltest->ships);
    EXPECT_TRUE(component_named(&everything, "png")->ships);
    sbom_document_free(&everything);

    dep_graph_free(graph);
    sandbox_close(&at);
}

MOLTEST(a_package_in_both_tables_ships) {
    /* One name is one node carrying both scopes (RFC-0008), so the test that
       decides whether it belongs in the document is on the bit and not on
       equality. Getting that wrong drops a library that is genuinely linked
       into the binary, which is the worst thing this document can do. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    EXPECT_TRUE(make_package(&at, "png", "[about]\nlicense = \"libpng-2.0\"\n"));

    char manifest[PATH_MAX_LEN * 4];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
             "[deps]\npng = { path = \"%s/png\" }\n"
             "[dev-deps]\npng = { path = \"%s/png\" }\n",
             at.root, at.root);

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse(manifest, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    sbom_document document;
    const sbom_options options = {.include_dev = false};
    ASSERT_TRUE(sbom_collect(&ctx, graph, &options, &document));

    ASSERT_EQ(1u, document.count);
    EXPECT_STREQ("png", document.components[0].name);
    EXPECT_TRUE(document.components[0].ships);

    sbom_document_free(&document);
    dep_graph_free(graph);
    sandbox_close(&at);
}

MOLTEST(a_package_with_no_dependencies_is_an_empty_document) {
    /* The ordinary case for a small project, and the one an emitter is most
       likely to turn into a broken array. */
    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n", &ctx, err,
                              sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    sbom_document document;
    const sbom_options options = {.include_dev = false};
    ASSERT_TRUE(sbom_collect(&ctx, graph, &options, &document));

    EXPECT_EQ(0u, document.count);
    EXPECT_EQ(0u, str_list_count(&document.root_dependencies));
    EXPECT_EQ(0u, sbom_unverified_count(&document));
    EXPECT_STREQ("app", document.name);

    sbom_document_free(&document);
    dep_graph_free(graph);
}

MOLTEST(components_come_out_sorted_by_name) {
    /* The graph is sorted already; what this pins is that collecting does not
       lose the order. A document whose diff reorders itself between runs is
       one nobody reads, which is the same reason Molto.lock is sorted. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    EXPECT_TRUE(make_package(&at, "zlib", NULL));
    EXPECT_TRUE(make_package(&at, "aa", NULL));

    char manifest[PATH_MAX_LEN * 4];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
             "[deps]\nzlib = { path = \"%s/zlib\" }\naa = { path = \"%s/aa\" }\n",
             at.root, at.root);

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse(manifest, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    sbom_document document;
    const sbom_options options = {.include_dev = false};
    ASSERT_TRUE(sbom_collect(&ctx, graph, &options, &document));

    ASSERT_EQ(2u, document.count);
    EXPECT_STREQ("aa", document.components[0].name);
    EXPECT_STREQ("zlib", document.components[1].name);
    /* And the root's own edges are sorted too. */
    ASSERT_EQ(2u, str_list_count(&document.root_dependencies));
    EXPECT_STREQ("aa", str_list_get(&document.root_dependencies, 0));

    sbom_document_free(&document);
    dep_graph_free(graph);
    sandbox_close(&at);
}
