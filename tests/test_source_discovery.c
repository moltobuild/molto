#include "test_framework.h"
#include "tests.h"

#include <molto/services/fs_service.h>
#include <molto/services/source_discovery.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void suite_source_discovery(void) {
    CHECK(source_is_cpp("x.cpp"));
    CHECK(source_is_cpp("x.cc"));
    CHECK(!source_is_cpp("x.c"));
    CHECK(!source_is_cpp("x.h"));

    char root[] = "/tmp/molto_disc_XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    char path[512];
    snprintf(path, sizeof path, "%s/sub", root);
    CHECK(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/a.c", root);
    CHECK(fs_write_file(path, "int a;\n"));
    snprintf(path, sizeof path, "%s/sub/b.cpp", root);
    CHECK(fs_write_file(path, "int b;\n"));
    snprintf(path, sizeof path, "%s/sub/c.h", root);
    CHECK(fs_write_file(path, "int c;\n"));
    snprintf(path, sizeof path, "%s/d.cc", root);
    CHECK(fs_write_file(path, "int d;\n"));

    str_list found;
    str_list_init(&found);
    CHECK(source_discovery_collect(root, &found));
    CHECK(str_list_count(&found) == 3); /* a.c, b.cpp, d.cc; c.h excluded */
    CHECK(contains_suffix(&found, "/a.c"));
    CHECK(contains_suffix(&found, "/b.cpp"));
    CHECK(contains_suffix(&found, "/d.cc"));
    CHECK(!contains_suffix(&found, "/c.h"));
    str_list_free(&found);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
