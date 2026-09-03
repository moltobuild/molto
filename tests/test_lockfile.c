#include <moltest.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/project/lockfile.h>
#include <molto/project/project_ctx.h>
#include <molto/services/build_service.h>
#include <molto/services/dep_graph.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The lock file, on a graph built from directories on disk.
 *
 * Rendering is tested against text rather than against a re-parse, because the
 * text is the artifact: it is committed, and its diff is the reason to commit
 * it. A round trip that agreed with itself while producing a file nobody could
 * read would pass. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];
} sandbox;

static bool sandbox_open(sandbox *at) {
    return moltest_temp_dir("molto_lock", at->root, sizeof at->root);
}

static void sandbox_close(const sandbox *at) { (void)fs_remove_tree(at->root); }

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
    if (!fs_format_path(file, sizeof file, "%s/%s.c", dir, name))
        return false;
    return fs_write_file(file, "int answer(void) { return 1; }\n");
}

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

/* `a` depends on `b`; the root depends on `a`. */
static bool build_chain(sandbox *at, project_ctx *ctx, dep_graph **graph) {
    if (!sandbox_open(at))
        return false;
    char deps[PATH_MAX_LEN * 2];
    snprintf(deps, sizeof deps, "[deps]\nb = { path = \"%s/b\" }\n", at->root);
    if (!make_package(at, "a", deps) || !make_package(at, "b", NULL))
        return false;

    char err[512] = "";
    const char *const names[] = {"a"};
    if (!parse_root(at, names, 1, ctx, err, sizeof err))
        return false;
    return dep_graph_resolve(ctx, graph, err, sizeof err);
}

MOLTEST(the_lock_records_the_whole_graph_sorted) {
    sandbox at;
    project_ctx ctx;
    dep_graph *graph = NULL;
    ASSERT_TRUE(build_chain(&at, &ctx, &graph));

    char *text = lockfile_render(ctx.project_name, graph, NULL, 0);
    ASSERT_NOT_NULL(text);

    EXPECT_NOT_NULL(strstr(text, "version = 1\n"));
    EXPECT_NOT_NULL(strstr(text, "root = \"app\"\n"));
    /* The transitive dependency is in the file, which is the point of writing
       one: the manifest never mentions `b`. */
    EXPECT_NOT_NULL(strstr(text, "name = \"a\"\n"));
    EXPECT_NOT_NULL(strstr(text, "name = \"b\"\n"));
    EXPECT_NOT_NULL(strstr(text, "dependencies = [\"b\"]\n"));
    EXPECT_NOT_NULL(strstr(text, "scopes = [\"runtime\"]\n"));
    /* Sorted, so the diff of a regenerated lock is empty when nothing moved. */
    EXPECT_TRUE(strstr(text, "name = \"a\"") < strstr(text, "name = \"b\""));
    /* A path dependency has no version and no checksum, and the keys are
       omitted rather than written blank. */
    EXPECT_NULL(strstr(text, "version = \"\""));
    EXPECT_NULL(strstr(text, "checksum = \"\""));
    EXPECT_NOT_NULL(strstr(text, "source = \"path+"));

    free(text);
    dep_graph_free(graph);
    sandbox_close(&at);
}

/* Regenerating an unchanged graph must produce the same bytes, or the file is
   noise in every diff and nobody reads it. */
MOLTEST(rendering_twice_gives_the_same_bytes) {
    sandbox at;
    project_ctx ctx;
    dep_graph *graph = NULL;
    ASSERT_TRUE(build_chain(&at, &ctx, &graph));

    char *first = lockfile_render(ctx.project_name, graph, NULL, 0);
    char *second = lockfile_render(ctx.project_name, graph, NULL, 0);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    EXPECT_STREQ(first, second);

    free(first);
    free(second);
    dep_graph_free(graph);
    sandbox_close(&at);
}

MOLTEST(a_written_lock_reads_back) {
    sandbox at;
    project_ctx ctx;
    dep_graph *graph = NULL;
    ASSERT_TRUE(build_chain(&at, &ctx, &graph));

    char err[512] = "";
    ASSERT_TRUE(lockfile_write(at.root, ctx.project_name, graph, NULL, 0, err, sizeof err));

    lockfile lock;
    ASSERT_TRUE(lockfile_read(at.root, &lock, err, sizeof err));
    EXPECT_EQ(1L, lock.version);
    EXPECT_STREQ("app", lock.root);
    ASSERT_EQ(2u, lock.count);
    EXPECT_STREQ("a", lock.packages[0].name);
    ASSERT_EQ(1u, str_list_count(&lock.packages[0].dependencies));
    EXPECT_STREQ("b", str_list_get(&lock.packages[0].dependencies, 0));
    EXPECT_STREQ("b", lock.packages[1].name);

    /* Written for the manifest it was resolved from, so it still describes it. */
    EXPECT_TRUE(lockfile_matches(&lock, &ctx));

    lockfile_free(&lock);
    dep_graph_free(graph);
    sandbox_close(&at);
}

/* A format this reader does not understand is discarded rather than guessed
   at, the same rule the WSDB follows. Re-resolving is always correct. */
MOLTEST(a_lock_from_the_future_is_refused) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    char path[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/Molto.lock", at.root));
    ASSERT_TRUE(fs_write_file(path, "version = 99\nroot = \"app\"\n"));

    lockfile lock;
    char err[512] = "";
    EXPECT_FALSE(lockfile_read(at.root, &lock, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "version 99"));

    sandbox_close(&at);
}

MOLTEST(a_missing_lock_is_reported_and_not_a_crash) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    lockfile lock;
    char err[512] = "";
    EXPECT_FALSE(lockfile_read(at.root, &lock, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "no Molto.lock"));

    sandbox_close(&at);
}

/* Editing the manifest makes the lock stale. Adding is the easy half. */
MOLTEST(a_lock_stops_matching_when_a_dependency_is_added) {
    sandbox at;
    project_ctx ctx;
    dep_graph *graph = NULL;
    ASSERT_TRUE(build_chain(&at, &ctx, &graph));

    char err[512] = "";
    ASSERT_TRUE(lockfile_write(at.root, ctx.project_name, graph, NULL, 0, err, sizeof err));
    dep_graph_free(graph);

    /* The manifest now names a second dependency the lock never saw. */
    EXPECT_TRUE(make_package(&at, "c", NULL));
    project_ctx grown;
    const char *const names[] = {"a", "c"};
    ASSERT_TRUE(parse_root(&at, names, 2, &grown, err, sizeof err));

    lockfile lock;
    ASSERT_TRUE(lockfile_read(at.root, &lock, err, sizeof err));
    EXPECT_FALSE(lockfile_matches(&lock, &grown));

    lockfile_free(&lock);
    sandbox_close(&at);
}

/* And removing one is the half a check of the direct entries alone would miss:
   every dependency the manifest still names is locked at the right version, and
   the lock is stale anyway because it holds a package nothing reaches. */
MOLTEST(a_lock_stops_matching_when_a_dependency_is_removed) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    EXPECT_TRUE(make_package(&at, "a", NULL));
    EXPECT_TRUE(make_package(&at, "c", NULL));

    char err[512] = "";
    project_ctx both;
    const char *const two[] = {"a", "c"};
    ASSERT_TRUE(parse_root(&at, two, 2, &both, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&both, &graph, err, sizeof err));
    ASSERT_TRUE(lockfile_write(at.root, both.project_name, graph, NULL, 0, err, sizeof err));
    dep_graph_free(graph);

    project_ctx fewer;
    const char *const one[] = {"a"};
    ASSERT_TRUE(parse_root(&at, one, 1, &fewer, err, sizeof err));

    lockfile lock;
    ASSERT_TRUE(lockfile_read(at.root, &lock, err, sizeof err));
    EXPECT_FALSE(lockfile_matches(&lock, &fewer));

    lockfile_free(&lock);
    sandbox_close(&at);
}

/* A git URL or a directory can carry a quote or a backslash. Writing one
   unescaped produces a lock file that stops parsing, and it would be found by
   whoever owns that path, long after the commit that broke it. */
MOLTEST(a_quote_in_a_source_survives_the_round_trip) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char path[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/Molto.lock", at.root));
    ASSERT_TRUE(fs_write_file(path, "version = 1\nroot = \"app\"\n\n[[package]]\n"
                                    "name = \"odd\"\n"
                                    "source = \"path+/tmp/we\\\"ird\\\\here\"\n"
                                    "scopes = [\"runtime\"]\n"
                                    "dependencies = []\n"));

    lockfile lock;
    char err[512] = "";
    ASSERT_TRUE(lockfile_read(at.root, &lock, err, sizeof err));
    ASSERT_EQ(1u, lock.count);
    EXPECT_STREQ("path+/tmp/we\"ird\\here", lock.packages[0].source);

    lockfile_free(&lock);
    sandbox_close(&at);
}

/* The same path, through the writer: a real dependency living in a directory
   whose name carries a quote. The package is called `odd` and its directory is
   not, which is what lets the name stay a name while the path stays awkward. */
MOLTEST(the_writer_escapes_a_quote_in_a_path) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(dir, sizeof dir, "%s/we\"ird", at.root));
    /* Asked of the system rather than assumed of the platform, the way the
       symlink test above asks: a quote is one of the characters Windows
       forbids in a filename, so the directory this needs cannot exist there.
       What is under test -- that the writer escapes a quote it is handed --
       is exercised by the reader's case either way; what cannot be arranged
       is a real dependency living behind such a name. */
    if(!fs_make_dirs(dir))
        SKIP("this system will not make a directory with a quote in its name");
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/recipe.toml", dir));
    ASSERT_TRUE(fs_write_file(file, "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                    "name = \"odd\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                                    "[artifacts]\ntype = \"source\"\nsources = [\"odd.c\"]\n"));
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/odd.c", dir));
    ASSERT_TRUE(fs_write_file(file, "int answer(void) { return 1; }\n"));

    char manifest[PATH_MAX_LEN * 2];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[deps]\nodd = { path = \"%s/we\\\"ird\" }\n",
             at.root);

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(project_parse(manifest, &ctx, err, sizeof err));

    dep_graph *graph = NULL;
    ASSERT_TRUE(dep_graph_resolve(&ctx, &graph, err, sizeof err));
    ASSERT_TRUE(lockfile_write(at.root, ctx.project_name, graph, NULL, 0, err, sizeof err));

    /* Written escaped, and therefore readable again. */
    lockfile lock;
    ASSERT_TRUE(lockfile_read(at.root, &lock, err, sizeof err));
    ASSERT_EQ(1u, lock.count);
    EXPECT_NOT_NULL(strstr(lock.packages[0].source, "we\"ird"));

    lockfile_free(&lock);
    dep_graph_free(graph);
    sandbox_close(&at);
}

/* --- the build writes one --- */

/* The lock is written by the build, not by a command of its own, because that
   is where the graph is already in hand. If this stops happening, nothing else
   in this file would notice. */
MOLTEST(a_build_with_dependencies_writes_the_lock) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    EXPECT_TRUE(make_package(&at, "greet", NULL));

    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(dir, sizeof dir, "%s/app/src", at.root));
    ASSERT_TRUE(fs_make_dirs(dir));
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/main.c", dir));
    ASSERT_TRUE(fs_write_file(file, "int answer(void);\nint main(void) { return answer() - 1; }\n"));

    char manifest[PATH_MAX_LEN * 2];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[target]\nstd = \"c17\"\n"
             "[deps]\ngreet = { path = \"%s/greet\" }\n",
             at.root);
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/app/Project.toml", at.root));
    ASSERT_TRUE(fs_write_file(file, manifest));

    char app[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(app, sizeof app, "%s/app", at.root));
    ASSERT_EQ(exit_ok, build_project(app, profile_debug, NULL, false, 0, NULL, 0));

    lockfile lock;
    char err[512] = "";
    ASSERT_TRUE(lockfile_read(app, &lock, err, sizeof err));
    ASSERT_EQ(1u, lock.count);
    EXPECT_STREQ("greet", lock.packages[0].name);

    lockfile_free(&lock);
    sandbox_close(&at);
}

/* A project with no dependencies has nothing to lock, and must not litter one
   into every repository that never needed it — molto's own included. */
MOLTEST(a_build_without_dependencies_writes_no_lock) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(dir, sizeof dir, "%s/src", at.root));
    ASSERT_TRUE(fs_make_dirs(dir));
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/main.c", dir));
    ASSERT_TRUE(fs_write_file(file, "int main(void) { return 0; }\n"));
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/Project.toml", at.root));
    ASSERT_TRUE(fs_write_file(file, "[package]\nname = \"bare\"\nversion = \"0.1.0\"\n"));

    ASSERT_EQ(exit_ok, build_project(at.root, profile_debug, NULL, false, 0, NULL, 0));

    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/Molto.lock", at.root));
    EXPECT_FALSE(fs_path_exists(file));

    sandbox_close(&at);
}

/* --- holding a resolution against what was locked --- */

/* The resolution that produced a lock agrees with it. Anything else here would
   mean a build could never be repeated. */
MOLTEST(a_resolution_verifies_against_its_own_lock) {
    sandbox at;
    project_ctx ctx;
    dep_graph *graph = NULL;
    ASSERT_TRUE(build_chain(&at, &ctx, &graph));

    char err[512] = "";
    ASSERT_TRUE(lockfile_write(at.root, ctx.project_name, graph, NULL, 0, err, sizeof err));

    lockfile lock;
    ASSERT_TRUE(lockfile_read(at.root, &lock, err, sizeof err));
    EXPECT_TRUE(lockfile_verify(&lock, graph, err, sizeof err));

    lockfile_free(&lock);
    dep_graph_free(graph);
    sandbox_close(&at);
}

/* A coordinate that now points somewhere else. The registry is a remote party
   and a coordinate is supposed to be immutable, so this is the check the lock
   exists for. */
MOLTEST(a_source_that_moved_is_refused) {
    sandbox at;
    project_ctx ctx;
    dep_graph *graph = NULL;
    ASSERT_TRUE(build_chain(&at, &ctx, &graph));

    char path[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/Molto.lock", at.root));
    ASSERT_TRUE(fs_write_file(path, "version = 1\nroot = \"app\"\n\n"
                                    "[[package]]\nname = \"a\"\nsource = \"path+/elsewhere/a\"\n"
                                    "scopes = [\"runtime\"]\ndependencies = [\"b\"]\n\n"
                                    "[[package]]\nname = \"b\"\nsource = \"path+/elsewhere/b\"\n"
                                    "scopes = [\"runtime\"]\ndependencies = []\n"));

    lockfile lock;
    char err[512] = "";
    ASSERT_TRUE(lockfile_read(at.root, &lock, err, sizeof err));
    EXPECT_FALSE(lockfile_verify(&lock, graph, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "now comes from"));

    lockfile_free(&lock);
    dep_graph_free(graph);
    sandbox_close(&at);
}

/* A dependency that appeared out of a coordinate that was supposed to be
   frozen. This is the shape a compromised release has. */
MOLTEST(a_package_the_lock_never_saw_is_refused) {
    sandbox at;
    project_ctx ctx;
    dep_graph *graph = NULL;
    ASSERT_TRUE(build_chain(&at, &ctx, &graph));

    /* The lock knows `a` and not the `b` that `a` pulls in. */
    char path[PATH_MAX_LEN];
    char lock_text[DEP_GRAPH_SOURCE_MAX + PATH_MAX_LEN];
    const dep_node *a = dep_graph_find(graph, "a");
    ASSERT_NOT_NULL(a);
    snprintf(lock_text, sizeof lock_text,
             "version = 1\nroot = \"app\"\n\n[[package]]\nname = \"a\"\nsource = \"%s\"\n"
             "scopes = [\"runtime\"]\ndependencies = []\n",
             a->source);
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/Molto.lock", at.root));
    ASSERT_TRUE(fs_write_file(path, lock_text));

    lockfile lock;
    char err[512] = "";
    ASSERT_TRUE(lockfile_read(at.root, &lock, err, sizeof err));
    EXPECT_FALSE(lockfile_verify(&lock, graph, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "'b'"));

    lockfile_free(&lock);
    dep_graph_free(graph);
    sandbox_close(&at);
}

/* And the build refuses too, rather than warning: a resolution that disagrees
   with the lock is the one case where continuing is the wrong default. */
MOLTEST(a_build_stops_when_the_lock_disagrees) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    EXPECT_TRUE(make_package(&at, "greet", NULL));

    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(dir, sizeof dir, "%s/app/src", at.root));
    ASSERT_TRUE(fs_make_dirs(dir));
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/main.c", dir));
    ASSERT_TRUE(fs_write_file(file, "int main(void) { return 0; }\n"));

    char manifest[PATH_MAX_LEN * 2];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[target]\nstd = \"c17\"\n"
             "[deps]\ngreet = { path = \"%s/greet\" }\n",
             at.root);
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/app/Project.toml", at.root));
    ASSERT_TRUE(fs_write_file(file, manifest));

    char app[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(app, sizeof app, "%s/app", at.root));
    ASSERT_EQ(exit_ok, build_project(app, profile_debug, NULL, false, 0, NULL, 0));

    /* Someone repoints the same name at a different directory. */
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/Molto.lock", app));
    ASSERT_TRUE(fs_write_file(file, "version = 1\nroot = \"app\"\n\n[[package]]\n"
                                    "name = \"greet\"\nsource = \"path+/somewhere/else\"\n"
                                    "scopes = [\"runtime\"]\ndependencies = []\n"));
    EXPECT_EQ(exit_dependency_failure, build_project(app, profile_debug, NULL, false, 0, NULL, 0));

    sandbox_close(&at);
}

/* --- what the host answered --- */

static lock_host one_host(const char *capability, const char *version) {
    lock_host entry = {0};
    snprintf(entry.capability, sizeof entry.capability, "%s", capability);
    snprintf(entry.answered, sizeof entry.answered, "%s", "pkg-config");
    snprintf(entry.version, sizeof entry.version, "%s", version);
    return entry;
}

MOLTEST(the_lock_records_what_a_host_library_answered) {
    const lock_host hosts[] = {one_host("gtk+-3.0", "3.24.33")};
    char *text = lockfile_render("ui", NULL, hosts, 1);
    ASSERT_NOT_NULL(text);

    EXPECT_NOT_NULL(strstr(text, "[[host]]"));
    EXPECT_NOT_NULL(strstr(text, "capability = \"gtk+-3.0\""));
    EXPECT_NOT_NULL(strstr(text, "answered = \"pkg-config\""));
    EXPECT_NOT_NULL(strstr(text, "version = \"3.24.33\""));
    free(text);
}

/* Omitted rather than written empty, like a path dependency's version: "" reads
   as a version that happens to be blank. */
MOLTEST(a_host_library_with_no_version_records_none) {
    const lock_host hosts[] = {one_host("toykit", "")};
    char *text = lockfile_render("ui", NULL, hosts, 1);
    ASSERT_NOT_NULL(text);

    /* Asserted on the entry and not the file: `version = 1` at the top is the
       lock's own format version and is always there. */
    const char *entry = strstr(text, "[[host]]");
    ASSERT_NOT_NULL(entry);
    EXPECT_NOT_NULL(strstr(entry, "capability = \"toykit\""));
    EXPECT_NULL(strstr(entry, "version ="));
    free(text);
}

/* A project with host libraries and no dependencies still gets a lock: what it
   borrowed from the machine is worth recording even when it borrowed no
   packages. */
MOLTEST(a_lock_with_only_host_entries_reads_back) {
    const lock_host hosts[] = {one_host("gtk+-3.0", "3.24.33"), one_host("zlib", "1.2.11")};
    char *text = lockfile_render("ui", NULL, hosts, 2);
    ASSERT_NOT_NULL(text);

    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    char path[128];
    snprintf(path, sizeof path, "%s/" LOCKFILE_NAME, at.root);
    ASSERT_TRUE(fs_write_file(path, text));
    free(text);

    lockfile lock;
    char err[512] = "";
    ASSERT_TRUE(lockfile_read(at.root, &lock, err, sizeof err));
    EXPECT_EQ(0u, lock.count);
    ASSERT_EQ(2u, lock.host_count);
    EXPECT_STREQ("gtk+-3.0", lock.hosts[0].capability);
    EXPECT_STREQ("3.24.33", lock.hosts[0].version);
    EXPECT_STREQ("zlib", lock.hosts[1].capability);

    lockfile_free(&lock);
    sandbox_close(&at);
}

/* Reported and never refused, which is the difference between this and
   lockfile_verify: a package resolving to other bytes is a registry rewriting
   history, and a host library on another version is Tuesday. */
MOLTEST(a_host_library_on_another_version_is_reported_not_refused) {
    lockfile lock = {0};
    lock_host recorded = one_host("gtk+-3.0", "3.24.30");
    lock.hosts = &recorded;
    lock.host_count = 1;

    const lock_host here[] = {one_host("gtk+-3.0", "3.24.33")};
    EXPECT_EQ(1u, lockfile_report_host_drift(&lock, here, 1));

    const lock_host same[] = {one_host("gtk+-3.0", "3.24.30")};
    EXPECT_EQ(0u, lockfile_report_host_drift(&lock, same, 1));
}

/* Nothing to say when either side knows no version: "it changed to unknown" is
   not a fact worth a line of a build's output. */
MOLTEST(a_host_library_with_no_version_on_either_side_is_quiet) {
    lockfile lock = {0};
    lock_host recorded = one_host("toykit", "");
    lock.hosts = &recorded;
    lock.host_count = 1;

    const lock_host here[] = {one_host("toykit", "2.0")};
    EXPECT_EQ(0u, lockfile_report_host_drift(&lock, here, 1));
}
