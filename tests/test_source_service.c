#include <moltest.h>

#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/recipe_service.h>
#include <molto/services/source_service.h>
#include <molto/util/json.h>
#include <molto/util/toml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The fetcher, exercised without a network.
 *
 * curl reads file:// URLs, so an archive built here and fetched by its own
 * path goes through the same download, verify, unpack and strip_prefix path a
 * real one does. What is not exercised is TLS and a remote server, which are
 * curl's to get right and not molto's. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];  /* the scratch directory everything lives under */
    char cache[128]; /* what $MOLTO_CACHE is pointed at */
} sandbox;

static bool sandbox_open(sandbox *at) {
    if (!moltest_temp_dir("molto_source", at->root, sizeof at->root))
        return false;
    snprintf(at->cache, sizeof at->cache, "%s/cache", at->root);
    return setenv("MOLTO_CACHE", at->cache, 1) == 0;
}

static void sandbox_close(const sandbox *at) {
    (void)unsetenv("MOLTO_CACHE");
    (void)fs_remove_tree(at->root);
}

/* A release tarball: one directory, wrapping one file, the way every upstream
   publishes one. Answers its path. */
static bool make_tarball(const sandbox *at, char *out, size_t size) {
    char inside[PATH_MAX_LEN];
    if (!fs_format_path(inside, sizeof inside, "%s/pkg-1.0/src", at->root) ||
        !fs_make_dirs(inside))
        return false;

    char file[PATH_MAX_LEN];
    if (!fs_format_path(file, sizeof file, "%s/lib.c", inside) ||
        !fs_write_file(file, "int answer(void) { return 42; }\n"))
        return false;

    if (!fs_format_path(out, size, "%s/pkg-1.0.tar.gz", at->root))
        return false;

    const char *argv[] = { "tar", "-czf", out, "-C", at->root, "pkg-1.0", NULL };
    return process_run(argv) == 0;
}

/* The same tree as a zip, which is what sqlite.org publishes. */
static bool make_zip(const sandbox *at, char *out, size_t size) {
    char inside[PATH_MAX_LEN];
    if (!fs_format_path(inside, sizeof inside, "%s/pkg-1.0/src", at->root) || !fs_make_dirs(inside))
        return false;

    char file[PATH_MAX_LEN];
    if (!fs_format_path(file, sizeof file, "%s/lib.c", inside) ||
        !fs_write_file(file, "int answer(void) { return 42; }\n"))
        return false;

    if (!fs_format_path(out, size, "%s/pkg-1.0.zip", at->root))
        return false;

    /* -j would flatten it; the wrapper directory is the point. */
    char script[PATH_MAX_LEN * 3];
    if (!fs_format_path(script, sizeof script, "cd '%s' && zip -qr '%s' pkg-1.0", at->root, out))
        return false;
    const char *argv[] = { "sh", "-c", script, NULL };
    return process_run(argv) == 0;
}

/*
 * The digest, from something that is not molto.
 *
 * Deliberately an external tool: molto computes this itself to verify what a
 * recipe names, and a test that asked molto for the expected value would pass
 * with a broken implementation on both sides.
 *
 * Two spellings because there is no one tool. GNU coreutils calls it
 * `sha256sum` — Linux, and MSYS2 on Windows — and macOS ships Perl's `shasum`
 * instead and no `sha256sum` at all. Both print the same shape, `<64 hex>` then
 * the filename, so only the command differs.
 */
static bool digest_from(const char *const *argv, char *out, size_t size) {
    char captured[256] = "";
    if (process_capture(argv, captured, sizeof captured) != 0)
        return false;
    const size_t length = strcspn(captured, " \t");
    if (length != 64 || length >= size)
        return false;
    snprintf(out, size, "%.*s", (int)length, captured);
    return true;
}

static bool digest_of(const char *file, char *out, size_t size) {
    const char *coreutils[] = { "sha256sum", "--binary", file, NULL };
    if (digest_from(coreutils, out, size))
        return true;
    const char *perl[] = { "shasum", "-a", "256", "-b", file, NULL };
    return digest_from(perl, out, size);
}

/* Reads a [source] table out of recipe text. */
static bool read_source(const char *text, source_spec *out, char *err, size_t err_size) {
    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    if (doc == NULL) {
        snprintf(err, err_size, "%s", parse_err);
        return false;
    }
    const bool ok = source_read(doc_from_toml(doc), out, err, err_size);
    toml_free(doc);
    return ok;
}

/* The same table, as the registry serves it back: parsed at publication and
   handed over as JSON, with the text it came from never stored. */
static bool read_source_json(const char *text, source_spec *out, char *err, size_t err_size) {
    json_document *doc = json_parse(text);
    if (doc == NULL) {
        snprintf(err, err_size, "%s", "not JSON");
        return false;
    }
    const bool ok = source_read(doc_from_json(json_root(doc)), out, err, err_size);
    json_free(doc);
    return ok;
}

MOLTEST(source_reads_an_archive_origin) {
    source_spec spec;
    char err[256] = "";
    ASSERT_TRUE(read_source("[source]\n"
                            "archive = \"https://sqlite.org/a.zip\"\n"
                            "sha256 = \"" /* 64 hex */
                            "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\"\n"
                            "strip_prefix = \"sqlite-amalgamation\"\n",
                            &spec, err, sizeof err));

    EXPECT_EQ(source_origin_archive, spec.origin);
    EXPECT_STREQ("https://sqlite.org/a.zip", spec.location);
    EXPECT_STREQ("sqlite-amalgamation", spec.strip_prefix);
}

MOLTEST(source_reads_a_git_origin_and_its_reference) {
    source_spec spec;
    char err[256] = "";
    ASSERT_TRUE(read_source("[source]\ngit = \"https://x/y.git\"\nrev = \"abc123\"\n", &spec, err,
                            sizeof err));

    EXPECT_EQ(source_origin_git, spec.origin);
    EXPECT_STREQ("abc123", spec.reference);
    /* A commit id is already a digest, so none is required beside it. */
    EXPECT_STREQ("", spec.sha256);
}

MOLTEST(source_refuses_an_archive_with_no_digest) {
    /* A URL promises a location, not content: an upstream that re-rolls its
       tarball would change what every consumer compiles, silently. */
    source_spec spec;
    char err[256] = "";
    EXPECT_FALSE(read_source("[source]\narchive = \"https://x/y.tar.gz\"\n", &spec, err,
                             sizeof err));
    EXPECT_NOT_NULL(strstr(err, "sha256"));
}

MOLTEST(source_refuses_a_digest_that_is_not_one) {
    source_spec spec;
    char err[256] = "";
    EXPECT_FALSE(read_source("[source]\narchive = \"https://x/y.tar.gz\"\nsha256 = \"beef\"\n",
                             &spec, err, sizeof err));
}

MOLTEST(source_refuses_two_origins) {
    source_spec spec;
    char err[256] = "";
    EXPECT_FALSE(read_source("[source]\ngit = \"https://x/y.git\"\npath = \"vendor/y\"\n", &spec,
                             err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "more than one origin"));
}

MOLTEST(source_refuses_a_tag_and_a_rev_together) {
    source_spec spec;
    char err[256] = "";
    EXPECT_FALSE(read_source("[source]\ngit = \"https://x/y.git\"\ntag = \"v1\"\nrev = \"abc\"\n",
                             &spec, err, sizeof err));
}

MOLTEST(source_reads_a_declared_compression_format) {
    /* Declared rather than inferred: an extension is a naming convention, not
       a fact about the bytes. */
    source_spec spec;
    char err[256] = "";
    ASSERT_TRUE(read_source("[source]\n"
                            "archive = \"https://x/download\"\n"
                            "sha256 = "
                            "\"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\"\n"
                            "compression_format = \"tar.xz\"\n",
                            &spec, err, sizeof err));

    EXPECT_EQ(source_compression_tar_xz, spec.compression);
    EXPECT_STREQ("tar.xz", source_compression_name(spec.compression));
}

MOLTEST(source_without_a_compression_format_still_infers_one) {
    /* The key is new, and the recipes written before it existed keep working. */
    source_spec spec;
    char err[256] = "";
    ASSERT_TRUE(read_source("[source]\n"
                            "archive = \"https://x/y.zip\"\n"
                            "sha256 = "
                            "\"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\"\n",
                            &spec, err, sizeof err));

    EXPECT_EQ(source_compression_infer, spec.compression);
}

MOLTEST(source_accepts_every_compression_it_can_unpack) {
    static const char *const formats[] = { "zip", "tar", "tar.gz", "tar.bz2", "tar.xz", "tar.zst" };

    for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        char text[512];
        snprintf(text, sizeof text,
                 "[source]\narchive = \"https://x/y\"\nsha256 = "
                 "\"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\"\n"
                 "compression_format = \"%s\"\n",
                 formats[i]);

        source_spec spec;
        char err[256] = "";
        ASSERT_TRUE(read_source(text, &spec, err, sizeof err));
        EXPECT_STREQ(formats[i], source_compression_name(spec.compression));
    }
}

MOLTEST(source_refuses_a_compression_it_cannot_unpack) {
    /* A rejected recipe on the machine that reads it beats an empty directory
       on the machine that builds it. */
    source_spec spec;
    char err[256] = "";
    EXPECT_FALSE(read_source("[source]\n"
                             "archive = \"https://x/y.rar\"\n"
                             "sha256 = "
                             "\"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\"\n"
                             "compression_format = \"rar\"\n",
                             &spec, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "tar.zst"));
}

MOLTEST(source_refuses_a_compression_format_beside_a_clone) {
    /* A git checkout is not packed, so the key describes nothing there — and a
       recipe that sets it believes something about what molto will do. */
    source_spec spec;
    char err[256] = "";
    EXPECT_FALSE(read_source("[source]\ngit = \"https://x/y.git\"\ncompression_format = \"zip\"\n",
                             &spec, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "not one"));
}

MOLTEST(source_refuses_a_recipe_with_no_source_table) {
    source_spec spec;
    char err[256] = "";
    EXPECT_FALSE(read_source("[build]\nsystem = \"none\"\n", &spec, err, sizeof err));
}

MOLTEST(source_reads_the_same_spec_from_a_registry_json_recipe) {
    /* The half with no local file anyone can diff against: if the two readers
       ever stop agreeing, a dependency resolved from the registry is fetched
       differently from the same recipe on disk. */
    source_spec from_toml;
    source_spec from_json;
    char err[256] = "";

    ASSERT_TRUE(read_source("[source]\n"
                            "archive = \"https://sqlite.org/a.zip\"\n"
                            "sha256 = "
                            "\"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\"\n"
                            "strip_prefix = \"sqlite-amalgamation-3530400\"\n",
                            &from_toml, err, sizeof err));
    ASSERT_TRUE(read_source_json(
        "{\"source\":{\"archive\":\"https://sqlite.org/a.zip\","
        "\"sha256\":\"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\","
        "\"strip_prefix\":\"sqlite-amalgamation-3530400\"}}",
        &from_json, err, sizeof err));

    EXPECT_EQ(from_toml.origin, from_json.origin);
    EXPECT_STREQ(from_toml.location, from_json.location);
    EXPECT_STREQ(from_toml.sha256, from_json.sha256);
    EXPECT_STREQ(from_toml.strip_prefix, from_json.strip_prefix);
}

MOLTEST(source_validates_a_spec_a_manifest_built_by_hand) {
    /* A [deps] entry produces this struct without ever going through a
       [source] table, and it owes the same two rules. */
    source_spec spec;
    char err[256] = "";
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_archive;
    snprintf(spec.location, sizeof spec.location, "%s", "https://x/y.tar.gz");

    EXPECT_FALSE(source_spec_validate(&spec, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "sha256"));

    memset(spec.sha256, 'a', 64);
    spec.sha256[64] = '\0';
    EXPECT_TRUE(source_spec_validate(&spec, err, sizeof err));

    snprintf(spec.sha256, sizeof spec.sha256, "%s", "NOTHEX");
    EXPECT_FALSE(source_spec_validate(&spec, err, sizeof err));
}

MOLTEST(source_addresses_the_cache_by_coordinate) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char path[PATH_MAX_LEN] = "";
    EXPECT_TRUE(source_cache_path("sqlite", "3.53.4", "any", path, sizeof path));
    EXPECT_NOT_NULL(strstr(path, "sources/sqlite/3.53.4/any"));
    EXPECT_FALSE(source_is_cached("sqlite", "3.53.4", "any"));

    sandbox_close(&at);
}

MOLTEST(source_caches_an_archive_under_its_digest) {
    /* Content-addressed: two recipes naming the same bytes share one entry,
       and a dependency with no version of its own still has a key. */
    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_archive;
    memset(spec.sha256, 'a', 64);
    spec.sha256[64] = '\0';

    char key[80] = "";
    char err[256] = "";
    ASSERT_TRUE(source_cache_key(&spec, key, sizeof key, err, sizeof err));
    EXPECT_STREQ(spec.sha256, key);
}

MOLTEST(source_caches_a_git_rev_under_the_rev_itself) {
    /* A commit id is already the answer, so asking the remote would be one
       round trip to be told what was written. */
    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_git;
    snprintf(spec.location, sizeof spec.location, "%s", "https://x/y.git");
    snprintf(spec.reference, sizeof spec.reference, "%s",
             "8bc411b5985233d9c31c50c8f7336cb7c0411b15");

    char key[80] = "";
    char err[256] = "";
    ASSERT_TRUE(source_cache_key(&spec, key, sizeof key, err, sizeof err));
    EXPECT_STREQ("8bc411b5985233d9c31c50c8f7336cb7c0411b15", key);
}

MOLTEST(source_resolves_a_git_tag_to_a_commit) {
    /* RFC-0008: a branch, a tag and a rev all resolve to a commit id, and the
       commit id is what gets recorded. A local repository stands in for a
       remote one — git does not care which. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char repo[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(repo, sizeof repo, "%s/repo", at.root));
    char script[PATH_MAX_LEN * 4];
    ASSERT_TRUE(fs_format_path(
        script, sizeof script,
        "mkdir -p '%s' && cd '%s' && git init -q . && git config user.email t@t && "
        "git config user.name t && echo x > f && git add f && "
        "git -c commit.gpgsign=false commit -qm one && git tag v1 && git rev-parse HEAD",
        repo, repo));
    const char *argv[] = { "sh", "-c", script, NULL };
    char head[128] = "";
    if (process_capture(argv, head, sizeof head) != 0) {
        SKIP("this machine has no usable git");
        sandbox_close(&at);
        return;
    }
    head[strcspn(head, " \t\r\n")] = '\0';

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_git;
    snprintf(spec.location, sizeof spec.location, "%s", repo);
    snprintf(spec.reference, sizeof spec.reference, "%s", "v1");

    char key[80] = "";
    char err[256] = "";
    ASSERT_TRUE(source_cache_key(&spec, key, sizeof key, err, sizeof err));
    EXPECT_STREQ(head, key);

    sandbox_close(&at);
}

MOLTEST(source_reports_a_reference_the_repository_does_not_have) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char repo[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(repo, sizeof repo, "%s/repo", at.root));
    char script[PATH_MAX_LEN * 3];
    ASSERT_TRUE(fs_format_path(script, sizeof script,
                               "mkdir -p '%s' && cd '%s' && git init -q .", repo, repo));
    const char *argv[] = { "sh", "-c", script, NULL };
    if (process_run(argv) != 0) {
        SKIP("this machine has no usable git");
        sandbox_close(&at);
        return;
    }

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_git;
    snprintf(spec.location, sizeof spec.location, "%s", repo);
    snprintf(spec.reference, sizeof spec.reference, "%s", "v-nope");

    char key[80] = "";
    char err[256] = "";
    EXPECT_FALSE(source_cache_key(&spec, key, sizeof key, err, sizeof err));

    sandbox_close(&at);
}

MOLTEST(source_has_no_cache_key_for_a_path) {
    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_path;

    char key[80] = "";
    char err[256] = "";
    EXPECT_FALSE(source_cache_key(&spec, key, sizeof key, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "never cached"));
}

MOLTEST(source_refuses_a_coordinate_that_could_escape_the_cache) {
    /* The coordinate comes out of a recipe, and a recipe comes from a
       registry. A target of ".." would otherwise put a fetch anywhere. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char path[PATH_MAX_LEN] = "";
    EXPECT_FALSE(source_cache_path("sqlite", "3.53.4", "..", path, sizeof path));
    EXPECT_FALSE(source_cache_path("sqlite", "3.53.4", "../../etc", path, sizeof path));
    EXPECT_FALSE(source_cache_path("sqlite", "", "any", path, sizeof path));
    EXPECT_FALSE(source_cache_path("", "3.53.4", "any", path, sizeof path));
    EXPECT_TRUE(source_cache_path("sqlite", "3.53.4", "any", path, sizeof path));

    sandbox_close(&at);
}

MOLTEST(source_fetches_verifies_and_strips_an_archive) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char tarball[PATH_MAX_LEN];
    char digest[80];
    ASSERT_TRUE(make_tarball(&at, tarball, sizeof tarball));
    ASSERT_TRUE(digest_of(tarball, digest, sizeof digest));

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_archive;
    snprintf(spec.location, sizeof spec.location, "file://%s", tarball);
    snprintf(spec.sha256, sizeof spec.sha256, "%s", digest);
    snprintf(spec.strip_prefix, sizeof spec.strip_prefix, "%s", "pkg-1.0");

    char out[PATH_MAX_LEN] = "";
    char err[256] = "";
    ASSERT_TRUE(source_fetch(&spec, "pkg", "1.0", "any", out, sizeof out, err, sizeof err));

    /* The wrapper directory is gone: what the recipe exports is the root. */
    char source[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(source, sizeof source, "%s/src/lib.c", out));
    EXPECT_TRUE(fs_path_exists(source));
    EXPECT_TRUE(source_is_cached("pkg", "1.0", "any"));

    sandbox_close(&at);
}

MOLTEST(source_unpacks_a_zip_by_what_upstream_called_it) {
    /* The format is decided by the extension, so the download has to keep the
       name the URL gave it. Saved under a name of molto's own choosing it has
       none, and sqlite.org's .zip was handed to tar, which said so. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char zip[PATH_MAX_LEN];
    char digest[80];
    ASSERT_TRUE(make_zip(&at, zip, sizeof zip));
    ASSERT_TRUE(digest_of(zip, digest, sizeof digest));

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_archive;
    snprintf(spec.location, sizeof spec.location, "file://%s", zip);
    snprintf(spec.sha256, sizeof spec.sha256, "%s", digest);
    snprintf(spec.strip_prefix, sizeof spec.strip_prefix, "%s", "pkg-1.0");

    char out[PATH_MAX_LEN] = "";
    char err[256] = "";
    ASSERT_TRUE(source_fetch(&spec, "pkg", "1.0", "any", out, sizeof out, err, sizeof err));

    char source[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(source, sizeof source, "%s/src/lib.c", out));
    EXPECT_TRUE(fs_path_exists(source));

    sandbox_close(&at);
}

MOLTEST(source_believes_the_recipe_over_the_extension) {
    /* The case the key exists for: a URL that ends in .zip and serves a
       tarball. Inference reaches for unzip and fails; the declared format is
       the fact. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char tarball[PATH_MAX_LEN];
    ASSERT_TRUE(make_tarball(&at, tarball, sizeof tarball));

    /* Same bytes, a name that lies about them. */
    char lying[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(lying, sizeof lying, "%s/pkg-1.0.zip", at.root));
    ASSERT_TRUE(fs_copy_file(tarball, lying));

    char digest[80];
    ASSERT_TRUE(digest_of(lying, digest, sizeof digest));

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_archive;
    snprintf(spec.location, sizeof spec.location, "file://%s", lying);
    snprintf(spec.sha256, sizeof spec.sha256, "%s", digest);
    snprintf(spec.strip_prefix, sizeof spec.strip_prefix, "%s", "pkg-1.0");

    /* Inferring from the name picks unzip, which cannot read it. */
    char out[PATH_MAX_LEN] = "";
    char err[256] = "";
    spec.compression = source_compression_infer;
    EXPECT_FALSE(source_fetch(&spec, "pkg", "1.0", "any", out, sizeof out, err, sizeof err));

    /* Declared, it unpacks. */
    spec.compression = source_compression_tar_gz;
    ASSERT_TRUE(source_fetch(&spec, "pkg", "1.0", "any", out, sizeof out, err, sizeof err));
    char source[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(source, sizeof source, "%s/src/lib.c", out));
    EXPECT_TRUE(fs_path_exists(source));

    sandbox_close(&at);
}

MOLTEST(source_refuses_bytes_that_are_not_the_ones_the_recipe_names) {
    /* The point of the digest. Nothing is left in the cache afterwards: a
       failed fetch that leaves a tree behind is a tree the next build reads. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char tarball[PATH_MAX_LEN];
    ASSERT_TRUE(make_tarball(&at, tarball, sizeof tarball));

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_archive;
    snprintf(spec.location, sizeof spec.location, "file://%s", tarball);
    snprintf(spec.sha256, sizeof spec.sha256, "%s", "a" /* not the digest */);
    memset(spec.sha256, 'a', 64);
    spec.sha256[64] = '\0';

    char out[PATH_MAX_LEN] = "";
    char err[256] = "";
    EXPECT_FALSE(source_fetch(&spec, "pkg", "1.0", "any", out, sizeof out, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "hashes to"));
    EXPECT_FALSE(source_is_cached("pkg", "1.0", "any"));

    sandbox_close(&at);
}

MOLTEST(source_fetches_a_coordinate_once) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char tarball[PATH_MAX_LEN];
    char digest[80];
    ASSERT_TRUE(make_tarball(&at, tarball, sizeof tarball));
    ASSERT_TRUE(digest_of(tarball, digest, sizeof digest));

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_archive;
    snprintf(spec.location, sizeof spec.location, "file://%s", tarball);
    snprintf(spec.sha256, sizeof spec.sha256, "%s", digest);
    snprintf(spec.strip_prefix, sizeof spec.strip_prefix, "%s", "pkg-1.0");

    char out[PATH_MAX_LEN] = "";
    char err[256] = "";
    ASSERT_TRUE(source_fetch(&spec, "pkg", "1.0", "any", out, sizeof out, err, sizeof err));

    /* Deleting the archive proves the second call never went back for it. */
    EXPECT_EQ(0, remove(tarball));
    char again[PATH_MAX_LEN] = "";
    EXPECT_TRUE(source_fetch(&spec, "pkg", "1.0", "any", again, sizeof again, err, sizeof err));
    EXPECT_STREQ(out, again);

    sandbox_close(&at);
}

MOLTEST(source_refuses_a_strip_prefix_that_is_not_there) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char tarball[PATH_MAX_LEN];
    char digest[80];
    ASSERT_TRUE(make_tarball(&at, tarball, sizeof tarball));
    ASSERT_TRUE(digest_of(tarball, digest, sizeof digest));

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_archive;
    snprintf(spec.location, sizeof spec.location, "file://%s", tarball);
    snprintf(spec.sha256, sizeof spec.sha256, "%s", digest);
    snprintf(spec.strip_prefix, sizeof spec.strip_prefix, "%s", "pkg-2.0");

    char out[PATH_MAX_LEN] = "";
    char err[256] = "";
    EXPECT_FALSE(source_fetch(&spec, "pkg", "1.0", "any", out, sizeof out, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "strip_prefix"));

    sandbox_close(&at);
}

MOLTEST(source_uses_a_local_path_where_it_is) {
    /* Not copied: a recipe being developed has to keep tracking the edits
       being made to the directory it names. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_path;
    snprintf(spec.location, sizeof spec.location, "%s", at.root);

    char out[PATH_MAX_LEN] = "";
    char err[256] = "";
    ASSERT_TRUE(source_fetch(&spec, "pkg", "1.0", "any", out, sizeof out, err, sizeof err));
    EXPECT_STREQ(at.root, out);
    EXPECT_FALSE(source_is_cached("pkg", "1.0", "any"));

    sandbox_close(&at);
}

MOLTEST(source_reports_a_local_path_that_is_not_a_directory) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    source_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.origin = source_origin_path;
    snprintf(spec.location, sizeof spec.location, "%s/nowhere", at.root);

    char out[PATH_MAX_LEN] = "";
    char err[256] = "";
    EXPECT_FALSE(source_fetch(&spec, "pkg", "1.0", "any", out, sizeof out, err, sizeof err));

    sandbox_close(&at);
}

/* --- the files a recipe copies into place --- */

/* A drop with one file under `scripts/`, which is the shape every library that
   ships a prebuilt configuration has. */
static bool make_drop(const sandbox *at, char *out, size_t size) {
    char scripts[PATH_MAX_LEN];
    snprintf(out, size, "%s/drop", at->root);
    snprintf(scripts, sizeof scripts, "%s/scripts", out);
    if (!fs_make_dirs(scripts))
        return false;

    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/prebuilt.h", scripts);
    if (!fs_write_file(file, "#define CONFIGURED 1\n"))
        return false;

    snprintf(file, sizeof file, "%s/upstream.c", out);
    return fs_write_file(file, "int upstream(void) { return 1; }\n");
}

static recipe_provide one_provision(const char *file, const char *from) {
    recipe_provide provide = {0};
    snprintf(provide.items[0].file, sizeof provide.items[0].file, "%s", file);
    snprintf(provide.items[0].from, sizeof provide.items[0].from, "%s", from);
    provide.count = 1;
    return provide;
}

MOLTEST(a_provision_writes_the_file_a_configure_step_would_have) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    char drop[PATH_MAX_LEN];
    ASSERT_TRUE(make_drop(&at, drop, sizeof drop));

    const recipe_provide provide = one_provision("config.h", "scripts/prebuilt.h");
    char err[256] = "";
    EXPECT_TRUE(source_provide(drop, &provide, err, sizeof err));

    char written[PATH_MAX_LEN];
    snprintf(written, sizeof written, "%s/config.h", drop);
    char *text = fs_read_file(written);
    ASSERT_NOT_NULL(text);
    EXPECT_STREQ("#define CONFIGURED 1\n", text);
    free(text);

    sandbox_close(&at);
}

/* A path origin is used where it lies and carries no stamp, so this runs again
   on every build. Identical bytes mean the copy already happened. */
MOLTEST(a_provision_already_applied_is_not_an_error) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    char drop[PATH_MAX_LEN];
    ASSERT_TRUE(make_drop(&at, drop, sizeof drop));

    const recipe_provide provide = one_provision("config.h", "scripts/prebuilt.h");
    char err[256] = "";
    ASSERT_TRUE(source_provide(drop, &provide, err, sizeof err));
    EXPECT_TRUE(source_provide(drop, &provide, err, sizeof err));

    sandbox_close(&at);
}

/* The line between completing a configuration and patching one. */
MOLTEST(a_provision_refuses_to_overwrite_what_upstream_shipped) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    char drop[PATH_MAX_LEN];
    ASSERT_TRUE(make_drop(&at, drop, sizeof drop));

    const recipe_provide provide = one_provision("upstream.c", "scripts/prebuilt.h");
    char err[256] = "";
    EXPECT_FALSE(source_provide(drop, &provide, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "different bytes"));

    sandbox_close(&at);
}

MOLTEST(a_provision_stays_inside_the_source_it_completes) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    char drop[PATH_MAX_LEN];
    ASSERT_TRUE(make_drop(&at, drop, sizeof drop));
    char err[256] = "";

    const recipe_provide absolute = one_provision("config.h", "/etc/hostname");
    EXPECT_FALSE(source_provide(drop, &absolute, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "absolute"));

    const recipe_provide climbing = one_provision("config.h", "../../etc/hostname");
    EXPECT_FALSE(source_provide(drop, &climbing, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "climbs out"));

    /* And the destination is fenced as well as the origin: writing above the
       drop is how a dependency edits the project that consumes it. */
    const recipe_provide escaping = one_provision("../escaped.h", "scripts/prebuilt.h");
    EXPECT_FALSE(source_provide(drop, &escaping, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "climbs out"));

    sandbox_close(&at);
}

MOLTEST(a_provision_naming_a_file_the_source_lacks_is_refused) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));
    char drop[PATH_MAX_LEN];
    ASSERT_TRUE(make_drop(&at, drop, sizeof drop));

    const recipe_provide provide = one_provision("config.h", "scripts/absent.h");
    char err[256] = "";
    EXPECT_FALSE(source_provide(drop, &provide, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "scripts/absent.h"));

    sandbox_close(&at);
}

MOLTEST(providing_nothing_touches_nothing) {
    const recipe_provide empty = {0};
    char err[256] = "";
    EXPECT_TRUE(source_provide("/nonexistent/path", &empty, err, sizeof err));
}
