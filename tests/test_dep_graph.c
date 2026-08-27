#include <moltest.h>

#include <molto/project/project_ctx.h>
#include <molto/services/dep_graph.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Walking the whole graph, without a network.
 *
 * A `path` dependency carries its own recipe, and a recipe's `[deps]` is read
 * by the same code that reads a manifest's — so a chain of directories
 * exercises every edge the walk can follow. What is not here is resolution
 * against a registry, which test_resolve_service.c covers against canned
 * bodies. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];
} sandbox;

static bool sandbox_open(sandbox *at) {
    snprintf(at->root, sizeof at->root, "%s", "/tmp/molto_graph_XXXXXX");
    return mkdtemp(at->root) != NULL;
}

static void sandbox_close(const sandbox *at) { (void)fs_remove_tree(at->root); }

/* One package on disk: a source file, and a recipe naming `deps` as its own
   dependencies — each of which is a sibling directory in the same sandbox. */
static bool make_package(const sandbox *at, const char *name, const char *deps) {
    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    if (!fs_format_path(dir, sizeof dir, "%s/%s", at->root, name) || !fs_make_dirs(dir))
        return false;

    char recipe[PATH_MAX_LEN * 4];
    snprintf(recipe, sizeof recipe,
             "schema = 1\nform = \"source\"\nkind = \"package\"\n"
             "name = \"%s\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
             "[artifacts]\ntype = \"source\"\nsources = [\"%s.c\"]\ninclude = [\".\"]\n"
             "%s",
             name, name, deps == NULL ? "" : deps);

    if (!fs_format_path(file, sizeof file, "%s/recipe.toml", dir) || !fs_write_file(file, recipe))
        return false;
    if (!fs_format_path(file, sizeof file, "%s/%s.c", dir, name))
        return false;
    return fs_write_file(file, "int answer(void) { return 1; }\n");
}

/* A `[deps]` fragment naming one sibling package by path. */
static void dep_on(const sandbox *at, const char *name, char *out, size_t out_size) {
    snprintf(out, out_size, "[deps]\n%s = { path = \"%s/%s\" }\n", name, at->root, name);
}

/* A root manifest depending on the named sibling packages by path. */
static bool parse_root(const sandbox *at, const char *const *names, size_t count, project_ctx *out,
                       char *err, size_t err_size) {
    char manifest[PATH_MAX_LEN * 4];
    int used = snprintf(manifest, sizeof manifest,
                        "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[deps]\n");
    for (size_t i = 0; i < count; i++) {
        used += snprintf(manifest + used, sizeof manifest - (size_t)used,
                         "%s = { path = \"%s/%s\" }\n", names[i], at->root, names[i]);
    }
    return project_parse(manifest, out, err, err_size);
}

/* The point of the whole exercise: the manifest names one dependency and the
   build gets three, because each recipe named the next. */
MOLTEST(the_graph_reaches_past_what_the_manifest_declared) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    dep_on(&at, "c", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "b", deps));
    dep_on(&at, "b", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "a", deps));
    EXPECT_TRUE(make_package(&at, "c", NULL));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));
    ASSERT_EQ(1u, ctx.deps.count);

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    EXPECT_EQ(3u, dep_graph_count(graph));
    /* Sorted by name, so a lock file's diff is worth reading. */
    EXPECT_STREQ("a", dep_graph_at(graph, 0)->name);
    EXPECT_STREQ("b", dep_graph_at(graph, 1)->name);
    EXPECT_STREQ("c", dep_graph_at(graph, 2)->name);

    /* And each node remembers who pulled it in, which is what a conflict
       message and a lock file both need. */
    EXPECT_STREQ("", dep_graph_find(graph, "a")->required_by);
    EXPECT_STREQ("a", dep_graph_find(graph, "b")->required_by);
    EXPECT_STREQ("b", dep_graph_find(graph, "c")->required_by);

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* The edges, recorded per node, are what a lock file writes as `dependencies`
   and what makes the graph reconstructible without walking it again. */
MOLTEST(a_node_records_its_own_edges_sorted) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 4];
    snprintf(deps, sizeof deps, "[deps]\nz = { path = \"%s/z\" }\nm = { path = \"%s/m\" }\n",
             at.root, at.root);
    EXPECT_TRUE(make_package(&at, "a", deps));
    EXPECT_TRUE(make_package(&at, "z", NULL));
    EXPECT_TRUE(make_package(&at, "m", NULL));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    const dep_node *a = dep_graph_find(graph, "a");
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(2u, str_list_count(&a->dependencies));
    EXPECT_STREQ("m", str_list_get(&a->dependencies, 0));
    EXPECT_STREQ("z", str_list_get(&a->dependencies, 1));

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* Two dependents on one package is one node, not two. Anything else is two
   copies of the same library in one link. */
MOLTEST(a_package_reached_twice_is_one_node) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    dep_on(&at, "shared", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "a", deps));
    EXPECT_TRUE(make_package(&at, "b", deps));
    EXPECT_TRUE(make_package(&at, "shared", NULL));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a", "b"};
    ASSERT_TRUE(parse_root(&at, names, 2, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    EXPECT_EQ(3u, dep_graph_count(graph));
    EXPECT_NOT_NULL(dep_graph_find(graph, "shared"));

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* A cycle is not an error and must not hang. With `type = "source"` both drops
   land in one binary, so `a` needing `b` needing `a` describes a build that
   works; the visited set is what makes it terminate. */
MOLTEST(a_cycle_terminates_and_is_not_an_error) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    dep_on(&at, "b", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "a", deps));
    dep_on(&at, "a", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "b", deps));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    EXPECT_EQ(2u, dep_graph_count(graph));

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* One name pointed at two different directories is two packages wearing one
   name. Unifying them would be the duplicate-symbol problem again, so the walk
   refuses and says who asked for each. */
MOLTEST(one_name_from_two_sources_is_a_conflict) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    /* Both dependents call it `png`, and they mean different directories. */
    snprintf(deps, sizeof deps, "[deps]\npng = { path = \"%s/png_old\" }\n", at.root);
    EXPECT_TRUE(make_package(&at, "a", deps));
    snprintf(deps, sizeof deps, "[deps]\npng = { path = \"%s/png_new\" }\n", at.root);
    EXPECT_TRUE(make_package(&at, "b", deps));
    EXPECT_TRUE(make_package(&at, "png_old", NULL));
    EXPECT_TRUE(make_package(&at, "png_new", NULL));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a", "b"};
    ASSERT_TRUE(parse_root(&at, names, 2, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    EXPECT_FALSE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    EXPECT_NULL(graph);

    /* The message has to name the package and both requirers: the fix is to
       change one of them, and the user needs to know which two to look at. */
    EXPECT_NOT_NULL(strstr(err, "png"));
    EXPECT_NOT_NULL(strstr(err, "png_old"));
    EXPECT_NOT_NULL(strstr(err, "png_new"));
    EXPECT_NOT_NULL(strstr(err, " by a"));
    EXPECT_NOT_NULL(strstr(err, " by b"));

    sandbox_close(&at);
}

/* A dependency that fails deep in the graph says where it was reached from.
   "could not read x" without that is a name the manifest never mentions. */
MOLTEST(a_failure_names_who_required_it) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    dep_on(&at, "missing", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "a", deps));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    EXPECT_FALSE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "missing"));
    EXPECT_NOT_NULL(strstr(err, "required by 'a'"));

    sandbox_close(&at);
}

/* No dependencies is an empty graph, not a failure, and it must not go looking
   for a registry to tell it so. */
MOLTEST(no_dependencies_is_an_empty_graph) {
    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n", &ctx, err,
                              sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    EXPECT_EQ(0u, dep_graph_count(graph));
    EXPECT_NULL(dep_graph_at(graph, 0));

    dep_graph_free(graph);
}

/* As `make_package`, at a nested location: the package name stays valid while
   the path to it grows past what a conflict record can hold. */
static bool make_package_under(const sandbox *at, const char *subdir, const char *name,
                               char *path, size_t path_size) {
    char dir[PATH_MAX_LEN];
    if (!fs_format_path(dir, sizeof dir, "%s/%s/%s", at->root, subdir, name)
        || !fs_make_dirs(dir))
        return false;

    char recipe[PATH_MAX_LEN * 2];
    snprintf(recipe, sizeof recipe,
             "schema = 1\nform = \"source\"\nkind = \"package\"\n"
             "name = \"%s\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
             "[artifacts]\ntype = \"source\"\nsources = [\"%s.c\"]\ninclude = [\".\"]\n",
             name, name);

    char file[PATH_MAX_LEN];
    if (!fs_format_path(file, sizeof file, "%s/recipe.toml", dir) || !fs_write_file(file, recipe))
        return false;
    if (!fs_format_path(file, sizeof file, "%s/%s.c", dir, name))
        return false;
    snprintf(path, path_size, "%s", dir);
    return fs_write_file(file, "int answer(void) { return 1; }\n");
}

/* The defect this closes: a source is a URL or a path, and it used to be
   copied into a field sized for a version number. Everything past sixty-three
   characters was dropped, silently, in the one message whose whole job is to
   say which two things disagree — so two dependencies under a long enough
   prefix were reported as the same string twice. */
MOLTEST(a_conflict_between_two_long_paths_names_both_of_them) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    /* Long enough that the two paths share their first DEP_IDENTITY_MAX bytes
       and differ only at the end, which is exactly the case a tail-clipping
       copy cannot report. */
    char deep[192];
    memset(deep, 'd', sizeof deep - 1);
    deep[sizeof deep - 1] = '\0';

    char old_path[PATH_MAX_LEN];
    char new_path[PATH_MAX_LEN];
    ASSERT_TRUE(make_package_under(&at, deep, "png_old", old_path, sizeof old_path));
    ASSERT_TRUE(make_package_under(&at, deep, "png_new", new_path, sizeof new_path));

    char deps[PATH_MAX_LEN * 2];
    snprintf(deps, sizeof deps, "[deps]\npng = { path = \"%s\" }\n", old_path);
    EXPECT_TRUE(make_package(&at, "a", deps));
    snprintf(deps, sizeof deps, "[deps]\npng = { path = \"%s\" }\n", new_path);
    EXPECT_TRUE(make_package(&at, "b", deps));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a", "b"};
    ASSERT_TRUE(parse_root(&at, names, 2, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    dep_conflict conflict = {0};
    const dep_resolve_options options = { .propose = true };
    EXPECT_FALSE(dep_graph_resolve_with(&ctx, &options, &graph, &conflict, err, sizeof err));

    /* Both ends survive, so the two claims read as two different things. */
    EXPECT_NOT_NULL(strstr(conflict.version, "png_old"));
    EXPECT_NOT_NULL(strstr(conflict.other_version, "png_new"));
    EXPECT_STRNE(conflict.version, conflict.other_version);
    /* And the shortening is visible rather than implied. */
    EXPECT_NOT_NULL(strstr(conflict.version, "\xe2\x80\xa6"));

    sandbox_close(&at);
}

/* A conflict is reported as data as well as as a message, so a caller can ask
   the user about it instead of only printing it (RFC-0008). */
MOLTEST(a_conflict_is_reported_as_the_two_claims_and_who_made_them) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    snprintf(deps, sizeof deps, "[deps]\npng = { path = \"%s/png_old\" }\n", at.root);
    EXPECT_TRUE(make_package(&at, "a", deps));
    snprintf(deps, sizeof deps, "[deps]\npng = { path = \"%s/png_new\" }\n", at.root);
    EXPECT_TRUE(make_package(&at, "b", deps));
    EXPECT_TRUE(make_package(&at, "png_old", NULL));
    EXPECT_TRUE(make_package(&at, "png_new", NULL));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a", "b"};
    ASSERT_TRUE(parse_root(&at, names, 2, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    dep_conflict conflict = {0};
    const dep_resolve_options options = { .propose = true };
    EXPECT_FALSE(dep_graph_resolve_with(&ctx, &options, &graph, &conflict, err, sizeof err));

    EXPECT_STREQ("png", conflict.name);
    EXPECT_STREQ("a", conflict.required_by);
    EXPECT_STREQ("b", conflict.other_required_by);
    /* A path dependency has no version, so it is identified by where it came
       from — and there is nothing to propose, because a directory has no
       releases to choose between. */
    EXPECT_NOT_NULL(strstr(conflict.version, "png_old"));
    EXPECT_NOT_NULL(strstr(conflict.other_version, "png_new"));
    EXPECT_FALSE(conflict.has_proposal);

    sandbox_close(&at);
}

/* Nothing set means nothing to ask about: a caller distinguishes a conflict
   from an unreachable registry by whether the name is filled in. */
MOLTEST(an_ordinary_failure_leaves_the_conflict_record_empty) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    snprintf(deps, sizeof deps, "[deps]\nmissing = { path = \"%s/nowhere\" }\n", at.root);
    EXPECT_TRUE(make_package(&at, "a", deps));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    dep_conflict conflict = {0};
    const dep_resolve_options options = { .propose = true };
    EXPECT_FALSE(dep_graph_resolve_with(&ctx, &options, &graph, &conflict, err, sizeof err));
    EXPECT_EQ('\0', conflict.name[0]);

    sandbox_close(&at);
}

/* --- what one package reaches --- */

/* The closure is what a dependency is allowed to see: its own dependencies and
   theirs, so the headers it includes are on its command line and nothing else
   is (RFC-0008). */
MOLTEST(a_package_reaches_what_its_dependencies_reach) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    dep_on(&at, "c", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "b", deps));
    dep_on(&at, "b", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "a", deps));
    EXPECT_TRUE(make_package(&at, "c", NULL));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    str_list reached;
    str_list_init(&reached);
    ASSERT_TRUE(dep_graph_closure(graph, "a", &reached));
    /* Breadth-first from `a`, and `a` is not in its own closure. */
    ASSERT_EQ(2u, str_list_count(&reached));
    EXPECT_STREQ("b", str_list_get(&reached, 0));
    EXPECT_STREQ("c", str_list_get(&reached, 1));
    str_list_free(&reached);

    /* A leaf reaches nothing, which is what keeps a sibling's flags off its
       command line. */
    str_list leaf;
    str_list_init(&leaf);
    ASSERT_TRUE(dep_graph_closure(graph, "c", &leaf));
    EXPECT_EQ(0u, str_list_count(&leaf));
    str_list_free(&leaf);

    /* A name nobody resolved reaches nothing, and that is not a failure. */
    str_list absent;
    str_list_init(&absent);
    EXPECT_TRUE(dep_graph_closure(graph, "nowhere", &absent));
    EXPECT_EQ(0u, str_list_count(&absent));
    str_list_free(&absent);

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* Two dependents on one package see it once each, and neither sees the other:
   the closure is per package, not the union the build used to hand out. */
MOLTEST(a_closure_leaves_out_the_siblings) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    dep_on(&at, "shared", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "a", deps));
    EXPECT_TRUE(make_package(&at, "b", deps));
    EXPECT_TRUE(make_package(&at, "shared", NULL));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a", "b"};
    ASSERT_TRUE(parse_root(&at, names, 2, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    str_list reached;
    str_list_init(&reached);
    ASSERT_TRUE(dep_graph_closure(graph, "a", &reached));
    ASSERT_EQ(1u, str_list_count(&reached));
    EXPECT_STREQ("shared", str_list_get(&reached, 0));

    /* Accumulated into the same list, `b` adds nothing: it reaches the one
       package `a` already put there. */
    ASSERT_TRUE(dep_graph_closure(graph, "b", &reached));
    EXPECT_EQ(1u, str_list_count(&reached));
    str_list_free(&reached);

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* The walk that built the graph closes cycles by visiting a name once. This one
   walks a graph already built, so it closes them itself — and a package is
   never in its own closure, however it loops back. */
MOLTEST(a_closure_over_a_cycle_terminates) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    dep_on(&at, "b", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "a", deps));
    dep_on(&at, "a", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "b", deps));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"a"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    str_list reached;
    str_list_init(&reached);
    ASSERT_TRUE(dep_graph_closure(graph, "a", &reached));
    ASSERT_EQ(1u, str_list_count(&reached));
    EXPECT_STREQ("b", str_list_get(&reached, 0));
    str_list_free(&reached);

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* What a recipe says about itself (RFC-0009 `[about]`) reaches the graph, and
   reaches it for free: the walk already parses that recipe to learn what to
   compile, so there is no extra fetch and no second reader. It is what turns a
   resolved graph into something that can name the licence of every component
   it links. */
MOLTEST(a_node_carries_what_its_recipe_says_about_itself) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    EXPECT_TRUE(make_package(&at, "png",
                             "[about]\n"
                             "description = \"The PNG reference library\"\n"
                             "license = \"libpng-2.0\"\n"
                             "homepage = \"http://www.libpng.org\"\n"));
    EXPECT_TRUE(make_package(&at, "quiet", NULL));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"png", "quiet"};
    ASSERT_TRUE(parse_root(&at, names, 2, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    const dep_node *png = dep_graph_find(graph, "png");
    ASSERT_NOT_NULL(png);
    EXPECT_STREQ("libpng-2.0", png->about.license);
    EXPECT_STREQ("The PNG reference library", png->about.description);
    EXPECT_STREQ("http://www.libpng.org", png->about.homepage);

    /* A recipe with no [about] is not an error: it simply says nothing, and
       what reads the graph has to be able to tell that apart from a licence. */
    const dep_node *quiet = dep_graph_find(graph, "quiet");
    ASSERT_NOT_NULL(quiet);
    EXPECT_STREQ("", quiet->about.license);

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* Reading a source string back. What composes these lives one function away
   from what parses them, and both spell the scheme from the same constant —
   but the mapping is what a report and a lock file both depend on, so it is
   pinned here rather than assumed. */
MOLTEST(a_source_string_says_which_kind_of_origin_it_names) {
    EXPECT_EQ(dep_source_version, dep_graph_source_kind("registry+https://molto.dev"));
    EXPECT_EQ(dep_source_git, dep_graph_source_kind("git+https://example.test/x.git#5a1e8ff"));
    EXPECT_EQ(dep_source_path, dep_graph_source_kind("path+/home/someone/modules/net"));
    EXPECT_EQ(dep_source_archive, dep_graph_source_kind("archive+https://example.test/x.tar.gz"));
}

/* Anything unreadable is a path: bytes nobody can go back for. Answering
   "registry" would be the one wrong answer, because that is the kind whose
   version and checksum are claims someone is expected to verify. */
MOLTEST(an_unreadable_source_is_not_taken_for_a_registry_package) {
    EXPECT_EQ(dep_source_path, dep_graph_source_kind(""));
    EXPECT_EQ(dep_source_path, dep_graph_source_kind("https://example.test/x.tar.gz"));
    EXPECT_EQ(dep_source_path, dep_graph_source_kind("registry"));
    EXPECT_EQ(dep_source_path, dep_graph_source_kind(NULL));
}

/* --- the build system a dependency's recipe names --- */

/* Molto runs none of them. Compiling the sources anyway is not a lesser version
   of honouring the recipe: it is a green build of something the recipe said
   needs configuring first, which is what happened while nothing read the
   table. */
MOLTEST(a_dependency_built_by_a_build_system_molto_cannot_run_is_refused) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    EXPECT_TRUE(make_package(&at, "png", "[build]\nsystem = \"cmake\"\n"));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"png"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    EXPECT_FALSE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    EXPECT_NULL(graph);

    /* Both halves are the message's job: which dependency to look at, and what
       it asked for that molto could not give it. */
    EXPECT_NOT_NULL(strstr(err, "png"));
    EXPECT_NOT_NULL(strstr(err, "cmake"));

    sandbox_close(&at);
}

/* The one value molto can honour, and the reason the refusal above is a
   refusal rather than the whole table being rejected. */
MOLTEST(a_dependency_that_names_no_build_system_resolves) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    EXPECT_TRUE(make_package(&at, "amalgam", "[build]\nsystem = \"none\"\n"));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"amalgam"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    EXPECT_EQ(1u, dep_graph_count(graph));

    dep_graph_free(graph);
    sandbox_close(&at);
}

/* Reached through another package rather than named by the manifest: the
   refusal is on the walk, so depth does not get around it. */
MOLTEST(a_transitive_dependency_naming_a_build_system_is_refused_too) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char deps[PATH_MAX_LEN * 2];
    dep_on(&at, "png", deps, sizeof deps);
    EXPECT_TRUE(make_package(&at, "wrapper", deps));
    EXPECT_TRUE(make_package(&at, "png", "[build]\nsystem = \"autotools\"\n"));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"wrapper"};
    ASSERT_TRUE(parse_root(&at, names, 1, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    EXPECT_FALSE(dep_graph_resolve(&ctx, &graph, err, sizeof err));

    EXPECT_NOT_NULL(strstr(err, "png"));
    EXPECT_NOT_NULL(strstr(err, "autotools"));
    /* And says who pulled it in, which is where the fix has to go. */
    EXPECT_NOT_NULL(strstr(err, "wrapper"));

    sandbox_close(&at);
}
