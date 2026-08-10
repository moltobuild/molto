#include <moltest.h>

#include <molto/project/project_ctx.h>
#include <molto/project/project_deps.h>
#include <molto/util/doc.h>
#include <molto/util/json.h>
#include <molto/util/toml.h>

#include <stdio.h>
#include <string.h>

/* Reads a [deps] table out of manifest text. */
static bool read_deps(const char *text, project_deps *out, char *err, size_t err_size) {
    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    if (doc == NULL) {
        snprintf(err, err_size, "%s", parse_err);
        return false;
    }
    const bool ok = project_deps_read(doc, out, err, err_size);
    toml_free(doc);
    return ok;
}

/* The one dependency this whole chain exists for. The amalgamation lives on
   sqlite.org, not on git — so the manifest names a coordinate, and the URL and
   digest live in the recipe the registry serves. */
MOLTEST(deps_read_the_registry_shorthand) {
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[deps]\nsqlite = \"3.53.4\"\n", &deps, err, sizeof err));

    ASSERT_EQ(1u, deps.count);
    EXPECT_STREQ("sqlite", deps.items[0].name);
    EXPECT_EQ(dep_source_version, deps.items[0].source);
    EXPECT_EQ(dep_resolution_registry, deps.items[0].resolution);
    EXPECT_STREQ("3.53.4", deps.items[0].version);
    EXPECT_STREQ("", deps.items[0].registry);
}

MOLTEST(deps_read_an_inline_table_with_git_and_tag) {
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[deps]\n"
                          "yyjson = { git = \"https://github.com/ibireme/yyjson\", tag = \"0.10.0\" }\n",
                          &deps, err, sizeof err));

    ASSERT_EQ(1u, deps.count);
    const project_dep *dep = &deps.items[0];
    EXPECT_STREQ("yyjson", dep->name);
    EXPECT_EQ(dep_source_git, dep->source);
    /* Carried: the manifest already says where the bytes are, so no registry
       is asked and source_fetch can run straight away. */
    EXPECT_EQ(dep_resolution_carried, dep->resolution);
    EXPECT_STREQ("https://github.com/ibireme/yyjson", dep->location);
    EXPECT_STREQ("0.10.0", dep->reference);
    EXPECT_EQ(dep_git_ref_tag, dep->git_ref);
}

MOLTEST(deps_convert_a_carried_source_for_the_fetcher) {
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[deps]\nyyjson = { git = \"https://x/y.git\", rev = \"abc123\" }\n",
                          &deps, err, sizeof err));

    source_spec spec;
    ASSERT_TRUE(project_dep_to_source(&deps.items[0], &spec, err, sizeof err));
    EXPECT_EQ(source_origin_git, spec.origin);
    EXPECT_STREQ("https://x/y.git", spec.location);
    EXPECT_STREQ("abc123", spec.reference);
}

MOLTEST(deps_refuse_to_hand_a_coordinate_to_the_fetcher) {
    /* A version dependency has no source until a registry answers, and saying
       so beats fetching an empty URL. */
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[deps]\nsqlite = \"3.53.4\"\n", &deps, err, sizeof err));

    source_spec spec;
    EXPECT_FALSE(project_dep_to_source(&deps.items[0], &spec, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "registry"));
}

MOLTEST(deps_treat_the_shorthand_and_the_table_form_as_equal) {
    /* RFC-0003: `dep = "1.2.3"` is exactly `dep = { version = "1.2.3" }`. */
    project_deps shorthand;
    project_deps table;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[deps]\nyyjson = \"1.2.3\"\n", &shorthand, err, sizeof err));
    ASSERT_TRUE(read_deps("[deps]\nyyjson = { version = \"1.2.3\" }\n", &table, err, sizeof err));

    ASSERT_EQ(1u, shorthand.count);
    ASSERT_EQ(1u, table.count);
    EXPECT_EQ(shorthand.items[0].source, table.items[0].source);
    EXPECT_EQ(shorthand.items[0].resolution, table.items[0].resolution);
    EXPECT_STREQ(shorthand.items[0].version, table.items[0].version);
}

MOLTEST(deps_read_a_header_exactly_as_an_inline_table) {
    project_deps inline_form;
    project_deps header_form;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[deps]\nyyjson = { git = \"https://x/y.git\", tag = \"v1\" }\n",
                          &inline_form, err, sizeof err));
    ASSERT_TRUE(read_deps("[deps.yyjson]\ngit = \"https://x/y.git\"\ntag = \"v1\"\n", &header_form,
                          err, sizeof err));

    ASSERT_EQ(1u, inline_form.count);
    ASSERT_EQ(1u, header_form.count);
    EXPECT_EQ(0, memcmp(&inline_form.items[0], &header_form.items[0], sizeof(project_dep)));
}

MOLTEST(deps_keep_declaration_order_across_both_forms) {
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[deps]\n"
                          "yyjson = \"1.0.0\"\n"
                          "sqlite = { version = \"3.53.4\" }\n"
                          "http = { path = \"modules/http\" }\n",
                          &deps, err, sizeof err));

    ASSERT_EQ(3u, deps.count);
    EXPECT_STREQ("yyjson", deps.items[0].name);
    EXPECT_STREQ("sqlite", deps.items[1].name);
    EXPECT_STREQ("http", deps.items[2].name);
}

MOLTEST(deps_read_every_source_a_manifest_may_name) {
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[deps]\n"
                          "local = { path = \"modules/local\" }\n"
                          "tarred = { archive = \"https://x/y.tar.gz\", sha256 = "
                          "\"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\", "
                          "strip_prefix = \"y-1.0\" }\n",
                          &deps, err, sizeof err));

    ASSERT_EQ(2u, deps.count);
    EXPECT_EQ(dep_source_path, deps.items[0].source);
    /* A path is whatever is on disk right now, which is the point of it, so it
       carries no digest. */
    EXPECT_STREQ("", deps.items[0].sha256);

    EXPECT_EQ(dep_source_archive, deps.items[1].source);
    EXPECT_STREQ("y-1.0", deps.items[1].strip_prefix);
}

MOLTEST(deps_reject_a_version_range) {
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps("[deps]\nsqlite = \"^3.53.0\"\n", &deps, err, sizeof err));

    /* The message names the operator, the dependency and the reason. */
    EXPECT_NOT_NULL(strstr(err, "sqlite"));
    EXPECT_NOT_NULL(strstr(err, "'^'"));
    EXPECT_NOT_NULL(strstr(err, "RFC-0008"));
}

MOLTEST(deps_reject_a_range_in_the_table_form_too) {
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps("[deps]\nsqlite = { version = \">=3.0.0\" }\n", &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "'>='"));
}

MOLTEST(deps_reject_a_dependency_with_no_source) {
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps("[deps]\nsqlite = { registry = \"myorg\" }\n", &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "no source"));
}

MOLTEST(deps_reject_two_sources) {
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps("[deps]\nx = { git = \"https://x/y.git\", path = \"vendor/x\" }\n",
                           &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "more than one source"));
}

MOLTEST(deps_reject_two_git_references) {
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps("[deps]\nx = { git = \"https://x/y.git\", tag = \"v1\", rev = \"a\" }\n",
                           &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "branch, tag and rev"));
}

MOLTEST(deps_reject_a_git_reference_without_a_repository) {
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps("[deps]\nx = { path = \"vendor/x\", tag = \"v1\" }\n", &deps, err,
                           sizeof err));
    EXPECT_NOT_NULL(strstr(err, "no git repository"));
}

MOLTEST(deps_reject_an_archive_without_a_digest) {
    /* A URL promises a location, not content (RFC-0008). */
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps("[deps]\nx = { archive = \"https://x/y.tar.gz\" }\n", &deps, err,
                           sizeof err));
    EXPECT_NOT_NULL(strstr(err, "sha256"));
}

MOLTEST(deps_reject_an_unknown_key) {
    /* The typo case: without this, `tags` is dropped and the dependency
       silently resolves to the repository's default branch. */
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps("[deps]\nx = { git = \"https://x/y.git\", tags = \"v1\" }\n", &deps, err,
                           sizeof err));
    EXPECT_NOT_NULL(strstr(err, "unknown key 'tags'"));
}

MOLTEST(deps_reject_a_key_that_is_not_supported_yet) {
    /* Following [package].artifact: accepting a key and doing nothing with it
       tells the user their manifest said something it did not. */
    static const char *const pending[] = { "recipe = \"png\"", "artifact = \"static\"",
                                           "optional = true", "features = [\"a\"]",
                                           "default_features = false" };

    for (size_t i = 0; i < sizeof pending / sizeof pending[0]; i++) {
        char text[256];
        snprintf(text, sizeof text, "[deps]\nx = { git = \"https://x/y.git\", %s }\n", pending[i]);

        project_deps deps;
        char err[512] = "";
        EXPECT_FALSE(read_deps(text, &deps, err, sizeof err));
        EXPECT_NOT_NULL(strstr(err, "not supported yet"));
    }
}

MOLTEST(deps_reject_a_name_that_is_not_a_package_name) {
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps("[deps]\nSQLite = \"3.53.4\"\n", &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "not a package name"));
}

MOLTEST(deps_absent_table_is_not_an_error) {
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[package]\nname = \"x\"\n", &deps, err, sizeof err));
    EXPECT_EQ(0u, deps.count);
}

MOLTEST(deps_find_answers_by_name) {
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps("[deps]\nsqlite = \"3.53.4\"\nyyjson = \"1.0.0\"\n", &deps, err,
                          sizeof err));

    const project_dep *found = project_deps_find(&deps, "yyjson");
    ASSERT_NOT_NULL(found);
    EXPECT_STREQ("1.0.0", found->version);
    EXPECT_NULL(project_deps_find(&deps, "nothing"));
}

/* --- [registries] --- */

MOLTEST(registries_are_read_and_selectable_by_a_dependency) {
    char err[512] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                              "[registries]\nmyorg = \"https://pkgs.myorg.dev\"\n"
                              "[deps]\nsqlite = { version = \"3.53.4\", registry = \"myorg\" }\n",
                              &ctx, err, sizeof err));

    ASSERT_EQ(1u, ctx.registries.count);
    EXPECT_STREQ("https://pkgs.myorg.dev", project_registries_url(&ctx.registries, "myorg"));
    ASSERT_EQ(1u, ctx.deps.count);
    EXPECT_STREQ("myorg", ctx.deps.items[0].registry);
}

MOLTEST(a_registry_nobody_declared_is_an_error_and_not_a_fallback) {
    /* Falling back to the official registry would resolve the dependency
       against somewhere the manifest never named. */
    char err[512] = "";
    project_ctx ctx;
    EXPECT_FALSE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                               "[deps]\nsqlite = { version = \"3.53.4\", registry = \"myorg\" }\n",
                               &ctx, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "myorg"));
}

/* --- through the manifest --- */

MOLTEST(project_ctx_carries_the_deps) {
    char err[512] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                              "[target]\nstd = \"c17\"\n"
                              "[deps]\nsqlite = \"3.53.4\"\n",
                              &ctx, err, sizeof err));

    ASSERT_EQ(1u, ctx.deps.count);
    EXPECT_STREQ("sqlite", ctx.deps.items[0].name);
    EXPECT_STREQ("c17", ctx.target.std);
}

MOLTEST(a_manifest_whose_deps_are_all_tables_is_read_whole) {
    /* The regression that mattered: an inline table stores its members under
       "deps.<name>" and nothing under "deps", so a reader built on
       toml_section_keys saw none of these and reported no error. */
    char err[512] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                              "[deps]\n"
                              "a = { git = \"https://x/a.git\" }\n"
                              "b = { path = \"modules/b\" }\n",
                              &ctx, err, sizeof err));

    ASSERT_EQ(2u, ctx.deps.count);
    EXPECT_STREQ("a", ctx.deps.items[0].name);
    EXPECT_STREQ("b", ctx.deps.items[1].name);
}

MOLTEST(a_bad_dependency_fails_the_whole_manifest) {
    char err[512] = "";
    project_ctx ctx;
    EXPECT_FALSE(project_parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                               "[deps]\nsqlite = \"~3.53.0\"\n",
                               &ctx, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "'~'"));
}

/* --- the same reader, on the encoding a registry answers with (RFC-0010) --- */

/* Reads a [deps] table out of a recipe served as JSON, the way a registry
   serves one inside an artifact's metadata. */
static bool read_deps_json(const char *text, project_deps *out, char *err, size_t err_size) {
    json_document *doc = json_parse(text);
    if (doc == NULL) {
        snprintf(err, err_size, "not JSON");
        return false;
    }
    const bool ok = project_deps_read_doc(doc_from_json(json_root(doc)), out, err, err_size);
    json_free(doc);
    return ok;
}

/* Discovering a transitive dependency means reading the [deps] of a recipe that
   never was a file. Both spellings have to survive the crossing, or the graph
   depends on which encoding a dependency happened to arrive in. */
MOLTEST(deps_read_the_same_from_a_recipe_served_as_json) {
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps_json("{\"kind\":\"package\",\"deps\":{"
                               "\"yyjson\":\"0.10.0\","
                               "\"png\":{\"git\":\"https://x/png.git\",\"tag\":\"v1.6.40\"}"
                               "}}",
                               &deps, err, sizeof err));

    ASSERT_EQ(2u, deps.count);
    EXPECT_STREQ("yyjson", deps.items[0].name);
    EXPECT_EQ(dep_resolution_registry, deps.items[0].resolution);
    EXPECT_STREQ("0.10.0", deps.items[0].version);

    EXPECT_STREQ("png", deps.items[1].name);
    EXPECT_EQ(dep_source_git, deps.items[1].source);
    EXPECT_EQ(dep_resolution_carried, deps.items[1].resolution);
    EXPECT_STREQ("https://x/png.git", deps.items[1].location);
    EXPECT_EQ(dep_git_ref_tag, deps.items[1].git_ref);
    EXPECT_STREQ("v1.6.40", deps.items[1].reference);
}

/* A recipe with no [deps] is a leaf, not a failure: most of them are. */
MOLTEST(a_recipe_without_deps_is_a_leaf) {
    project_deps deps;
    char err[512] = "";
    ASSERT_TRUE(read_deps_json("{\"kind\":\"package\",\"name\":\"sqlite\"}", &deps, err, sizeof err));
    EXPECT_EQ(0u, deps.count);
}

/* The rules are the reader's, not the manifest's: a range published in a recipe
   is refused exactly as one written by hand. */
MOLTEST(a_range_in_a_recipe_is_refused_too) {
    project_deps deps;
    char err[512] = "";
    EXPECT_FALSE(read_deps_json("{\"deps\":{\"png\":\">=1.6.0\"}}", &deps, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "'>='"));
}
