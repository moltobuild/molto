#include <moltest.h>

#include <molto/services/resolve_service.h>
#include <molto/services/fs_service.h>
#include <molto/util/semver.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Everything a resolution can get wrong, against canned bodies.
 *
 * The bodies below are what molto_registry actually answers with for
 * `GET /v1/packages/{name}/{version}` — a release envelope whose `targets`
 * each carry the whole recipe in `metadata`. No network is involved, which is
 * the point: choosing a target, refusing a yanked artifact and reading the
 * recipe are decisions, and decisions are worth testing without a server. */

#define RECIPE                                                                                     \
    "\"metadata\":{\"schema\":1,\"form\":\"source\",\"kind\":\"package\","                          \
    "\"name\":\"sqlite\",\"version\":\"3.53.4\",\"target\":\"any\","                                \
    "\"source\":{\"archive\":\"https://sqlite.org/2026/sqlite-amalgamation-3530400.zip\","          \
    "\"sha256\":\"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\","              \
    "\"strip_prefix\":\"sqlite-amalgamation-3530400\"},"                                            \
    "\"build\":{\"system\":\"none\"},"                                                              \
    "\"artifacts\":{\"type\":\"source\",\"sources\":[\"sqlite3.c\"],\"include\":[\".\"],"           \
    "\"link\":[\"m\",\"dl\",\"pthread\"],\"defines\":[\"SQLITE_THREADSAFE=1\"]}}"

#define ENVELOPE(targets)                                                                          \
    "{\"kind\":\"package\",\"name\":\"sqlite\",\"version\":\"3.53.4\",\"targets\":[" targets "]}"

#define SOURCE_TARGET                                                                              \
    "{\"kind\":\"package\",\"form\":\"source\",\"name\":\"sqlite\",\"version\":\"3.53.4\","         \
    "\"target\":\"any\",\"format\":null,\"checksum\":null,\"size_bytes\":null,"                     \
    "\"yanked\":false,\"published_at\":\"2026-08-07T00:00:00Z\",\"download_url\":null," RECIPE "}"

static bool resolve(const char *body, resolved_dep *out, char *err, size_t err_size) {
    return resolve_read_release(body, "sqlite", "3.53.4", out, err, err_size);
}

MOLTEST(resolve_reads_a_source_recipe_from_a_release_body) {
    /* The whole of C3 in one assertion set: a coordinate goes in, and what
       comes out is what the fetcher takes and what a compile line needs. */
    resolved_dep dep;
    char err[512] = "";
    ASSERT_TRUE(resolve(ENVELOPE(SOURCE_TARGET), &dep, err, sizeof err));

    EXPECT_EQ(recipe_form_source, dep.coordinate.form);
    EXPECT_STREQ("sqlite", dep.coordinate.name);
    EXPECT_STREQ("3.53.4", dep.coordinate.version);

    /* The [source] the fetcher takes — the URL lives in the recipe, not in
       anybody's manifest. */
    EXPECT_EQ(source_origin_archive, dep.source.origin);
    EXPECT_STREQ("https://sqlite.org/2026/sqlite-amalgamation-3530400.zip", dep.source.location);
    EXPECT_STREQ("sqlite-amalgamation-3530400", dep.source.strip_prefix);
    EXPECT_STREQ("1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d",
                 dep.source.sha256);

    /* And the [artifacts] a build needs. */
    EXPECT_EQ(recipe_artifact_source, dep.artifacts.type);
    ASSERT_EQ(1u, dep.artifacts.source_count);
    EXPECT_STREQ("sqlite3.c", dep.artifacts.sources[0]);
    ASSERT_EQ(3u, dep.artifacts.link_count);
    ASSERT_EQ(1u, dep.artifacts.options.include_count);
    EXPECT_STREQ(".", dep.artifacts.options.include[0]);

    /* A source recipe has no bytes in the registry, so nothing to download. */
    EXPECT_STREQ("", dep.download_url);
}

MOLTEST(resolve_prefers_the_any_target) {
    static const char *const body =
        ENVELOPE("{\"target\":\"x86_64-linux-gnu\",\"yanked\":false,\"metadata\":{}},"
                 SOURCE_TARGET);

    resolved_dep dep;
    char err[512] = "";
    ASSERT_TRUE(resolve(body, &dep, err, sizeof err));
    EXPECT_STREQ("any", dep.coordinate.target);
}

MOLTEST(resolve_skips_a_yanked_artifact) {
    /* Yanked stays resolvable so old builds keep working, and must not be
       chosen for a new one. */
    static const char *const body = ENVELOPE(
        "{\"target\":\"any\",\"yanked\":true,\"metadata\":{}}");

    resolved_dep dep;
    char err[512] = "";
    EXPECT_FALSE(resolve(body, &dep, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "yanked"));
}

MOLTEST(resolve_reports_when_only_platform_targets_exist) {
    /* Actionable: it names what was published, instead of a bare not-found
       that cannot be told from a version that does not exist. */
    static const char *const body =
        ENVELOPE("{\"target\":\"x86_64-linux-gnu\",\"yanked\":false,\"metadata\":{}},"
                 "{\"target\":\"aarch64-darwin\",\"yanked\":false,\"metadata\":{}}");

    resolved_dep dep;
    char err[512] = "";
    EXPECT_FALSE(resolve(body, &dep, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "x86_64-linux-gnu"));
    EXPECT_NOT_NULL(strstr(err, "aarch64-darwin"));
}

MOLTEST(resolve_reports_an_empty_target_list) {
    resolved_dep dep;
    char err[512] = "";
    EXPECT_FALSE(resolve(ENVELOPE(""), &dep, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "no targets"));
}

MOLTEST(resolve_reports_a_body_that_is_not_json) {
    resolved_dep dep;
    char err[512] = "";
    EXPECT_FALSE(resolve("<html>502 Bad Gateway</html>", &dep, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "not JSON"));
}

MOLTEST(resolve_reports_an_answer_with_no_targets_key) {
    resolved_dep dep;
    char err[512] = "";
    EXPECT_FALSE(resolve("{\"kind\":\"package\",\"name\":\"sqlite\"}", &dep, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "'targets'"));
}

MOLTEST(resolve_reports_an_artifact_with_no_recipe) {
    static const char *const body = ENVELOPE("{\"target\":\"any\",\"yanked\":false}");

    resolved_dep dep;
    char err[512] = "";
    EXPECT_FALSE(resolve(body, &dep, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "no recipe"));
}

MOLTEST(resolve_rejects_a_recipe_that_describes_something_else) {
    /* The integrity check: the registry is a remote party, and a client that
       caches whatever came back under the coordinate it asked for has no story
       at all for a registry that answers wrongly. */
    static const char *const body =
        ENVELOPE("{\"target\":\"any\",\"yanked\":false,"
                 "\"metadata\":{\"kind\":\"package\",\"name\":\"sqlite\","
                 "\"version\":\"3.53.5\",\"target\":\"any\"}}");

    resolved_dep dep;
    char err[512] = "";
    EXPECT_FALSE(resolve(body, &dep, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "3.53.5"));
}

MOLTEST(resolve_reads_a_binary_artifacts_url_and_checksum) {
    static const char *const body = ENVELOPE(
        "{\"target\":\"any\",\"yanked\":false,"
        "\"checksum\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"download_url\":\"https://registry.test/v1/packages/sqlite/3.53.4/any/download\","
        "\"metadata\":{\"schema\":1,\"form\":\"binary\",\"kind\":\"package\","
        "\"name\":\"sqlite\",\"version\":\"3.53.4\",\"target\":\"any\","
        "\"package\":{\"include\":[\"include\"],\"link\":[\"sqlite3\"]}}}");

    resolved_dep dep;
    char err[512] = "";
    ASSERT_TRUE(resolve(body, &dep, err, sizeof err));

    EXPECT_EQ(recipe_form_binary, dep.coordinate.form);
    EXPECT_STREQ("https://registry.test/v1/packages/sqlite/3.53.4/any/download", dep.download_url);
    EXPECT_STREQ("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", dep.checksum);
    /* No [source] is read for a binary recipe: there is nothing to fetch from
       upstream, the bytes are in the registry. */
    EXPECT_STREQ("", dep.source.location);
}

MOLTEST(resolve_reports_a_source_recipe_whose_source_table_is_broken) {
    /* A recipe that reached the registry before its validator did, or one
       served by a registry somebody else implemented. */
    static const char *const body =
        ENVELOPE("{\"target\":\"any\",\"yanked\":false,"
                 "\"metadata\":{\"schema\":1,\"form\":\"source\",\"kind\":\"package\","
                 "\"name\":\"sqlite\",\"version\":\"3.53.4\",\"target\":\"any\","
                 "\"source\":{\"archive\":\"https://x/y.zip\"}}}");

    resolved_dep dep;
    char err[512] = "";
    EXPECT_FALSE(resolve(body, &dep, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "sha256"));
}

MOLTEST(resolve_reports_a_registry_it_cannot_reach) {
    resolved_dep dep;
    char err[512] = "";
    EXPECT_FALSE(resolve_version("http://127.0.0.1:1", "sqlite", "3.53.4", &dep, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "registry"));
}

/* --- the catalogue listing --- */

#define LISTING(releases)                                                                          \
    "{\"kind\":\"package\",\"name\":\"png\",\"releases\":[" releases "]}"

static size_t versions_of(const char *body, str_list *out) {
    char err[256] = "";
    str_list_init(out);
    return resolve_read_versions(body, out, err, sizeof err) ? str_list_count(out) : 0;
}

MOLTEST(a_listing_yields_every_version_it_names) {
    str_list versions;
    ASSERT_EQ(3u, versions_of(LISTING("{\"version\":\"1.5.30\"},{\"version\":\"1.6.40\"},"
                                      "{\"version\":\"1.6.39\"}"),
                              &versions));

    EXPECT_STREQ("1.5.30", str_list_get(&versions, 0));
    str_list_free(&versions);
}

/* Order is the client's job: the registry serves versions as opaque strings in
   publication order, so the newest one is decided here. */
MOLTEST(a_listing_orders_newest_first_once_sorted) {
    str_list versions;
    ASSERT_EQ(3u, versions_of(LISTING("{\"version\":\"1.5.30\"},{\"version\":\"1.6.40\"},"
                                      "{\"version\":\"1.6.39\"}"),
                              &versions));

    EXPECT_EQ(3u, semver_sort_desc(&versions));
    EXPECT_STREQ("1.6.40", str_list_get(&versions, 0));
    EXPECT_STREQ("1.6.39", str_list_get(&versions, 1));
    EXPECT_STREQ("1.5.30", str_list_get(&versions, 2));
    str_list_free(&versions);
}

MOLTEST(a_body_that_is_not_a_listing_is_refused_with_a_reason) {
    str_list versions;
    str_list_init(&versions);
    char err[256] = "";

    EXPECT_FALSE(resolve_read_versions("{\"kind\":\"package\"}", &versions, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "releases"));

    EXPECT_FALSE(resolve_read_versions("not json", &versions, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "JSON"));
    str_list_free(&versions);
}

/* --- remembering an answer without fetching one --- */

MOLTEST(a_release_is_remembered_for_a_coordinate_nothing_fetched) {
    /* The whole point of the releases tree: a metadata walk learns what a
       version depends on without downloading it, and must be able to keep
       that. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_release", root, sizeof root));
    char cache[128] = "";
    snprintf(cache, sizeof cache, "%s/cache", root);
    ASSERT_EQ(0, setenv("MOLTO_CACHE", cache, 1));

    EXPECT_NULL(resolve_release_body("png", "1.6.40"));

    resolve_remember("png", "1.6.40", ENVELOPE(SOURCE_TARGET));
    char *body = resolve_release_body("png", "1.6.40");
    ASSERT_NOT_NULL(body);
    EXPECT_NOT_NULL(strstr(body, "sqlite-amalgamation-3530400"));
    free(body);

    /* Not under the sources, which a fetch replaces wholesale. */
    char sources[512] = "";
    ASSERT_TRUE(source_cache_path("png", "1.6.40", "any", sources, sizeof sources));
    EXPECT_FALSE(fs_path_exists(sources));

    (void)unsetenv("MOLTO_CACHE");
    (void)fs_remove_tree(root);
}
