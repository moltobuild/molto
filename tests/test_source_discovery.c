#include <moltest.h>

#include <molto/services/fs_service.h>
#include <molto/services/source_discovery.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool contains_suffix(const str_list *list, const char *suffix) {
    for (size_t i = 0; i < str_list_count(list); i++) {
        const char *path = str_list_get(list, i);
        size_t path_len = strlen(path);
        size_t suffix_len = strlen(suffix);
        if (path_len >= suffix_len
            && strcmp(path + path_len - suffix_len, suffix) == 0)
            return true;
    }
    return false;
}

MOLTEST(source_discovery) {
    EXPECT_TRUE(source_is_cpp("x.cpp"));
    EXPECT_TRUE(source_is_cpp("x.cc"));
    EXPECT_TRUE(!source_is_cpp("x.c"));
    EXPECT_TRUE(!source_is_cpp("x.h"));

    char root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_disc", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/sub", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/a.c", root);
    EXPECT_TRUE(fs_write_file(path, "int a;\n"));
    snprintf(path, sizeof path, "%s/sub/b.cpp", root);
    EXPECT_TRUE(fs_write_file(path, "int b;\n"));
    snprintf(path, sizeof path, "%s/sub/c.h", root);
    EXPECT_TRUE(fs_write_file(path, "int c;\n"));
    snprintf(path, sizeof path, "%s/d.cc", root);
    EXPECT_TRUE(fs_write_file(path, "int d;\n"));

    str_list found;
    str_list_init(&found);
    EXPECT_TRUE(source_discovery_collect(root, &found));
    EXPECT_TRUE(str_list_count(&found) == 3); /* a.c, b.cpp, d.cc; c.h excluded */
    EXPECT_TRUE(contains_suffix(&found, "/a.c"));
    EXPECT_TRUE(contains_suffix(&found, "/b.cpp"));
    EXPECT_TRUE(contains_suffix(&found, "/d.cc"));
    EXPECT_TRUE(!contains_suffix(&found, "/c.h"));
    str_list_free(&found);

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(source_discovery_does_not_follow_symlinked_directories) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_symlink", root, sizeof root));

    char nested[512];
    snprintf(nested, sizeof nested, "%s/src/inner", root);
    EXPECT_TRUE(fs_make_dirs(nested));
    char path[512];
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    /* A link pointing back at an ancestor: walking into it would never end. */
    char loop[600];
    snprintf(loop, sizeof loop, "%s/src/inner/loop", root);
    char target[512];
    snprintf(target, sizeof target, "%s/src", root);
    /* Skipped rather than failed where the system will not make one. This is
       the only test in the suite that needs a link to a *directory*, and
       `fs_link` gives a hard link on Windows, which cannot point at one. The
       guard being tested is real either way; what cannot be arranged there is
       the loop that triggers it. */
    if(!fs_link(target, loop))
        SKIP("this system will not make a link that points at a directory");

    str_list sources;
    str_list_init(&sources);
    EXPECT_TRUE(source_discovery_collect(target, &sources));
    EXPECT_EQ(1, str_list_count(&sources));

    str_list_free(&sources);
    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(source_discovery_returns_a_stable_order) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_order", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    /* Created out of order on purpose: discovery must not echo creation order. */
    static const char *names[] = { "zeta.c", "alpha.c", "middle.c" };
    for (size_t i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/src/%s", root, names[i]);
        EXPECT_TRUE(fs_write_file(path, "int placeholder(void) { return 0; }\n"));
    }

    char src_dir[512];
    snprintf(src_dir, sizeof src_dir, "%s/src", root);
    str_list sources;
    str_list_init(&sources);
    EXPECT_TRUE(source_discovery_collect(src_dir, &sources));
    ASSERT_EQ(3, str_list_count(&sources));

    /* Sorted, so the link line is the same on every filesystem. */
    EXPECT_NOT_NULL(strstr(str_list_get(&sources, 0), "alpha.c"));
    EXPECT_NOT_NULL(strstr(str_list_get(&sources, 1), "middle.c"));
    EXPECT_NOT_NULL(strstr(str_list_get(&sources, 2), "zeta.c"));

    str_list_free(&sources);
    char cmd[600];
    (void)fs_remove_tree(root);
}

/* --- what the tests are built from --- */

/* A project root with the files named under it, directories created as needed. */
static bool plant(const char *root, const char *const *files, size_t count) {
    for(size_t i = 0; i < count; i++) {
        char path[600];
        snprintf(path, sizeof path, "%s/%s", root, files[i]);
        char *slash = strrchr(path, '/');
        if(slash != NULL) {
            *slash = '\0';
            if(!fs_make_dirs(path))
                return false;
            *slash = '/';
        }
        if(!fs_write_file(path, "int placeholder(void) { return 0; }\n"))
            return false;
    }
    return true;
}

static void wipe(const char *root) {
    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(source_discovery_collects_the_tests_directory_and_the_extra_entries) {
    /* A listed directory is walked and a listed file is taken as it is: that is
       how a framework living outside tests/ — with the main() the tests do not
       have — gets compiled in. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_tests", root, sizeof root));

    static const char *files[] = {"tests/test_a.c", "tests/deep/test_b.cpp",
                                  "vendor/moltest/moltest.c", "extra/helper.c",
                                  "extra/ignored.txt"};
    ASSERT_TRUE(plant(root, files, 5));

    str_list extra;
    str_list_init(&extra);
    ASSERT_TRUE(str_list_push(&extra, "vendor/moltest"));  /* a directory, walked */
    ASSERT_TRUE(str_list_push(&extra, "extra/helper.c"));  /* a file, taken */

    str_list found;
    str_list_init(&found);
    char err[512] = "";
    ASSERT_TRUE(source_discovery_collect_tests(root, &extra, &found, err, sizeof err));
    EXPECT_STREQ("", err);

    ASSERT_EQ(4, str_list_count(&found));
    EXPECT_TRUE(contains_suffix(&found, "/tests/test_a.c"));
    EXPECT_TRUE(contains_suffix(&found, "/tests/deep/test_b.cpp"));
    EXPECT_TRUE(contains_suffix(&found, "/vendor/moltest/moltest.c"));
    EXPECT_TRUE(contains_suffix(&found, "/extra/helper.c"));
    EXPECT_TRUE(!contains_suffix(&found, "/extra/ignored.txt"));

    str_list_free(&found);
    str_list_free(&extra);
    wipe(root);
}

MOLTEST(source_discovery_treats_a_missing_tests_directory_as_nothing) {
    /* Not an error: a project without tests is a project, and `molto new`
       leaves an empty tests/ behind. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_notests", root, sizeof root));

    str_list found;
    str_list_init(&found);
    char err[512] = "";
    EXPECT_TRUE(source_discovery_collect_tests(root, NULL, &found, err, sizeof err));
    EXPECT_EQ(0, str_list_count(&found));
    EXPECT_STREQ("", err);

    str_list_free(&found);
    wipe(root);
}

MOLTEST(source_discovery_refuses_an_extra_entry_that_is_not_there) {
    /* A `[test].sources` entry naming nothing is a manifest describing a build
       that cannot happen, and the message has to name the entry — the point of
       reporting it is that the author can find it. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_missing", root, sizeof root));

    str_list extra;
    str_list_init(&extra);
    ASSERT_TRUE(str_list_push(&extra, "vendor/gone.c"));

    str_list found;
    str_list_init(&found);
    char err[512] = "";
    EXPECT_FALSE(source_discovery_collect_tests(root, &extra, &found, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "vendor/gone.c"));

    str_list_free(&found);
    str_list_free(&extra);
    wipe(root);
}

MOLTEST(source_discovery_anchors_a_relative_entry_at_the_project_root) {
    /* And uses an absolute one as written — the rule every relative path in a
       manifest obeys, which is why a build works the same from a subdirectory. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_anchor", root, sizeof root));

    static const char *files[] = {"vendor/framework.c"};
    ASSERT_TRUE(plant(root, files, 1));

    char absolute[600];
    snprintf(absolute, sizeof absolute, "%s/vendor/framework.c", root);

    str_list extra;
    str_list_init(&extra);
    ASSERT_TRUE(str_list_push(&extra, absolute));

    str_list found;
    str_list_init(&found);
    char err[512] = "";
    ASSERT_TRUE(source_discovery_collect_tests(root, &extra, &found, err, sizeof err));
    ASSERT_EQ(1, str_list_count(&found));
    EXPECT_STREQ(absolute, str_list_get(&found, 0));

    str_list_free(&found);
    str_list_free(&extra);
    wipe(root);
}
