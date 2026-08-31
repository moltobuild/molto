#include <moltest.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/scaffold_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MOLTEST(scaffold) {
    char root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_scaffold", root, sizeof root));

    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    EXPECT_TRUE(scaffold_project(project, "demo") == exit_ok);

    char path[700];
    /* Layout: Project.toml, src/, tests/, and a starter src/main.c. */
    snprintf(path, sizeof path, "%s/Project.toml", project);
    EXPECT_TRUE(fs_path_exists(path));
    snprintf(path, sizeof path, "%s/src", project);
    EXPECT_TRUE(fs_is_dir(path));
    snprintf(path, sizeof path, "%s/tests", project);
    EXPECT_TRUE(fs_is_dir(path));

    snprintf(path, sizeof path, "%s/src/main.c", project);
    EXPECT_TRUE(fs_path_exists(path));
    char *main_c = fs_read_file(path);
    EXPECT_TRUE(main_c != NULL);
    if (main_c != NULL) {
        EXPECT_TRUE(strstr(main_c, "int main(void)") != NULL);
        EXPECT_TRUE(strstr(main_c, "Hello, world!") != NULL);
        free(main_c);
    }

    /* Re-scaffolding must not clobber an existing main.c (and reports the
       manifest already exists). */
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 7; }\n"));
    EXPECT_TRUE(scaffold_project(project, "demo") == exit_invalid_manifest);
    char *kept = fs_read_file(path);
    EXPECT_TRUE(kept != NULL && strstr(kept, "return 7") != NULL);
    free(kept);

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(scaffold_creates_the_include_directory_the_manifest_declares) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_include", root, sizeof root));

    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    ASSERT_TRUE(scaffold_project(project, "demo") == exit_ok);

    /* The generated manifest declares include = ["include"], so the directory
       has to exist: a manifest that points at nothing is worse than one that
       says nothing. */
    char path[700];
    snprintf(path, sizeof path, "%s/include", project);
    EXPECT_TRUE(fs_is_dir(path));

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(scaffold_ignores_the_directories_molto_owns) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_ignore", root, sizeof root));

    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    ASSERT_TRUE(scaffold_project(project, "demo") == exit_ok);

    /* Without this, `git add -A` on a fresh project commits the build output
       and the workspace database, which is binary and changes on every build. */
    char path[700];
    snprintf(path, sizeof path, "%s/.gitignore", project);
    ASSERT_TRUE(fs_path_exists(path));
    char *ignore = fs_read_file(path);
    ASSERT_NOT_NULL(ignore);
    EXPECT_NOT_NULL(strstr(ignore, "/build/"));
    EXPECT_NOT_NULL(strstr(ignore, "/.bin/"));
    /* And the compilation database, which every build rewrites from the
       manifest and the tree: committing it would put one developer's absolute
       paths in everyone else's checkout. */
    EXPECT_NOT_NULL(strstr(ignore, "/compile_commands.json"));
    free(ignore);

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(scaffold_keeps_an_existing_gitignore) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_keepignore", root, sizeof root));

    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    ASSERT_TRUE(fs_make_dirs(project));

    /* A project being adopted may already have one: it is the user's file. */
    char path[700];
    snprintf(path, sizeof path, "%s/.gitignore", project);
    ASSERT_TRUE(fs_write_file(path, "*.log\n"));

    ASSERT_TRUE(scaffold_project(project, "demo") == exit_ok);

    char *ignore = fs_read_file(path);
    ASSERT_NOT_NULL(ignore);
    EXPECT_NOT_NULL(strstr(ignore, "*.log"));
    EXPECT_NULL(strstr(ignore, "/build/"));
    free(ignore);

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
