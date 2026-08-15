#include <moltest.h>

#include <molto/services/fs_service.h>
#include <molto/services/object_cache.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The shared object cache: what it covers, what makes two compilations the
   same one, and what it refuses to answer for. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];
    char cache[128];
} sandbox;

static bool sandbox_open(sandbox *at) {
    snprintf(at->root, sizeof at->root, "%s", "/tmp/molto_objcache_XXXXXX");
    if (mkdtemp(at->root) == NULL)
        return false;
    snprintf(at->cache, sizeof at->cache, "%s/cache", at->root);
    return setenv("MOLTO_CACHE", at->cache, 1) == 0;
}

static void sandbox_close(const sandbox *at) {
    (void)unsetenv("MOLTO_CACHE");
    (void)fs_remove_tree(at->root);
}

/* A source inside the shared source cache, the way a fetched dependency's is. */
static bool dep_source(const sandbox *at, char *out, size_t size) {
    return fs_format_path(out, size, "%s/sources/sqlite/3.53.4/any/sqlite3.c", at->cache);
}

MOLTEST(object_cache_covers_a_dependency_and_nothing_else) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char source[PATH_MAX_LEN];
    ASSERT_TRUE(dep_source(&at, source, sizeof source));
    EXPECT_TRUE(object_cache_covers(source));

    /* A project's own sources are mutable and their headers unknown, which is
       the question this cache does not answer. */
    char own[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(own, sizeof own, "%s/app/src/main.c", at.root));
    EXPECT_FALSE(object_cache_covers(own));

    sandbox_close(&at);
}

MOLTEST(object_cache_gives_one_path_to_two_projects_compiling_alike) {
    /* The point of the whole thing: the output paths differ, everything that
       reaches the compiler does not, so one object serves both. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char source[PATH_MAX_LEN];
    ASSERT_TRUE(dep_source(&at, source, sizeof source));

    char first[PATH_MAX_LEN] = "";
    char second[PATH_MAX_LEN] = "";
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -O0 -std=c17 -o /a/x.o -MF /a/x.o.d",
                                  first, sizeof first));
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -O0 -std=c17 -o /b/y.o -MF /b/y.o.d",
                                  second, sizeof second));
    EXPECT_STREQ(first, second);
}

MOLTEST(object_cache_separates_two_ways_of_compiling_the_same_file) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char source[PATH_MAX_LEN];
    ASSERT_TRUE(dep_source(&at, source, sizeof source));

    char debug[PATH_MAX_LEN] = "";
    char release[PATH_MAX_LEN] = "";
    char defined[PATH_MAX_LEN] = "";
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -O0 -o /a/x.o", debug, sizeof debug));
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -O2 -o /a/x.o", release, sizeof release));
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -O0 -DFTS5 -o /a/x.o", defined,
                                  sizeof defined));

    EXPECT_STRNE(debug, release);
    EXPECT_STRNE(debug, defined);

    sandbox_close(&at);
}

MOLTEST(object_cache_names_the_coordinate_it_holds) {
    /* So a cache directory can be read by a person, and so two dependencies
       cannot collide on one entry. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char source[PATH_MAX_LEN];
    char path[PATH_MAX_LEN] = "";
    ASSERT_TRUE(dep_source(&at, source, sizeof source));
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -o /a/x.o", path, sizeof path));

    EXPECT_NOT_NULL(strstr(path, "/objects/"));
    EXPECT_NOT_NULL(strstr(path, "sqlite/3.53.4/any/sqlite3.c"));

    sandbox_close(&at);
}

MOLTEST(object_cache_hashes_the_environment_as_it_stands) {
    /* The reason the environment is marked off instead of appended: molto
       composes the argv and knows it has no spaces, but it does not compose an
       environment value. Split on spaces, "X=a -o b" and "X=a -o c" both lose
       the token after the "-o" and key alike — one cached object answering for
       two different environments, taken by the wrong build. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char source[PATH_MAX_LEN];
    ASSERT_TRUE(dep_source(&at, source, sizeof source));

    char first[PATH_MAX_LEN] = "";
    char second[PATH_MAX_LEN] = "";
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -o /a/x.o" OBJECT_CACHE_ENV_MARK "X=a -o b",
                                  first, sizeof first));
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -o /a/x.o" OBJECT_CACHE_ENV_MARK "X=a -o c",
                                  second, sizeof second));
    EXPECT_STRNE(first, second);

    sandbox_close(&at);
}

MOLTEST(object_cache_still_ignores_the_output_arguments_before_the_mark) {
    /* Marking the environment off must not cost the thing the cache is for:
       two projects with the same environment, writing to different places,
       still share one object. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char source[PATH_MAX_LEN];
    ASSERT_TRUE(dep_source(&at, source, sizeof source));

    char first[PATH_MAX_LEN] = "";
    char second[PATH_MAX_LEN] = "";
    ASSERT_TRUE(object_cache_path(source,
                                  "clang -c s.c -O0 -o /a/x.o -MF /a/x.o.d" OBJECT_CACHE_ENV_MARK
                                  "CPATH=/opt/include",
                                  first, sizeof first));
    ASSERT_TRUE(object_cache_path(source,
                                  "clang -c s.c -O0 -o /b/y.o -MF /b/y.o.d" OBJECT_CACHE_ENV_MARK
                                  "CPATH=/opt/include",
                                  second, sizeof second));
    EXPECT_STREQ(first, second);

    /* And a different environment is a different object. */
    char third[PATH_MAX_LEN] = "";
    ASSERT_TRUE(object_cache_path(source,
                                  "clang -c s.c -O0 -o /a/x.o -MF /a/x.o.d" OBJECT_CACHE_ENV_MARK
                                  "CPATH=/usr/include",
                                  third, sizeof third));
    EXPECT_STRNE(first, third);

    sandbox_close(&at);
}

MOLTEST(object_cache_keeps_the_key_it_had_before_environments_existed) {
    /* The key of a command that says nothing about its environment must not
       move, or every object every project already had cached is orphaned by an
       upgrade. The digest below was taken from the binary that predates the
       environment being part of the key; it is a fact about released behaviour,
       not a number this test is free to choose. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char source[PATH_MAX_LEN];
    char path[PATH_MAX_LEN] = "";
    ASSERT_TRUE(dep_source(&at, source, sizeof source));
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -O0 -std=c17 -o /a/x.o -MF /a/x.o.d", path,
                                  sizeof path));

    const char *digest = strrchr(path, '-');
    ASSERT_NOT_NULL(digest);
    EXPECT_STREQ("-3433222e0598b6fe.o", digest);

    sandbox_close(&at);
}

MOLTEST(object_cache_has_no_path_for_something_it_does_not_cover) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char own[PATH_MAX_LEN];
    char path[PATH_MAX_LEN] = "";
    ASSERT_TRUE(fs_format_path(own, sizeof own, "%s/app/src/main.c", at.root));
    EXPECT_FALSE(object_cache_path(own, "clang -c main.c -o /a/x.o", path, sizeof path));

    sandbox_close(&at);
}

MOLTEST(object_cache_round_trips_an_object) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char source[PATH_MAX_LEN];
    char cached[PATH_MAX_LEN] = "";
    ASSERT_TRUE(dep_source(&at, source, sizeof source));
    ASSERT_TRUE(object_cache_path(source, "clang -c s.c -o /a/x.o", cached, sizeof cached));

    char object[PATH_MAX_LEN];
    char restored[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(object, sizeof object, "%s/build/x.o", at.root));
    ASSERT_TRUE(fs_format_path(restored, sizeof restored, "%s/other/x.o", at.root));

    /* Nothing to take before anything was put. */
    EXPECT_FALSE(object_cache_take(cached, restored));

    char dir[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(dir, sizeof dir, "%s/build", at.root));
    ASSERT_TRUE(fs_make_dirs(dir));
    ASSERT_TRUE(fs_write_file(object, "objectbytes"));

    object_cache_put(object, cached);
    EXPECT_TRUE(fs_path_exists(cached));

    ASSERT_TRUE(object_cache_take(cached, restored));
    char *content = fs_read_file(restored);
    ASSERT_NOT_NULL(content);
    EXPECT_STREQ("objectbytes", content);
    free(content);

    sandbox_close(&at);
}

MOLTEST(object_cache_put_is_survivable_when_there_is_nothing_to_put) {
    /* A cache that cannot be written costs a rebuild somewhere else and
       nothing more, so this must not fail a build. */
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at));

    char cached[PATH_MAX_LEN];
    char missing[PATH_MAX_LEN];
    ASSERT_TRUE(fs_format_path(cached, sizeof cached, "%s/objects/x.o", at.cache));
    ASSERT_TRUE(fs_format_path(missing, sizeof missing, "%s/nothing.o", at.root));

    object_cache_put(missing, cached);
    EXPECT_FALSE(fs_path_exists(cached));

    sandbox_close(&at);
}
