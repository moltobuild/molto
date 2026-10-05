#include <moltest.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>

#include <stdio.h>

/* The coverage profile, built for real (RFC-0019).
 *
 * The IR tests say the flag is on the lines; this says the compiler took it.
 * An instrumented compile writes a `.gcno` beside each object, so its presence
 * under build/coverage/ is the instrumentation itself, and its absence under
 * build/debug/ is the other half: one profile's flags stay in that profile. */

#define PATH_LEN 512

static bool write_project(const char *root) {
    char path[PATH_LEN];
    return fs_format_path(path, sizeof path, "%s/src", root) && fs_make_dirs(path) &&
           fs_format_path(path, sizeof path, "%s/src/lib.c", root) &&
           fs_write_file(path, "int lib_answer(int x) {\n    return x > 0 ? 1 : 0;\n}\n") &&
           fs_format_path(path, sizeof path, "%s/src/main.c", root) &&
           fs_write_file(path, "int lib_answer(int x);\n"
                               "int main(void) { return lib_answer(1) == 1 ? 0 : 1; }\n") &&
           fs_format_path(path, sizeof path, "%s/Project.toml", root) &&
           fs_write_file(path, "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                               "[target]\nstd = \"c17\"\n");
}

DESCRIBE(the_coverage_profile_instruments_the_build) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_coverage", root, sizeof root));
    ASSERT_TRUE(write_project(root));

    ASSERT_EQ(exit_ok, build_project(root, profile_coverage, NULL, false, 0, NULL, 0));
    char notes[PATH_LEN];
    ASSERT_TRUE(fs_format_path(notes, sizeof notes, "%s/build/coverage/obj/src/lib.c.gcno", root));
    EXPECT_TRUE(fs_path_exists(notes));

    ASSERT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));
    ASSERT_TRUE(fs_format_path(notes, sizeof notes, "%s/build/debug/obj/src/lib.c.gcno", root));
    EXPECT_FALSE(fs_path_exists(notes));

    (void)fs_remove_tree(root);
}
