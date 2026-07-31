#include <moltest.h>

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

MOLTEST(build_service) {
    char root[] = "/tmp/molto_build_XXXXXX";
    EXPECT_TRUE(mkdtemp(root) != NULL);

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\n"
        "name = \"demo_app\"\n"
        "version = \"0.1.0\"\n"
        "[profile.debug]\n"
        "opt_level = 0\n"
        "debug_info = true\n"));
    /* Two translation units so the build fans out onto the task pool. */
    snprintf(path, sizeof path, "%s/src/util.h", root);
    EXPECT_TRUE(fs_write_file(path, "int answer(void);\n"));
    snprintf(path, sizeof path, "%s/src/util.c", root);
    EXPECT_TRUE(fs_write_file(path, "int answer(void) { return 0; }\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path,
        "#include <stdio.h>\n"
        "#include \"util.h\"\n"
        "int main(void) { printf(\"hi\\n\"); return answer(); }\n"));

    /* First build compiles and links. */
    EXPECT_TRUE(build_project(root, profile_debug, NULL, 0) == exit_ok);
    char binary[512];
    snprintf(binary, sizeof binary, "%s/build/debug/demo_app", root);
    EXPECT_TRUE(fs_path_exists(binary));
    long linked_at = mtime_of(binary);

    /* A no-op rebuild in a later second must NOT re-link (binary untouched). */
    sleep(1);
    EXPECT_TRUE(build_project(root, profile_debug, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(binary) == linked_at);

    /* Touching a header recompiles the units that include it. main.c includes
       util.h, so editing util.h must rebuild main.c and re-link. */
    sleep(1);
    char util_header[512];
    snprintf(util_header, sizeof util_header, "%s/src/util.h", root);
    EXPECT_TRUE(fs_write_file(util_header, "int answer(void); /* touched */\n"));
    EXPECT_TRUE(build_project(root, profile_debug, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(binary) > linked_at);
    linked_at = mtime_of(binary); /* rebase for the next step */

    /* Changing a source triggers recompilation and a re-link. */
    sleep(1);
    char main_path[512];
    snprintf(main_path, sizeof main_path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(main_path,
        "#include <stdio.h>\n"
        "int main(void) { printf(\"hi again\\n\"); return 0; }\n"));
    EXPECT_TRUE(build_project(root, profile_debug, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(binary) > linked_at);

    /* The produced binary runs successfully. */
    char cmd[600];
    snprintf(cmd, sizeof cmd, "%s > /dev/null 2>&1", binary);
    EXPECT_TRUE(system(cmd) == 0);

    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);

    /* A directory without Project.toml is an invalid-manifest error. */
    char empty[] = "/tmp/molto_empty_XXXXXX";
    EXPECT_TRUE(mkdtemp(empty) != NULL);
    EXPECT_TRUE(build_project(empty, profile_debug, NULL, 0) == exit_invalid_manifest);
    snprintf(cmd, sizeof cmd, "rm -rf %s", empty);
    (void)system(cmd);

    /* [target] std + link libraries are applied: this program calls sqrt() from
       libm, so it only links when `link = ["m"]` adds -lm. */
    char lib_root[] = "/tmp/molto_target_XXXXXX";
    EXPECT_TRUE(mkdtemp(lib_root) != NULL);
    snprintf(path, sizeof path, "%s/src", lib_root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", lib_root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"needs_libm\"\n"
        "[target]\nstd = \"c11\"\nlink = [\"m\"]\n"));
    snprintf(path, sizeof path, "%s/src/main.c", lib_root);
    EXPECT_TRUE(fs_write_file(path,
        "#include <math.h>\n"
        "int main(void) { return (int)sqrt(4.0) - 2; }\n"));
    EXPECT_TRUE(build_project(lib_root, profile_debug, NULL, 0) == exit_ok);
    snprintf(cmd, sizeof cmd, "rm -rf %s", lib_root);
    (void)system(cmd);

    /* Changing a profile setting recompiles even when the source is unchanged
       (command fingerprint). */
    char fp_root[] = "/tmp/molto_fp_XXXXXX";
    EXPECT_TRUE(mkdtemp(fp_root) != NULL);
    snprintf(path, sizeof path, "%s/src", fp_root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/src/main.c", fp_root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));
    snprintf(path, sizeof path, "%s/Project.toml", fp_root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"fp\"\n[profile.debug]\nopt_level = 0\ndebug_info = true\n"));
    EXPECT_TRUE(build_project(fp_root, profile_debug, NULL, 0) == exit_ok);
    char fp_obj[512];
    snprintf(fp_obj, sizeof fp_obj, "%s/build/debug/obj/src/main.c.o", fp_root);
    long compiled_at = mtime_of(fp_obj);
    sleep(1);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"fp\"\n[profile.debug]\nopt_level = 2\ndebug_info = true\n"));
    EXPECT_TRUE(build_project(fp_root, profile_debug, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(fp_obj) > compiled_at); /* recompiled due to changed opt_level */
    snprintf(cmd, sizeof cmd, "rm -rf %s", fp_root);
    (void)system(cmd);

    /* [target].defines reach the compiler: main uses ANSWER, so it only
       compiles when -DANSWER=42 is passed. */
    char def_root[] = "/tmp/molto_def_XXXXXX";
    EXPECT_TRUE(mkdtemp(def_root) != NULL);
    snprintf(path, sizeof path, "%s/src", def_root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", def_root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"def\"\n[target]\ndefines = [\"ANSWER=42\"]\n"));
    snprintf(path, sizeof path, "%s/src/main.c", def_root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return ANSWER - 42; }\n"));
    EXPECT_TRUE(build_project(def_root, profile_debug, NULL, 0) == exit_ok);
    snprintf(cmd, sizeof cmd, "rm -rf %s", def_root);
    (void)system(cmd);
}
