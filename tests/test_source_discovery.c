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

    char root[] = "/tmp/molto_disc_XXXXXX";
    EXPECT_TRUE(mkdtemp(root) != NULL);

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
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(source_discovery_does_not_follow_symlinked_directories) {
    char root[] = "/tmp/molto_symlink_XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);

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
    ASSERT_TRUE(symlink(target, loop) == 0);

    str_list sources;
    str_list_init(&sources);
    EXPECT_TRUE(source_discovery_collect(target, &sources));
    EXPECT_EQ(1, str_list_count(&sources));

    str_list_free(&sources);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(source_discovery_returns_a_stable_order) {
    char root[] = "/tmp/molto_order_XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);

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
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
