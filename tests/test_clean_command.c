#include <moltest.h>

#include <molto/build/profile.h>
#include <molto/commands/clean_command.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* A built workspace, entered so the command finds it by discovery. */
typedef struct {
    char root[64];
    char previous[4096];
    char build_dir[128];
    char bin_dir[128];
} built_workspace;

static bool built_workspace_setup(built_workspace *workspace) {
    if (!moltest_temp_dir("molto_clean", workspace->root, sizeof workspace->root))
        return false;

    char path[256];
    snprintf(path, sizeof path, "%s/src", workspace->root);
    if (!fs_make_dirs(path))
        return false;
    snprintf(path, sizeof path, "%s/Project.toml", workspace->root);
    if (!fs_write_file(path, "[package]\nname = \"sweeper\"\n"))
        return false;
    snprintf(path, sizeof path, "%s/src/main.c", workspace->root);
    if (!fs_write_file(path, "int main(void) { return 0; }\n"))
        return false;
    if (build_project(workspace->root, profile_debug, NULL, false, 0, NULL, 0) != exit_ok)
        return false;

    snprintf(workspace->build_dir, sizeof workspace->build_dir, "%s/build", workspace->root);
    snprintf(workspace->bin_dir, sizeof workspace->bin_dir, "%s/.bin", workspace->root);
    return getcwd(workspace->previous, sizeof workspace->previous) != NULL
        && chdir(workspace->root) == 0;
}

static void built_workspace_teardown(built_workspace *workspace) {
    (void)chdir(workspace->previous);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", workspace->root);
    (void)system(cmd);
}

MOLTEST(clean_removes_build_output_but_keeps_the_incremental_state) {
    built_workspace workspace;
    ASSERT_TRUE(built_workspace_setup(&workspace));
    ASSERT_TRUE(fs_path_exists(workspace.build_dir));
    ASSERT_TRUE(fs_path_exists(workspace.bin_dir));

    EXPECT_EQ(exit_ok, clean_command_run(false));
    EXPECT_FALSE(fs_path_exists(workspace.build_dir));
    EXPECT_TRUE(fs_path_exists(workspace.bin_dir));

    built_workspace_teardown(&workspace);
}

MOLTEST(clean_all_also_removes_the_incremental_state) {
    built_workspace workspace;
    ASSERT_TRUE(built_workspace_setup(&workspace));

    EXPECT_EQ(exit_ok, clean_command_run(true));
    EXPECT_FALSE(fs_path_exists(workspace.build_dir));
    EXPECT_FALSE(fs_path_exists(workspace.bin_dir));

    /* The project itself is untouched: only what Molto produced is gone. */
    char manifest[128];
    snprintf(manifest, sizeof manifest, "%s/Project.toml", workspace.root);
    EXPECT_TRUE(fs_path_exists(manifest));
    char source[128];
    snprintf(source, sizeof source, "%s/src/main.c", workspace.root);
    EXPECT_TRUE(fs_path_exists(source));

    built_workspace_teardown(&workspace);
}

MOLTEST(clean_on_an_already_clean_workspace_succeeds) {
    built_workspace workspace;
    ASSERT_TRUE(built_workspace_setup(&workspace));

    EXPECT_EQ(exit_ok, clean_command_run(true));
    /* Nothing left to remove is not a failure: the point is to end up without
       it, and a second run must be as harmless as the first. */
    EXPECT_EQ(exit_ok, clean_command_run(true));

    built_workspace_teardown(&workspace);
}

MOLTEST(clean_outside_a_workspace_is_a_manifest_error) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_noclean", root, sizeof root));
    char previous[4096];
    ASSERT_TRUE(getcwd(previous, sizeof previous) != NULL);
    ASSERT_TRUE(chdir(root) == 0);

    EXPECT_EQ(exit_invalid_manifest, clean_command_run(false));

    EXPECT_TRUE(chdir(previous) == 0);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
