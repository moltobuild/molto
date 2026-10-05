#include <moltest.h>
#include <moltest_mock.h>

#include <molto/services/registry_service.h>
#include <molto/services/resolve_service.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <string.h>

/*
 * What resolve_service asks the registry, and what it makes of every answer,
 * with the registry faked.
 *
 * An isolated test (RFC-0021): it replaces src/services/registry_service.c, so
 * registry_get() and registry_explain() below are the ones this binary has.
 * test_resolve_service.c reads canned bodies directly; these go through the
 * requests, so the path asked for, every status the registry can answer with,
 * and the integrity check on what comes back are each pinned down.
 */

MOCK_VALUE_FUNC(bool, registry_get, const char *, const char *, registry_response *, char *,
                size_t);
MOCK_VOID_FUNC(registry_explain, const registry_response *, char *, size_t);

/* The registry's answer, and the path it was asked for, copied during the call. */
static long status;
static const char *body;
static const char *transport_error;
static char asked[256];

static bool registry_answers(const char *base_url, const char *path, registry_response *out,
                             char *err, size_t err_size) {
    (void)base_url;
    snprintf(asked, sizeof asked, "%s", path);
    if(transport_error != NULL) {
        snprintf(err, err_size, "%s", transport_error);
        return false;
    }
    out->status = status;
    snprintf(out->body, sizeof out->body, "%s", body != NULL ? body : "");
    return true;
}

/* The real one reads a JSON message out of the body; the body is enough here. */
static void explain_with_body(const registry_response *response, char *out, size_t size) {
    snprintf(out, size, "%s", response->body);
}

BEFORE_EACH() {
    status = 200;
    body = NULL;
    transport_error = NULL;
    asked[0] = '\0';
    registry_get_mock.custom_fake = registry_answers;
    registry_explain_mock.custom_fake = explain_with_body;
}

static char err[512];
static str_list versions;

static bool versions_of(const char *name) {
    err[0] = '\0';
    str_list_init(&versions);
    return resolve_versions("https://registry.example", name, &versions, err, sizeof err);
}

/* --- the listing --- */

DESCRIBE(versions_are_asked_by_name_and_ordered_newest_first) {
    /* Publication order, a version molto cannot order, and a pre-release. */
    body = "{\"kind\":\"package\",\"name\":\"zlib\",\"releases\":["
           "{\"version\":\"1.2.13\"},{\"version\":\"1.3.1\"},{\"version\":\"latest\"},"
           "{\"version\":\"1.3.0\"},{\"version\":\"1.4.0-rc.1\"}]}";
    ASSERT_TRUE(versions_of("zlib"));
    EXPECT_STREQ("/v1/packages/zlib", asked);
    ASSERT_EQ(4, (int)str_list_count(&versions));
    EXPECT_STREQ("1.4.0-rc.1", str_list_get(&versions, 0));
    EXPECT_STREQ("1.3.1", str_list_get(&versions, 1));
    EXPECT_STREQ("1.2.13", str_list_get(&versions, 3));
    str_list_free(&versions);
}

DESCRIBE(a_name_the_registry_does_not_know_is_named) {
    status = 404;
    EXPECT_FALSE(versions_of("zlibb"));
    EXPECT_NOT_NULL(strstr(err, "no package called 'zlibb'"));
    str_list_free(&versions);
}

DESCRIBE(any_other_status_is_explained) {
    status = 503;
    body = "maintenance until 10:00";
    EXPECT_FALSE(versions_of("zlib"));
    EXPECT_NOT_NULL(strstr(err, "answered 503 for zlib"));
    EXPECT_NOT_NULL(strstr(err, "maintenance until 10:00"));
    str_list_free(&versions);
}

DESCRIBE(a_transport_failure_is_passed_on_as_it_was_said) {
    transport_error = "curl could not reach the registry: timed out";
    EXPECT_FALSE(versions_of("zlib"));
    EXPECT_STREQ("curl could not reach the registry: timed out", err);
    str_list_free(&versions);
}

DESCRIBE(a_package_without_releases_says_so) {
    body = "{\"kind\":\"package\",\"name\":\"zlib\",\"releases\":[]}";
    EXPECT_FALSE(versions_of("zlib"));
    EXPECT_NOT_NULL(strstr(err, "with no releases"));
    str_list_free(&versions);

    body = "{\"kind\":\"package\",\"name\":\"zlib\",\"releases\":[{\"version\":\"main\"}]}";
    EXPECT_FALSE(versions_of("zlib"));
    EXPECT_NOT_NULL(strstr(err, "no release with a version molto can order"));
    str_list_free(&versions);
}

DESCRIBE(a_name_too_long_for_a_path_is_refused_before_asking) {
    char name[600];
    memset(name, 'a', sizeof name - 1);
    name[sizeof name - 1] = '\0';
    EXPECT_FALSE(versions_of(name));
    /* The message quotes the name first, so its end is clipped by this buffer. */
    EXPECT_NOT_NULL(strstr(err, "the registry path for aaaa"));
    EXPECT_EQ(0, (int)registry_get_mock.call_count);
    str_list_free(&versions);
}

DESCRIBE(the_newest_version_is_the_first_once_ordered) {
    body = "{\"releases\":[{\"version\":\"0.9.0\"},{\"version\":\"0.10.0\"}]}";
    char newest[32] = "";
    ASSERT_TRUE(resolve_latest_version("https://registry.example", "png", newest, sizeof newest,
                                       err, sizeof err));
    EXPECT_STREQ("0.10.0", newest);

    char tiny[4] = "";
    EXPECT_FALSE(resolve_latest_version("https://registry.example", "png", tiny, sizeof tiny, err,
                                        sizeof err));
    EXPECT_NOT_NULL(strstr(err, "too long to record"));
}

/* --- one release --- */

#define RECIPE_OF(name, version)                                                                   \
    "\"metadata\":{\"schema\":1,\"form\":\"source\",\"kind\":\"package\","                          \
    "\"name\":\"" name "\",\"version\":\"" version "\",\"target\":\"any\","                         \
    "\"source\":{\"archive\":\"https://example.com/z.tar.gz\","                                     \
    "\"sha256\":\"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\"},"             \
    "\"build\":{\"system\":\"none\"},"                                                              \
    "\"artifacts\":{\"type\":\"source\",\"sources\":[\"z.c\"],\"include\":[\".\"]}}"

#define RELEASE_OF(name, version)                                                                  \
    "{\"kind\":\"package\",\"name\":\"zlib\",\"version\":\"1.3.1\",\"targets\":["                  \
    "{\"target\":\"any\",\"yanked\":false," RECIPE_OF(name, version) "}]}"

static resolved_dep dep;

static bool release(const char *version) {
    err[0] = '\0';
    return resolve_version("https://registry.example", "zlib", version, &dep, err, sizeof err);
}

DESCRIBE(a_release_is_asked_by_coordinate_and_kept_as_answered) {
    body = RELEASE_OF("zlib", "1.3.1");
    ASSERT_TRUE(release("1.3.1"));
    EXPECT_STREQ("/v1/packages/zlib/1.3.1", asked);
    EXPECT_STREQ("zlib", dep.coordinate.name);
    EXPECT_STREQ("1.3.1", dep.coordinate.version);
    /* The body travels with it, for the caller to remember after the fetch. */
    EXPECT_STREQ(RELEASE_OF("zlib", "1.3.1"), dep.body);
}

DESCRIBE(a_release_whose_recipe_describes_something_else_is_refused) {
    /* The registry is a remote party: asked for one coordinate, an answer about
       another must never be cached under the first. */
    body = RELEASE_OF("zlib", "1.2.0");
    EXPECT_FALSE(release("1.3.1"));
    EXPECT_NOT_NULL(strstr(err, "asked the registry for zlib 1.3.1"));
    EXPECT_NOT_NULL(strstr(err, "describes zlib 1.2.0"));
}

DESCRIBE(a_release_the_registry_does_not_have_is_named) {
    status = 404;
    EXPECT_FALSE(release("9.9.9"));
    EXPECT_NOT_NULL(strstr(err, "zlib 9.9.9 is not in the registry"));
}

DESCRIBE(a_release_answered_with_an_error_is_explained) {
    status = 500;
    body = "internal error";
    EXPECT_FALSE(release("1.3.1"));
    EXPECT_NOT_NULL(strstr(err, "answered 500 for zlib 1.3.1"));
    EXPECT_NOT_NULL(strstr(err, "internal error"));
}

DESCRIBE(a_release_is_not_asked_when_its_path_does_not_fit) {
    char version[600];
    memset(version, '1', sizeof version - 1);
    version[sizeof version - 1] = '\0';
    EXPECT_FALSE(release(version));
    EXPECT_NOT_NULL(strstr(err, "the registry path for zlib 1111"));
    EXPECT_EQ(0, (int)registry_get_mock.call_count);
}
