#include <moltest.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/project/lockfile.h>
#include <molto/project/project_ctx.h>
#include <molto/services/build_service.h>
#include <molto/services/dep_graph.h>
#include <molto/services/fs_service.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `[dev-deps]`: what a package needs while it is being developed and does not
 * ship (RFC-0008).
 *
 * The claim worth testing is not that the table parses. It is that a
 * development dependency cannot reach the binary — and the way that is
 * enforced is the absence of an `-I`, so the test for it is a build that fails
 * to compile. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];
} sandbox;

static bool sandbox_open(sandbox *at) {
    snprintf(at->root, sizeof at->root, "%s", "/tmp/molto_devdeps_XXXXXX");
    return mkdtemp(at->root) != NULL;
}

static void sandbox_close(const sandbox *at) { (void)fs_remove_tree(at->root); }

/* A package on disk exporting one header and one source. */
static bool make_package(const sandbox *at, const char *name, const char *deps) {
    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    if (!fs_format_path(dir, sizeof dir, "%s/%s", at->root, name) || !fs_make_dirs(dir))
        return false;

    char recipe[PATH_MAX_LEN * 4];
    snprintf(recipe, sizeof recipe,
             "schema = 1\nform = \"source\"\nkind = \"package\"\n"
             "name = \"%s\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
             "[artifacts]\ntype = \"source\"\nsources = [\"%s.c\"]\ninclude = [\".\"]\n%s",
             name, name, deps == NULL ? "" : deps);
    if (!fs_format_path(file, sizeof file, "%s/recipe.toml", dir) || !fs_write_file(file, recipe))
        return false;

    char body[256];
    snprintf(body, sizeof body, "int %s_answer(void) { return 1; }\n", name);
    if (!fs_format_path(file, sizeof file, "%s/%s.c", dir, name) || !fs_write_file(file, body))
        return false;

    snprintf(body, sizeof body, "int %s_answer(void);\n", name);
    return fs_format_path(file, sizeof file, "%s/%s.h", dir, name) && fs_write_file(file, body);
}

/* A project whose src/ and tests/ contents are given verbatim. */
static bool make_project(const sandbox *at, const char *manifest, const char *lib_body,
                         const char *test_body) {
    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    if (!fs_format_path(dir, sizeof dir, "%s/app/src", at->root) || !fs_make_dirs(dir))
        return false;
    if (!fs_format_path(file, sizeof file, "%s/lib.c", dir) || !fs_write_file(file, lib_body))
        return false;
    /* An entry point, so `molto build` has an executable to link — and one the
       test build leaves out, which is what lets both run on the same tree. */
    if (!fs_format_path(file, sizeof file, "%s/main.c", dir) ||
        !fs_write_file(file, "int main(void) { return 0; }\n"))
        return false;

    if (test_body != NULL) {
        if (!fs_format_path(dir, sizeof dir, "%s/app/tests", at->root) || !fs_make_dirs(dir))
            return false;
        if (!fs_format_path(file, sizeof file, "%s/test_it.c", dir) ||
            !fs_write_file(file, test_body))
            return false;
    }

    return fs_format_path(file, sizeof file, "%s/app/Project.toml", at->root) &&
           fs_write_file(file, manifest);
}

static void app_path(const sandbox *at, char *out, size_t size) {
    (void)fs_format_path(out, size, "%s/app", at->root);
}

/* A manifest with `helper` as a development dependency. */
static void dev_manifest(const sandbox *at, char *out, size_t size) {
    snprintf(out, size,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[target]\nstd = \"c17\"\n"
             "[dev-deps]\nhelper = { path = \"%s/helper\" }\n",
             at->root);
}

/* --- the separation --- */

/* A test may include it, and the test binary links it. */
MOLTEST(a_test_may_include_a_development_dependency) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_package(&at, "helper", NULL));

    char manifest[PATH_MAX_LEN * 2];
    dev_manifest(&at, manifest, sizeof manifest);
    ASSERT_TRUE(make_project(&at, manifest, "int lib_answer(void) { return 2; }\n",
                             "#include <helper.h>\n"
                             "int main(void) { return helper_answer() == 1 ? 0 : 1; }\n"));

    char app[PATH_MAX_LEN];
    app_path(&at, app, sizeof app);
    str_list binaries;
    str_list_init(&binaries);
    EXPECT_EQ(exit_ok, build_tests(app, profile_debug, false, &binaries, NULL));

    str_list_free(&binaries);
    sandbox_close(&at);
}

/* And src/ may not. This is the whole feature: the include path it would need
   is not on the command line that compiles the project, so the compiler says
   so on the first build. Nothing has to remember the rule. */
MOLTEST(src_may_not_include_a_development_dependency) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_package(&at, "helper", NULL));

    char manifest[PATH_MAX_LEN * 2];
    dev_manifest(&at, manifest, sizeof manifest);
    ASSERT_TRUE(make_project(&at, manifest,
                             "#include <helper.h>\n"
                             "int lib_answer(void) { return helper_answer(); }\n",
                             NULL));

    char app[PATH_MAX_LEN];
    app_path(&at, app, sizeof app);
    EXPECT_EQ(exit_build_failure, build_project(app, profile_debug, false, NULL, 0));

    sandbox_close(&at);
}

/* A runtime dependency reaches both, which is the other half of the same
   claim: the failure above is about the scope, not about the mechanism. */
MOLTEST(src_may_include_a_runtime_dependency) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_package(&at, "helper", NULL));

    char manifest[PATH_MAX_LEN * 2];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[target]\nstd = \"c17\"\n"
             "[deps]\nhelper = { path = \"%s/helper\" }\n",
             at.root);
    ASSERT_TRUE(make_project(&at, manifest,
                             "#include <helper.h>\n"
                             "int lib_answer(void) { return helper_answer(); }\n",
                             NULL));

    char app[PATH_MAX_LEN];
    app_path(&at, app, sizeof app);
    EXPECT_EQ(exit_ok, build_project(app, profile_debug, false, NULL, 0));

    sandbox_close(&at);
}

/* --- the graph --- */

/* Development dependencies stop at the root package. A library's own test
   framework is not the consumer's problem, and resolving it would download and
   version-check something nothing will ever compile. */
MOLTEST(a_dependencys_own_dev_deps_are_not_followed) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    snprintf(deps, sizeof deps, "[dev-deps]\nframework = { path = \"%s/framework\" }\n", at.root);
    ASSERT_TRUE(make_package(&at, "lib", deps));
    /* Deliberately never created: reaching for it would fail the resolve. */

    char manifest[PATH_MAX_LEN * 2];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[deps]\nlib = { path = \"%s/lib\" }\n",
             at.root);

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse(manifest, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    EXPECT_EQ(1u, dep_graph_count(graph));
    EXPECT_NULL(dep_graph_find(graph, "framework"));

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* One package in both tables is one node carrying both scopes — not two nodes,
   which in a test link would be duplicate symbols. */
MOLTEST(a_package_in_both_tables_is_one_node_with_both_scopes) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_package(&at, "shared", NULL));

    char manifest[PATH_MAX_LEN * 3];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[deps]\nshared = { path = \"%s/shared\" }\n"
             "[dev-deps]\nshared = { path = \"%s/shared\" }\n",
             at.root, at.root);

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse(manifest, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    ASSERT_EQ(1u, dep_graph_count(graph));
    const dep_node *node = dep_graph_find(graph, "shared");
    ASSERT_NOT_NULL(node);
    EXPECT_TRUE((node->scope & dep_scope_runtime) != 0);
    EXPECT_TRUE((node->scope & dep_scope_dev) != 0);

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* And when the two tables disagree about which package that is, it is a
   conflict like any other: the test binary links src/ against one of them and
   tests/ against the other. */
MOLTEST(the_two_tables_share_one_version) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_package(&at, "png_old", NULL));
    ASSERT_TRUE(make_package(&at, "png_new", NULL));

    char manifest[PATH_MAX_LEN * 3];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[deps]\npng = { path = \"%s/png_old\" }\n"
             "[dev-deps]\npng = { path = \"%s/png_new\" }\n",
             at.root, at.root);

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse(manifest, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    EXPECT_FALSE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "png"));

    sandbox_close(&at);
}

/* --- the lock --- */

/* The lock says which builds reach a package, so a production install can
   fetch only what it links. */
MOLTEST(the_lock_records_each_scope) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_package(&at, "ships", NULL));
    ASSERT_TRUE(make_package(&at, "helper", NULL));

    char manifest[PATH_MAX_LEN * 3];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[deps]\nships = { path = \"%s/ships\" }\n"
             "[dev-deps]\nhelper = { path = \"%s/helper\" }\n",
             at.root, at.root);

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse(manifest, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    char *text = lockfile_render(ctx.project_name, graph);
    ASSERT_NOT_NULL(text);
    const char *helper = strstr(text, "name = \"helper\"");
    const char *ships = strstr(text, "name = \"ships\"");
    ASSERT_NOT_NULL(helper);
    ASSERT_NOT_NULL(ships);
    EXPECT_NOT_NULL(strstr(helper, "scopes = [\"dev\"]"));
    EXPECT_NOT_NULL(strstr(ships, "scopes = [\"runtime\"]"));

    free(text);
    dep_graph_free(graph);
    sandbox_close(&at);
}

/* --- the manifest --- */

MOLTEST(the_two_tables_are_read_apart) {
    char err[512] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                              "[deps]\nyyjson = \"0.10.0\"\n"
                              "[dev-deps]\nmoltest = \"0.4.1\"\n",
                              &ctx, err, sizeof err));

    ASSERT_EQ(1u, ctx.deps.count);
    EXPECT_STREQ("yyjson", ctx.deps.items[0].name);
    ASSERT_EQ(1u, ctx.dev_deps.count);
    EXPECT_STREQ("moltest", ctx.dev_deps.items[0].name);
    EXPECT_STREQ("0.4.1", ctx.dev_deps.items[0].version);
}

/* The rules are the reader's, not the table's: a range is refused in either,
   and the message names the table the user has to go and edit. */
MOLTEST(a_range_in_dev_deps_names_that_table) {
    char err[512] = "";
    project_ctx ctx;
    EXPECT_FALSE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                               "[dev-deps]\nmoltest = \"^0.4.1\"\n",
                               &ctx, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[dev-deps].moltest"));
    EXPECT_NOT_NULL(strstr(err, "'^'"));
}
