#include <moltest.h>
#include <moltest_mock.h>

#include <molto/project/project_ctx.h>
#include <molto/project/project_deps.h>
#include <molto/services/dep_graph.h>
#include <molto/services/resolve_service.h>
#include <molto/services/source_service.h>
#include <molto/util/str_list.h>
#include <molto/util/toml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The resolution of registry dependencies, with the registry and the fetcher
 * faked.
 *
 * An isolated test (RFC-0021): it replaces src/services/resolve_service.c and
 * src/services/source_service.c. A small registry lives in the table below —
 * what each package publishes and what each release depends on — so the
 * walk, the deferred downloads and the search for a way out of a conflict can
 * be followed without a network, and every download counted.
 */

MOCK_VALUE_FUNC(bool, resolve_version, const char *, const char *, const char *, resolved_dep *,
                char *, size_t);
MOCK_VALUE_FUNC(bool, resolve_versions, const char *, const char *, str_list *, char *, size_t);
MOCK_VALUE_FUNC(bool, resolve_remembered, const char *, const char *, resolved_dep *);
MOCK_VOID_FUNC(resolve_remember, const char *, const char *, const char *);
/* No release here is a platform recipe, so there is never one to free. */
MOCK_VOID_FUNC(resolved_dep_release, resolved_dep *);
MOCK_VALUE_FUNC(bool, source_fetch, const source_spec *, const char *, const char *, const char *,
                char *, size_t, char *, size_t);
MOCK_VALUE_FUNC(bool, source_provide, const char *, const struct recipe_provide *, char *, size_t);
MOCK_VALUE_FUNC(bool, source_overlay, const char *, const struct recipe_overlay *, char *, size_t);
/* Read only from a recipe a path or git source carries, which nothing here
   is; the linker takes dep_graph.c whole. */
MOCK_VALUE_FUNC(bool, source_read, doc_view, source_spec *, char *, size_t);
MOCK_VALUE_FUNC(bool, source_cache_key, const source_spec *, char *, size_t, char *, size_t);
/* Reached only through platform_service, which nothing here resolves to. */
MOCK_VALUE_FUNC(bool, source_cache_root, char *, size_t);
/* Never about a registry dependency, but src/project/project_deps.c checks a
   git or archive origin with it, and the linker takes that file whole. Every
   origin here is valid. */
MOCK_VALUE_FUNC(bool, source_spec_validate, const source_spec *, char *, size_t);

/* --- the registry --- */

typedef struct {
    const char *name;
    const char *version;
    const char *deps; /* the release's [deps], as a recipe would write them */
    bool prebuilt;
} release;

static const release *published;
static size_t published_count;

static const release *find(const char *name, const char *version) {
    for(size_t i = 0; i < published_count; i++) {
        if(strcmp(published[i].name, name) == 0 && strcmp(published[i].version, version) == 0)
            return &published[i];
    }
    return NULL;
}

static bool read_release(const release *r, resolved_dep *out) {
    memset(out, 0, sizeof *out);
    out->coordinate.form = r->prebuilt ? recipe_form_binary : recipe_form_source;
    snprintf(out->coordinate.name, sizeof out->coordinate.name, "%s", r->name);
    snprintf(out->coordinate.version, sizeof out->coordinate.version, "%s", r->version);
    snprintf(out->coordinate.target, sizeof out->coordinate.target, "any");
    snprintf(out->body, sizeof out->body, "{\"release\":\"%s %s\"}", r->name, r->version);
    char text[256];
    snprintf(text, sizeof text, "[deps]\n%s", r->deps != NULL ? r->deps : "");
    char err[256] = "";
    toml_document *doc = toml_parse(text, err, sizeof err);
    const bool ok = doc != NULL && project_deps_read(doc, &out->deps, err, sizeof err);
    toml_free(doc);
    return ok;
}

/* Releases of png 2.x asked for, counted during the call: the mock's history
   keeps the pointers it was given, and these point into copies of the
   manifest that are gone by the time a test could read them. */
static size_t png_2_asked;

static bool registry_has(const char *base_url, const char *name, const char *version,
                         resolved_dep *out, char *err, size_t err_size) {
    (void)base_url;
    if(strcmp(name, "png") == 0 && strncmp(version, "2.", 2) == 0)
        png_2_asked++;
    const release *r = find(name, version);
    if(r == NULL) {
        snprintf(err, err_size, "%s %s is not in the registry", name, version);
        return false;
    }
    return read_release(r, out);
}

/* Every version of `name` the table publishes, newest first as the real one
   orders them: the table lists them that way. */
/* What a previous build kept on disk: the same answer, without asking. */
static bool remembered(const char *name, const char *version, resolved_dep *out) {
    const release *r = find(name, version);
    return r != NULL && read_release(r, out);
}

static bool registry_lists(const char *base_url, const char *name, str_list *out, char *err,
                           size_t err_size) {
    (void)base_url, (void)err, (void)err_size;
    for(size_t i = 0; i < published_count; i++) {
        if(strcmp(published[i].name, name) == 0 && !str_list_push(out, published[i].version))
            return false;
    }
    return str_list_count(out) > 0;
}

/* Fetches nothing: says where the sources would be. */
static bool fetched_into(const source_spec *spec, const char *name, const char *version,
                         const char *target, char *out, size_t out_size, char *err,
                         size_t err_size) {
    (void)spec, (void)target, (void)err, (void)err_size;
    snprintf(out, out_size, "/fetched/%s-%s", name, version);
    return true;
}

BEFORE_EACH() {
    published = NULL;
    published_count = 0;
    png_2_asked = 0;
    resolve_version_mock.custom_fake = registry_has;
    resolve_versions_mock.custom_fake = registry_lists;
    source_fetch_mock.custom_fake = fetched_into;
    source_provide_mock.return_val = true;
    source_overlay_mock.return_val = true;
    source_spec_validate_mock.return_val = true;
}

#define PUBLISH(table)                                                                             \
    do {                                                                                           \
        published = (table);                                                                       \
        published_count = sizeof(table) / sizeof(table)[0];                                        \
    } while(0)

static project_ctx ctx;
static dep_graph *graph;
static dep_conflict conflict;
static char err[512];

static bool resolve(const char *deps, bool propose) {
    char manifest[512];
    snprintf(manifest, sizeof manifest, "[package]\nname = \"app\"\n[deps]\n%s", deps);
    char parse_err[256] = "";
    if(!project_parse(manifest, &ctx, parse_err, sizeof parse_err))
        return false;
    graph = NULL;
    memset(&conflict, 0, sizeof conflict);
    err[0] = '\0';
    const dep_resolve_options options = {.propose = propose};
    return dep_graph_resolve_with(&ctx, &options, &graph, &conflict, err, sizeof err);
}

/* --- walking --- */

DESCRIBE(a_registry_dependency_brings_what_its_release_depends_on) {
    static const release table[] = {
        {"png", "1.6.0", "zlib = \"1.3.1\"\n", false},
        {"zlib", "1.3.1", NULL, false},
    };
    PUBLISH(table);
    ASSERT_TRUE(resolve("png = \"1.6.0\"\n", false));
    ASSERT_EQ(2, (int)dep_graph_count(graph));
    const dep_node *zlib = dep_graph_find(graph, "zlib");
    ASSERT_NOT_NULL(zlib);
    EXPECT_STREQ("1.3.1", zlib->version);
    EXPECT_STREQ("png", zlib->required_by);
    /* Fetched once the graph was known, each into its own root. */
    EXPECT_EQ(2, (int)source_fetch_mock.call_count);
    EXPECT_STREQ("/fetched/zlib-1.3.1", zlib->root);
    /* And every answer kept, so the next build asks nobody. */
    EXPECT_EQ(2, (int)resolve_remember_mock.call_count);
    dep_graph_free(graph);
}

DESCRIBE(a_remembered_release_is_not_asked_for_again) {
    static const release table[] = {{"zlib", "1.3.1", NULL, false}};
    PUBLISH(table);
    resolve_remembered_mock.custom_fake = remembered;
    ASSERT_TRUE(resolve("zlib = \"1.3.1\"\n", false));
    EXPECT_EQ(0, (int)resolve_version_mock.call_count);
    dep_graph_free(graph);
}

DESCRIBE(a_prebuilt_release_is_refused_by_name) {
    static const release table[] = {{"zlib", "1.3.1", NULL, true}};
    PUBLISH(table);
    EXPECT_FALSE(resolve("zlib = \"1.3.1\"\n", false));
    EXPECT_NOT_NULL(strstr(err, "zlib 1.3.1 is published as a prebuilt artifact"));
    EXPECT_EQ(0, (int)source_fetch_mock.call_count);
}

DESCRIBE(a_failed_download_names_the_dependency) {
    static const release table[] = {{"zlib", "1.3.1", NULL, false}};
    PUBLISH(table);
    source_fetch_mock.custom_fake = NULL; /* returns false */
    EXPECT_FALSE(resolve("zlib = \"1.3.1\"\n", false));
    EXPECT_NOT_NULL(strstr(err, "dependency 'zlib'"));
}

/* --- a conflict, and the search for a way out --- */

/* png and tiff both depend on zlib and disagree about which. png 1.7.0 moved
   to the zlib tiff wants; png 1.5.0 is older than the one declared. */
static const release DISAGREEING[] = {
    {"png", "1.7.0", "zlib = \"1.3.1\"\n", false},
    {"png", "1.6.0", "zlib = \"1.2.0\"\n", false},
    {"png", "1.5.0", "zlib = \"1.3.1\"\n", false},
    {"tiff", "4.0.0", "zlib = \"1.3.1\"\n", false},
    {"zlib", "1.3.1", NULL, false},
    {"zlib", "1.2.0", NULL, false},
};

DESCRIBE(a_conflict_names_both_claims_and_downloads_nothing) {
    PUBLISH(DISAGREEING);
    EXPECT_FALSE(resolve("png = \"1.6.0\"\ntiff = \"4.0.0\"\n", false));
    EXPECT_STREQ("zlib", conflict.name);
    EXPECT_FALSE(conflict.has_proposal);
    /* Nothing is on disk for a version the user is about to be asked to change. */
    EXPECT_EQ(0, (int)source_fetch_mock.call_count);
}

DESCRIBE(the_search_proposes_the_newer_release_that_settles_it) {
    PUBLISH(DISAGREEING);
    EXPECT_FALSE(resolve("png = \"1.6.0\"\ntiff = \"4.0.0\"\n", true));
    ASSERT_TRUE(conflict.has_proposal);
    EXPECT_STREQ("png", conflict.change_name);
    EXPECT_STREQ("1.6.0", conflict.change_from);
    EXPECT_STREQ("1.7.0", conflict.change_to);
    EXPECT_STREQ("deps", conflict.change_table);
    EXPECT_STREQ("1.3.1", conflict.settles_on);
    EXPECT_EQ(0, (int)source_fetch_mock.call_count);
}

DESCRIBE(the_search_never_proposes_a_downgrade) {
    /* Only png 1.5.0 would settle it, and it is older than what was chosen:
       taking a version away is not a proposal to accept without thinking. */
    static const release table[] = {
        {"png", "1.6.0", "zlib = \"1.2.0\"\n", false},
        {"png", "1.5.0", "zlib = \"1.3.1\"\n", false},
        {"tiff", "4.0.0", "zlib = \"1.3.1\"\n", false},
        {"zlib", "1.3.1", NULL, false},
        {"zlib", "1.2.0", NULL, false},
    };
    PUBLISH(table);
    EXPECT_FALSE(resolve("png = \"1.6.0\"\ntiff = \"4.0.0\"\n", true));
    EXPECT_STREQ("zlib", conflict.name);
    EXPECT_FALSE(conflict.has_proposal);
}

DESCRIBE(the_search_tries_a_bounded_number_of_releases) {
    /* Twelve newer png releases, none of which settles it: the search gives up
       after eight rather than walking the whole history while somebody waits. */
    static const release table[] = {
        {"png", "2.11.0", "zlib = \"1.2.0\"\n", false}, {"png", "2.10.0", "zlib = \"1.2.0\"\n", false},
        {"png", "2.9.0", "zlib = \"1.2.0\"\n", false},  {"png", "2.8.0", "zlib = \"1.2.0\"\n", false},
        {"png", "2.7.0", "zlib = \"1.2.0\"\n", false},  {"png", "2.6.0", "zlib = \"1.2.0\"\n", false},
        {"png", "2.5.0", "zlib = \"1.2.0\"\n", false},  {"png", "2.4.0", "zlib = \"1.2.0\"\n", false},
        {"png", "2.3.0", "zlib = \"1.2.0\"\n", false},  {"png", "2.2.0", "zlib = \"1.2.0\"\n", false},
        {"png", "2.1.0", "zlib = \"1.2.0\"\n", false},  {"png", "2.0.0", "zlib = \"1.2.0\"\n", false},
        {"png", "1.6.0", "zlib = \"1.2.0\"\n", false},  {"tiff", "4.0.0", "zlib = \"1.3.1\"\n", false},
        {"zlib", "1.3.1", NULL, false},                 {"zlib", "1.2.0", NULL, false},
    };
    PUBLISH(table);
    EXPECT_FALSE(resolve("png = \"1.6.0\"\ntiff = \"4.0.0\"\n", true));
    EXPECT_FALSE(conflict.has_proposal);
    EXPECT_EQ(8, (int)png_2_asked);
}
