#include "test_framework.h"
#include "tests.h"

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/scaffold_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void suite_scaffold(void) {
    char root[] = "/tmp/molto_scaffold_XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    CHECK(scaffold_project(project, "demo") == exit_ok);

    char path[700];
    /* Layout: Project.toml, src/, tests/, and a starter src/main.c. */
    snprintf(path, sizeof path, "%s/Project.toml", project);
    CHECK(fs_path_exists(path));
    snprintf(path, sizeof path, "%s/src", project);
    CHECK(fs_is_dir(path));
    snprintf(path, sizeof path, "%s/tests", project);
    CHECK(fs_is_dir(path));

    snprintf(path, sizeof path, "%s/src/main.c", project);
    CHECK(fs_path_exists(path));
    char *main_c = fs_read_file(path);
    CHECK(main_c != NULL);
    if (main_c != NULL) {
        CHECK(strstr(main_c, "int main(void)") != NULL);
        CHECK(strstr(main_c, "Hello, world!") != NULL);
        free(main_c);
    }

    /* Re-scaffolding must not clobber an existing main.c (and reports the
       manifest already exists). */
    CHECK(fs_write_file(path, "int main(void) { return 7; }\n"));
    CHECK(scaffold_project(project, "demo") == exit_invalid_manifest);
    char *kept = fs_read_file(path);
    CHECK(kept != NULL && strstr(kept, "return 7") != NULL);
    free(kept);

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
