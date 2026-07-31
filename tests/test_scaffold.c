#include <moltest.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/scaffold_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MOLTEST(scaffold) {
    char root[] = "/tmp/molto_scaffold_XXXXXX";
    EXPECT_TRUE(mkdtemp(root) != NULL);

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
