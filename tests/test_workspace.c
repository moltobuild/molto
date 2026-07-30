#include "test_framework.h"
#include "tests.h"

#include <molto/services/fs_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void suite_workspace(void) {
    char previous[4096];
    CHECK(getcwd(previous, sizeof previous) != NULL);

    /* A project with a nested subdirectory. */
    char root[] = "/tmp/molto_ws_XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char path[4200];
    snprintf(path, sizeof path, "%s/Project.toml", root);
    CHECK(fs_write_file(path, "[package]\nname = \"ws\"\n"));
    snprintf(path, sizeof path, "%s/src/deep", root);
    CHECK(fs_make_dirs(path));

    /* From a subdirectory, discovery walks up to the project root. */
    CHECK(chdir(path) == 0);
    char found[4096];
    CHECK(workspace_find_root(found, sizeof found));
    CHECK(strcmp(found, root) == 0);

    /* From the root itself. */
    CHECK(chdir(root) == 0);
    CHECK(workspace_find_root(found, sizeof found));
    CHECK(strcmp(found, root) == 0);

    /* A directory with no Project.toml anywhere above -> not a workspace. */
    char bare[] = "/tmp/molto_bare_XXXXXX";
    CHECK(mkdtemp(bare) != NULL);
    CHECK(chdir(bare) == 0);
    CHECK(!workspace_find_root(found, sizeof found));

    CHECK(chdir(previous) == 0);
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "rm -rf %s %s", root, bare);
    (void)system(cmd);
}
