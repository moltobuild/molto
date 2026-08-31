#include <moltest.h>

#include <molto/services/fs_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

MOLTEST(workspace) {
    char previous[4096];
    EXPECT_TRUE(getcwd(previous, sizeof previous) != NULL);

    /* A project with a nested subdirectory. */
    char root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_ws", root, sizeof root));
    char path[4200];
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"ws\"\n"));
    snprintf(path, sizeof path, "%s/src/deep", root);
    EXPECT_TRUE(fs_make_dirs(path));

    /* From a subdirectory, discovery walks up to the project root. */
    EXPECT_TRUE(chdir(path) == 0);
    char found[4096];
    EXPECT_TRUE(workspace_find_root(found, sizeof found));
    EXPECT_TRUE(strcmp(found, root) == 0);

    /* From the root itself. */
    EXPECT_TRUE(chdir(root) == 0);
    EXPECT_TRUE(workspace_find_root(found, sizeof found));
    EXPECT_TRUE(strcmp(found, root) == 0);

    /* A directory with no Project.toml anywhere above -> not a workspace. */
    char bare[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_bare", bare, sizeof bare));
    EXPECT_TRUE(chdir(bare) == 0);
    EXPECT_TRUE(!workspace_find_root(found, sizeof found));

    EXPECT_TRUE(chdir(previous) == 0);
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "rm -rf %s %s", root, bare);
    (void)system(cmd);
}
