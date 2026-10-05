#include <moltest.h>

#include <molto/util/semver.h>
#include <molto/util/str_list.h>

#include <string.h>

/* Semver's ordering, which is neither string order nor numeric order. */

static int order(const char *left, const char *right) {
    semver a;
    semver b;
    if (!semver_parse(left, &a) || !semver_parse(right, &b))
        return 99; /* a parse failure must not read as "equal" */
    const int result = semver_compare(&a, &b);
    return result < 0 ? -1 : (result > 0 ? 1 : 0);
}

MOLTEST(semver_reads_the_three_components) {
    semver v;
    ASSERT_TRUE(semver_parse("1.10.3", &v));
    EXPECT_EQ(1u, v.major);
    EXPECT_EQ(10u, v.minor);
    EXPECT_EQ(3u, v.patch);
    EXPECT_STREQ("", v.prerelease);
    EXPECT_STREQ("", v.build);
}

MOLTEST(semver_reads_a_prerelease_and_build_metadata) {
    semver v;
    ASSERT_TRUE(semver_parse("1.0.0-rc.1+build.5", &v));
    EXPECT_EQ(1u, v.major);
    EXPECT_STREQ("rc.1", v.prerelease);
    EXPECT_STREQ("build.5", v.build);
}

/* The one everybody gets wrong by sorting strings. */
MOLTEST(ten_is_newer_than_nine) {
    EXPECT_EQ(1, order("1.10.0", "1.9.0"));
    EXPECT_EQ(1, order("1.0.10", "1.0.9"));
    EXPECT_EQ(1, order("10.0.0", "9.99.99"));
}

MOLTEST(components_are_weighted_left_to_right) {
    EXPECT_EQ(1, order("2.0.0", "1.99.99"));
    EXPECT_EQ(1, order("1.2.0", "1.1.99"));
    EXPECT_EQ(0, order("1.2.3", "1.2.3"));
}

/* A pre-release is a release that is not ready, so it comes first. */
MOLTEST(a_prerelease_precedes_the_release) {
    EXPECT_EQ(-1, order("1.0.0-rc.1", "1.0.0"));
    EXPECT_EQ(1, order("1.0.0", "1.0.0-rc.1"));
}

/* The ordering semver spells out, in the order it spells it. */
MOLTEST(prerelease_identifiers_compare_by_the_specified_rules) {
    EXPECT_EQ(-1, order("1.0.0-alpha", "1.0.0-alpha.1"));   /* fewer fields first */
    EXPECT_EQ(-1, order("1.0.0-alpha.1", "1.0.0-alpha.beta")); /* numeric before text */
    EXPECT_EQ(-1, order("1.0.0-alpha.beta", "1.0.0-beta"));
    EXPECT_EQ(-1, order("1.0.0-beta", "1.0.0-beta.2"));
    EXPECT_EQ(-1, order("1.0.0-beta.2", "1.0.0-beta.11")); /* 2 < 11, not "11" < "2" */
    EXPECT_EQ(-1, order("1.0.0-beta.11", "1.0.0-rc.1"));
}

/* Two versions differing only in build metadata are the same version. */
MOLTEST(build_metadata_does_not_order) {
    EXPECT_EQ(0, order("1.0.0+a", "1.0.0+b"));
    EXPECT_EQ(0, order("1.0.0", "1.0.0+b"));
    EXPECT_EQ(0, order("1.0.0-rc.1+a", "1.0.0-rc.1+z"));
}

/* Refused here, or sorted wrong somewhere later. */
MOLTEST(semver_refuses_what_is_not_a_version) {
    semver v;
    EXPECT_FALSE(semver_parse("1.0", &v));
    EXPECT_FALSE(semver_parse("1.0.0.0", &v));
    EXPECT_FALSE(semver_parse("v1.0.0", &v));
    EXPECT_FALSE(semver_parse("1.0.x", &v));
    EXPECT_FALSE(semver_parse("^1.0.0", &v));
    EXPECT_FALSE(semver_parse("", &v));
    EXPECT_FALSE(semver_parse("1.0.0-", &v));
    EXPECT_FALSE(semver_parse("1.0.0-a..b", &v));
    /* Leading zeroes would make one version two spellings that sort apart. */
    EXPECT_FALSE(semver_parse("1.01.0", &v));
}

MOLTEST(the_highest_of_a_list_is_the_newest) {
    const char *const versions[] = {"1.9.0", "1.10.0", "1.2.30", "0.99.99"};
    char out[64] = "";
    ASSERT_TRUE(semver_highest(versions, 4, out, sizeof out));
    EXPECT_STREQ("1.10.0", out);
}

/* A published pre-release is not what `molto add <name>` should pick, but it is
   still ordered correctly against the releases around it. */
MOLTEST(the_highest_prefers_a_release_over_its_prerelease) {
    const char *const versions[] = {"2.0.0-rc.1", "1.9.0"};
    char out[64] = "";
    ASSERT_TRUE(semver_highest(versions, 2, out, sizeof out));
    EXPECT_STREQ("2.0.0-rc.1", out);

    const char *const both[] = {"2.0.0-rc.1", "2.0.0"};
    ASSERT_TRUE(semver_highest(both, 2, out, sizeof out));
    EXPECT_STREQ("2.0.0", out);
}

/* A registry serves strings it never interprets, so a list may hold something
   that is not a version. One odd entry must not lose the good ones. */
MOLTEST(unparseable_entries_are_skipped_not_fatal) {
    const char *const versions[] = {"13.2.0-x86_64-linux", "not-a-version", "1.2.3"};
    char out[64] = "";
    ASSERT_TRUE(semver_highest(versions, 3, out, sizeof out));
    EXPECT_STREQ("1.2.3", out);

    const char *const none[] = {"nope", "also-nope"};
    EXPECT_FALSE(semver_highest(none, 2, out, sizeof out));
}

/* --- ordering a whole list --- */

static void push_all(str_list *list, const char *const *values, size_t count) {
    str_list_init(list);
    for (size_t i = 0; i < count; i++) {
        ASSERT_TRUE(str_list_push(list, values[i]));
    }
}

MOLTEST(sorting_puts_the_newest_first) {
    const char *const versions[] = {"1.9.0", "1.10.0", "1.2.3", "2.0.0"};
    str_list list;
    push_all(&list, versions, 4);

    EXPECT_EQ(4u, semver_sort_desc(&list));
    EXPECT_STREQ("2.0.0", str_list_get(&list, 0));
    EXPECT_STREQ("1.10.0", str_list_get(&list, 1));
    EXPECT_STREQ("1.9.0", str_list_get(&list, 2));
    EXPECT_STREQ("1.2.3", str_list_get(&list, 3));

    str_list_free(&list);
}

/* A pre-release precedes the release it leads to, so it sorts after it here. */
MOLTEST(sorting_places_a_prerelease_below_its_release) {
    const char *const versions[] = {"2.0.0-rc.1", "2.0.0", "2.0.0-alpha"};
    str_list list;
    push_all(&list, versions, 3);

    EXPECT_EQ(3u, semver_sort_desc(&list));
    EXPECT_STREQ("2.0.0", str_list_get(&list, 0));
    EXPECT_STREQ("2.0.0-rc.1", str_list_get(&list, 1));
    EXPECT_STREQ("2.0.0-alpha", str_list_get(&list, 2));

    str_list_free(&list);
}

/* What is not a version goes to the tail and is not counted: a caller reading
   the count never has to ask whether an entry parses. */
MOLTEST(sorting_keeps_what_is_not_a_version_out_of_the_count) {
    const char *const versions[] = {"nightly", "1.0.0", "13.2.0-x86_64-linux", "1.4.0"};
    str_list list;
    push_all(&list, versions, 4);

    EXPECT_EQ(2u, semver_sort_desc(&list));
    EXPECT_STREQ("1.4.0", str_list_get(&list, 0));
    EXPECT_STREQ("1.0.0", str_list_get(&list, 1));

    str_list_free(&list);
}

MOLTEST(sorting_an_empty_list_counts_nothing) {
    str_list list;
    str_list_init(&list);

    EXPECT_EQ(0u, semver_sort_desc(&list));

    str_list_free(&list);
}
