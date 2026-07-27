#include "test_framework.h"
#include "tests.h"

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static long mtime_of(const char *path) {
    struct stat info;
    if (stat(path, &info) != 0)
        return -1;
    return (long)info.st_mtime;
}

void suite_build_service(void) {
    char root[] = "/tmp/molto_build_XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    CHECK(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    CHECK(fs_write_file(path,
        "[package]\n"
        "name = \"demo_app\"\n"
        "version = \"0.1.0\"\n"
        "[profile.debug]\n"
        "opt_level = 0\n"
        "debug_info = true\n"));
    /* Two translation units so the build fans out onto the task pool. */
    snprintf(path, sizeof path, "%s/src/util.h", root);
    CHECK(fs_write_file(path, "int answer(void);\n"));
    snprintf(path, sizeof path, "%s/src/util.c", root);
    CHECK(fs_write_file(path, "int answer(void) { return 0; }\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    CHECK(fs_write_file(path,
        "#include <stdio.h>\n"
        "#include \"util.h\"\n"
        "int main(void) { printf(\"hi\\n\"); return answer(); }\n"));

    /* First build compiles and links. */
    CHECK(build_project(root, profile_debug, NULL, 0) == exit_ok);
    char binary[512];
    snprintf(binary, sizeof binary, "%s/build/debug/demo_app", root);
    CHECK(fs_path_exists(binary));
    long linked_at = mtime_of(binary);

    /* A no-op rebuild in a later second must NOT re-link (binary untouched). */
    sleep(1);
    CHECK(build_project(root, profile_debug, NULL, 0) == exit_ok);
    CHECK(mtime_of(binary) == linked_at);

    /* Touching a header recompiles the units that include it. main.c includes
       util.h, so editing util.h must rebuild main.c and re-link. */
    sleep(1);
    char util_header[512];
    snprintf(util_header, sizeof util_header, "%s/src/util.h", root);
    CHECK(fs_write_file(util_header, "int answer(void); /* touched */\n"));
    CHECK(build_project(root, profile_debug, NULL, 0) == exit_ok);
    CHECK(mtime_of(binary) > linked_at);
    linked_at = mtime_of(binary); /* rebase for the next step */

    /* Changing a source triggers recompilation and a re-link. */
    sleep(1);
    char main_path[512];
    snprintf(main_path, sizeof main_path, "%s/src/main.c", root);
    CHECK(fs_write_file(main_path,
        "#include <stdio.h>\n"
        "int main(void) { printf(\"hi again\\n\"); return 0; }\n"));
    CHECK(build_project(root, profile_debug, NULL, 0) == exit_ok);
    CHECK(mtime_of(binary) > linked_at);

    /* The produced binary runs successfully. */
    char cmd[600];
    snprintf(cmd, sizeof cmd, "%s > /dev/null 2>&1", binary);
    CHECK(system(cmd) == 0);

    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);

    /* A directory without Project.toml is an invalid-manifest error. */
    char empty[] = "/tmp/molto_empty_XXXXXX";
    CHECK(mkdtemp(empty) != NULL);
    CHECK(build_project(empty, profile_debug, NULL, 0) == exit_invalid_manifest);
    snprintf(cmd, sizeof cmd, "rm -rf %s", empty);
    (void)system(cmd);
}
