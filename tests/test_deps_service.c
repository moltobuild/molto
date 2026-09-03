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
    return moltest_temp_dir("molto_deps", at->root, sizeof at->root);
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

    /* One unit, because one package ships sources. */
    ASSERT_EQ(1u, deps.unit_count);
    EXPECT_STREQ("yyjson", deps.units[0].name);

    /* Only what the recipe named: tool.c brings its own main() and would fail
       the link. */
    ASSERT_EQ(1u, deps.units[0].sources.count);
    EXPECT_NOT_NULL(strstr(deps.units[0].sources.items[0], "yyjson.c"));

    /* Absolute, because the source is not under the project root.

       Asked with `fs_path_is_absolute` rather than by looking at the first
       byte: a Windows path opens with a drive and a colon, so `[0] == '/'` is
       this question asked in a way that is only right on one platform. */
    ASSERT_EQ(1u, deps.includes.count);
    EXPECT_TRUE(fs_path_is_absolute(deps.includes.items[0]));

    ASSERT_EQ(1u, deps.defines.count);
    EXPECT_STREQ("YYJSON_STATIC=1", deps.defines.items[0]);
    ASSERT_EQ(1u, deps.links.count);
    EXPECT_STREQ("m", deps.links.items[0]);

    /* Where the package sits, kept rather than only used to compose paths:
       it is what turns one of those absolute sources back into the name its
       own author would use. Every source is under it. */
    EXPECT_TRUE(fs_path_is_absolute(deps.units[0].root));
    EXPECT_EQ(0, strncmp(deps.units[0].sources.items[0], deps.units[0].root,
                         strlen(deps.units[0].root)));

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

/* A manifest whose dependency is named *relatively*, as a real one is: the
   fixtures above all write an absolute path, which is exactly why none of them
   ever exercised the anchoring. */
static bool parse_with_relative_dep(project_ctx *out, char *err, size_t err_size) {
    static const char *const manifest = "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                                        "[deps]\nyyjson = { path = \"yyjson\" }\n";
    return project_parse(manifest, out, err, err_size);
}

MOLTEST(deps_prepare_anchors_a_relative_path_at_the_project_root) {
    /* The working directory here is not the sandbox, so a path used as written
       cannot resolve. That is the whole bug: every command walks up to find its
       project, so running one from a subdirectory used to send the build
       looking for `modules/x` under `src/`. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, RECIPE));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_relative_dep(&ctx, err, sizeof err));
    snprintf(ctx.root, sizeof ctx.root, "%s", at.root);

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));
    EXPECT_STREQ("", err);

    ASSERT_EQ(1u, deps.unit_count);
    EXPECT_STREQ("yyjson", deps.units[0].name);
    /* Under the root it was anchored at, not under the working directory. */
    EXPECT_EQ(0, strncmp(deps.units[0].root, at.root, strlen(at.root)));

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

MOLTEST(deps_prepare_leaves_the_manifest_path_as_it_was_written) {
    /* Anchoring happens on the copy that opens the directory and never on what
       the manifest said, because what the manifest said is what the lock file
       records. An anchored one would write this machine's absolute path into a
       file that is committed and read by everyone else — a lock pinning a
       directory none of them have. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, RECIPE));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_relative_dep(&ctx, err, sizeof err));
    snprintf(ctx.root, sizeof ctx.root, "%s", at.root);

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));

    ASSERT_EQ(1u, ctx.deps.count);
    EXPECT_STREQ("yyjson", ctx.deps.items[0].location);

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

MOLTEST(deps_prepare_records_what_each_package_exports) {
    /* The sum of every package's interface is what a compile line needs, and it
       cannot answer which package asked for what. A document's `Dependency`
       node has to, so the export is recorded against the package as well —
       from the same table and the same pass, so the two cannot disagree. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, RECIPE));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));

    ASSERT_EQ(1u, deps.unit_count);
    const prepared_interface *exports = &deps.units[0].exports;

    ASSERT_EQ(1u, exports->includes.count);
    EXPECT_TRUE(fs_path_is_absolute(exports->includes.items[0])); /* absolute, as the sum's is */
    ASSERT_EQ(1u, exports->defines.count);
    EXPECT_STREQ("YYJSON_STATIC=1", exports->defines.items[0]);
    ASSERT_EQ(1u, exports->links.count);
    EXPECT_STREQ("m", exports->links.items[0]);

    /* With one package the sum is its export, entry for entry. That is the
       relationship being asserted, not a coincidence of this fixture: the sum
       is built by concatenating them. */
    ASSERT_EQ(exports->includes.count, deps.includes.count);
    EXPECT_STREQ(exports->includes.items[0], deps.includes.items[0]);
    ASSERT_EQ(exports->defines.count, deps.defines.count);
    EXPECT_STREQ(exports->defines.items[0], deps.defines.items[0]);
    ASSERT_EQ(exports->links.count, deps.links.count);
    EXPECT_STREQ(exports->links.items[0], deps.links.items[0]);

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
    ASSERT_EQ(1u, deps.unit_count);
    ASSERT_EQ(1u, deps.units[0].sources.count);
    EXPECT_NOT_NULL(strstr(deps.units[0].sources.items[0], "yyjson.c"));

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

/* A directory is not a translation unit, and left to pass through it reaches
   the compiler as an input — which reports it as an unused linker argument,
   naming neither the recipe nor the key that put it there. `sources` names
   files one by one because it fails closed (RFC-0009). */
MOLTEST(deps_prepare_reports_a_directory_where_a_source_was_named) {
    static const char *const directory = "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                         "name = \"yyjson\"\nversion = \"0.10.0\"\n"
                                         "target = \"any\"\n"
                                         "\n[artifacts]\ntype = \"source\"\n"
                                         "sources = [\"vendor\"]\n";
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, directory));

    char vendor[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(vendor, sizeof vendor, "%s/yyjson/vendor", at.root));
    ASSERT_TRUE(fs_make_dirs(vendor));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    EXPECT_FALSE(deps_prepare(&ctx, &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "vendor"));
    EXPECT_NOT_NULL(strstr(err, "directory"));

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
    EXPECT_EQ(0u, deps.unit_count);
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

/* --- what each dependency compiles against --- */

/* A package on disk under its own name: a recipe, and one source for it to
   name. Sibling directories in one sandbox are how a graph is built here. */
static bool make_named_package(const sandbox *at, const char *name, const char *recipe) {
    char dir[PATH_MAX_LEN];
    char file[PATH_MAX_LEN];
    if (!fs_format_path(dir, sizeof dir, "%s/%s", at->root, name) || !fs_make_dirs(dir))
        return false;
    if (!fs_format_path(file, sizeof file, "%s/recipe.toml", dir) || !fs_write_file(file, recipe))
        return false;
    if (!fs_format_path(file, sizeof file, "%s/%s.c", dir, name))
        return false;
    return fs_write_file(file, "int answer(void) { return 1; }\n");
}

/* A manifest depending on the named sibling packages by path. */
static bool parse_with_deps(const sandbox *at, const char *const *names, size_t count,
                            project_ctx *out, char *err, size_t err_size) {
    char manifest[PATH_MAX_LEN * 4];
    int used = snprintf(manifest, sizeof manifest,
                        "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[deps]\n");
    for (size_t i = 0; i < count; i++) {
        used += snprintf(manifest + used, sizeof manifest - (size_t)used,
                         "%s = { path = \"%s/%s\" }\n", names[i], at->root, names[i]);
    }
    return project_parse(manifest, out, err, err_size);
}

static const prepared_unit *unit_named(const prepared_deps *deps, const char *name) {
    for (size_t i = 0; i < deps->unit_count; i++) {
        if (strcmp(deps->units[i].name, name) == 0)
            return &deps->units[i];
    }
    return NULL;
}

/* True when any entry of `list` contains `text`. Substring rather than equality
   because an include directory comes out absolute, rooted in the sandbox. */
static bool mentions(const str_list *list, const char *text) {
    for (size_t i = 0; i < str_list_count(list); i++) {
        if (strstr(str_list_get(list, i), text) != NULL)
            return true;
    }
    return false;
}

/* The defect this whole thing exists to fix. Two dependencies that know nothing
   of each other used to share one set of flags, so a define one of them needed
   internally reached the other's preprocessor — and a warning one of them chose
   to silence was silenced for everybody. */
MOLTEST(one_dependency_does_not_compile_with_another_s_flags) {
    static const char *const alpha = "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                     "name = \"alpha\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                                     "[artifacts]\ntype = \"source\"\n"
                                     "include = [\".\"]\ndefines = [\"ALPHA_API=1\"]\n"
                                     "[artifacts.private]\nflags = [\"-Wno-alpha\"]\n"
                                     "defines = [\"ALPHA_INTERNAL\"]\n";
    static const char *const beta = "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                    "name = \"beta\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                                    "[artifacts]\ntype = \"source\"\n"
                                    "include = [\".\"]\ndefines = [\"BETA_API=1\"]\n"
                                    "[artifacts.private]\nflags = [\"-Wno-beta\"]\n";

    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_named_package(&at, "alpha", alpha));
    ASSERT_TRUE(make_named_package(&at, "beta", beta));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"alpha", "beta"};
    ASSERT_TRUE(parse_with_deps(&at, names, 2, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));
    ASSERT_EQ(2u, deps.unit_count);

    const prepared_unit *a = unit_named(&deps, "alpha");
    const prepared_unit *b = unit_named(&deps, "beta");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    /* Each compiles with its own private flag and not with its sibling's. */
    EXPECT_TRUE(mentions(&a->flags, "-Wno-alpha"));
    EXPECT_FALSE(mentions(&a->flags, "-Wno-beta"));
    EXPECT_TRUE(mentions(&b->flags, "-Wno-beta"));
    EXPECT_FALSE(mentions(&b->flags, "-Wno-alpha"));

    /* And with its own defines, interface and private alike — but not with the
       interface of a package it never named. */
    EXPECT_TRUE(mentions(&a->defines, "ALPHA_API=1"));
    EXPECT_TRUE(mentions(&a->defines, "ALPHA_INTERNAL"));
    EXPECT_FALSE(mentions(&a->defines, "BETA_API=1"));
    EXPECT_FALSE(mentions(&b->defines, "ALPHA_API=1"));
    EXPECT_FALSE(mentions(&b->defines, "ALPHA_INTERNAL"));

    /* The consumer sees both interfaces and neither private table. */
    EXPECT_TRUE(mentions(&deps.defines, "ALPHA_API=1"));
    EXPECT_TRUE(mentions(&deps.defines, "BETA_API=1"));
    EXPECT_FALSE(mentions(&deps.defines, "ALPHA_INTERNAL"));
    EXPECT_FALSE(mentions(&deps.flags, "-Wno-alpha"));
    EXPECT_FALSE(mentions(&deps.flags, "-Wno-beta"));

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

/* Isolation cannot mean isolation from what a package actually depends on: its
   sources include its dependency's headers, so that dependency's interface has
   to be on its command line. */
MOLTEST(a_dependency_compiles_against_what_it_reaches) {
    static const char *const inner = "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                     "name = \"inner\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                                     "[artifacts]\ntype = \"source\"\n"
                                     "include = [\".\"]\ndefines = [\"INNER_API=1\"]\n"
                                     "[artifacts.private]\ndefines = [\"INNER_INTERNAL\"]\n";
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_named_package(&at, "inner", inner));

    char outer[PATH_MAX_LEN * 2];
    snprintf(outer, sizeof outer,
             "schema = 1\nform = \"source\"\nkind = \"package\"\n"
             "name = \"outer\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
             "[artifacts]\ntype = \"source\"\ninclude = [\".\"]\n"
             "[deps]\ninner = { path = \"%s/inner\" }\n",
             at.root);
    ASSERT_TRUE(make_named_package(&at, "outer", outer));

    project_ctx ctx;
    char err[512] = "";
    const char *const names[] = {"outer"};
    ASSERT_TRUE(parse_with_deps(&at, names, 1, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));

    const prepared_unit *out = unit_named(&deps, "outer");
    const prepared_unit *in = unit_named(&deps, "inner");
    ASSERT_NOT_NULL(out);
    ASSERT_NOT_NULL(in);

    /* Down the edge: outer compiles against inner's interface. */
    EXPECT_TRUE(mentions(&out->defines, "INNER_API=1"));
    EXPECT_TRUE(mentions(&out->includes, "/inner"));
    /* And not against what inner kept to itself. */
    EXPECT_FALSE(mentions(&out->defines, "INNER_INTERNAL"));

    /* Never up it: inner was resolved before outer existed as far as its own
       sources are concerned. */
    EXPECT_FALSE(mentions(&in->includes, "/outer"));

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

/* The standard travels from the recipe to the unit that compiles that package,
   and each language is decided on its own: a C library with one C++ shim names
   `std` and lets `cpp_std` be whatever the consumer compiles with. */
MOLTEST(a_unit_carries_the_standard_its_recipe_named) {
    static const char *const legacy = "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                      "name = \"yyjson\"\nversion = \"0.10.0\"\n"
                                      "target = \"any\"\n"
                                      "\n[artifacts]\ntype = \"source\"\n"
                                      "sources = [\"yyjson.c\"]\ninclude = [\".\"]\n"
                                      "std = \"c99\"\n";
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, legacy));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));

    ASSERT_EQ(1u, deps.unit_count);
    EXPECT_STREQ("c99", deps.units[0].std);
    /* Unnamed, so the consumer's applies. */
    EXPECT_STREQ("", deps.units[0].cpp_std);

    prepared_deps_free(&deps);
    sandbox_close(&at);
}

/* And a recipe that names none leaves both empty, which is how every package
   behaved before the keys existed. */
MOLTEST(a_unit_whose_recipe_named_no_standard_inherits_the_consumers) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    ASSERT_TRUE(make_dependency(&at, RECIPE));

    project_ctx ctx;
    char err[512] = "";
    ASSERT_TRUE(parse_with_dep(&at, &ctx, err, sizeof err));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_TRUE(deps_prepare(&ctx, &deps, err, sizeof err));

    ASSERT_EQ(1u, deps.unit_count);
    EXPECT_STREQ("", deps.units[0].std);
    EXPECT_STREQ("", deps.units[0].cpp_std);

    prepared_deps_free(&deps);
    sandbox_close(&at);
}
