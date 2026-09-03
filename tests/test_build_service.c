#include <moltest.h>

#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/util/json.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Nanosecond precision: without it, a re-link within the same second would look
   unchanged. */
static int64_t mtime_of(const char *path) {
    int64_t when = -1;
    (void)fs_mtime_ns(path, &when);
    return when;
}

MOLTEST(build_service) {
    char root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_build", root, sizeof root));

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
    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    char binary[512];
    snprintf(binary, sizeof binary, "%s/build/debug/demo_app" FS_EXECUTABLE_SUFFIX, root);
    EXPECT_TRUE(fs_path_exists(binary));
    int64_t linked_at = mtime_of(binary);

    /* A no-op rebuild must NOT re-link (the binary is left untouched). */
    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(binary) == linked_at);

    /* Touching a header recompiles the units that include it. main.c includes
       util.h, so editing util.h must rebuild main.c and re-link. */
    char util_header[512];
    snprintf(util_header, sizeof util_header, "%s/src/util.h", root);
    EXPECT_TRUE(fs_write_file(util_header, "int answer(void); /* touched */\n"));
    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(binary) > linked_at);
    linked_at = mtime_of(binary); /* rebase for the next step */

    /* Changing a source triggers recompilation and a re-link. */
    char main_path[512];
    snprintf(main_path, sizeof main_path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(main_path,
        "#include <stdio.h>\n"
        "int main(void) { printf(\"hi again\\n\"); return 0; }\n"));
    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(binary) > linked_at);

    /* The produced binary runs successfully.

       Through `process_capture_all` rather than `system`, which asked a shell
       to do it: on Windows that shell is `cmd.exe`, which reads the `/` in an
       absolute path as the start of an option and answers "The system cannot
       find the path specified" -- and `> /dev/null` names a file that does not
       exist there either. Capturing is also what silences it, with no
       redirection to write in a syntax that differs per platform. */
    char output[512] = "";
    const char *const run[] = { binary, NULL };
    EXPECT_EQ(0, process_capture_all(run, NULL, 0, output, sizeof output, NULL));

    (void)fs_remove_tree(root);

    /* A directory without Project.toml is an invalid-manifest error. */
    char empty[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_empty", empty, sizeof empty));
    EXPECT_TRUE(build_project(empty, profile_debug, NULL, false, 0, NULL, 0) == exit_invalid_manifest);
    (void)fs_remove_tree(empty);

    /* [target] std + link libraries are applied: this program calls sqrt() from
       libm, so it only links when `link = ["m"]` adds -lm. */
    char lib_root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_target", lib_root, sizeof lib_root));
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
    EXPECT_TRUE(build_project(lib_root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    (void)fs_remove_tree(lib_root);

    /* Changing a profile setting recompiles even when the source is unchanged
       (command fingerprint). */
    char fp_root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_fp", fp_root, sizeof fp_root));
    snprintf(path, sizeof path, "%s/src", fp_root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/src/main.c", fp_root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));
    snprintf(path, sizeof path, "%s/Project.toml", fp_root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"fp\"\n[profile.debug]\nopt_level = 0\ndebug_info = true\n"));
    EXPECT_TRUE(build_project(fp_root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    char fp_obj[512];
    snprintf(fp_obj, sizeof fp_obj, "%s/build/debug/obj/src/main.c.o", fp_root);
    int64_t compiled_at = mtime_of(fp_obj);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"fp\"\n[profile.debug]\nopt_level = 2\ndebug_info = true\n"));
    EXPECT_TRUE(build_project(fp_root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(fp_obj) > compiled_at); /* recompiled due to changed opt_level */
    (void)fs_remove_tree(fp_root);

    /* [target].defines reach the compiler: main uses ANSWER, so it only
       compiles when -DANSWER=42 is passed. */
    char def_root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_def", def_root, sizeof def_root));
    snprintf(path, sizeof path, "%s/src", def_root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", def_root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"def\"\n[target]\ndefines = [\"ANSWER=42\"]\n"));
    snprintf(path, sizeof path, "%s/src/main.c", def_root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return ANSWER - 42; }\n"));
    EXPECT_TRUE(build_project(def_root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    (void)fs_remove_tree(def_root);
}

MOLTEST(build_keeps_the_units_that_compiled_when_another_fails) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_partial", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"partial\"\n"));
    snprintf(path, sizeof path, "%s/src/good.c", root);
    EXPECT_TRUE(fs_write_file(path, "int good(void) { return 0; }\n"));
    snprintf(path, sizeof path, "%s/src/bad.c", root);
    EXPECT_TRUE(fs_write_file(path, "this is not valid C\n"));

    /* The build fails because of bad.c... */
    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_build_failure);

    /* ...but good.c did compile, and its object is recorded as up to date. */
    char good_object[512];
    snprintf(good_object, sizeof good_object, "%s/build/debug/obj/src/good.c.o", root);
    ASSERT_TRUE(fs_path_exists(good_object));
    int64_t compiled_at = mtime_of(good_object);

    /* A second attempt only retries the broken unit: the good object is left
       alone instead of being thrown away and rebuilt. */
    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_build_failure);
    EXPECT_TRUE(mtime_of(good_object) == compiled_at);

    /* No stale depfile is left behind for the unit that failed. */
    char bad_depfile[512];
    snprintf(bad_depfile, sizeof bad_depfile, "%s/build/debug/obj/src/bad.c.o.d", root);
    EXPECT_FALSE(fs_path_exists(bad_depfile));

    /* Fixing the broken unit completes the build. */
    snprintf(path, sizeof path, "%s/src/bad.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));
    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(good_object) == compiled_at); /* still untouched */

    char cmd[600];
    (void)fs_remove_tree(root);
}

/* What the reader is left with when a unit does not compile.
 *
 * The compiler's output is captured rather than inherited, which is what lets
 * it be framed — and which means nothing reaches the terminal unless this code
 * puts it there. That makes the two cases below the whole contract: a unit that
 * failed says so, and a unit that merely warned says that instead. */
MOLTEST(a_unit_that_fails_is_framed_with_the_line_it_failed_on) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_framed", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"framed\"\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "struct user { int id; };\n"
                                    "int main(void) {\n"
                                    "    struct user u = {0};\n"
                                    "    return u.name;\n"
                                    "}\n"));

    FILE *said = tmpfile();
    ASSERT_NOT_NULL(said);
    build_report *report = build_report_create(said);
    ASSERT_NOT_NULL(report);
    EXPECT_EQ(exit_build_failure,
              build_project_with(root, profile_debug, NULL, false, 0, NULL, 0, report));

    char text[8192] = "";
    (void)fflush(said);
    rewind(said);
    text[fread(text, 1, sizeof text - 1, said)] = '\0';

    EXPECT_NOT_NULL(strstr(text, "✗ Failed to compile `main.c`"));
    EXPECT_NOT_NULL(strstr(text, "src/main.c:4"));
    EXPECT_NOT_NULL(strstr(text, "return u.name;")); /* the line, read back off disk */
    EXPECT_NOT_NULL(strstr(text, "^"));
    EXPECT_NOT_NULL(strstr(text, "= compiler: "));

    build_report_destroy(report);
    (void)fclose(said);
    char cmd[600];
    (void)fs_remove_tree(root);
}

/* A build that succeeds still has to hand over what the compiler said about
   it. Capturing the output and printing it only on failure would make every
   warning in every green build disappear. */
MOLTEST(a_unit_that_only_warned_still_says_so_and_still_succeeds) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_warned", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"warned\"\n"
                                    "\n[target]\nflags = [\"-Wall\"]\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) {\n    int tmp = 0;\n    return 0;\n}\n"));

    FILE *said = tmpfile();
    ASSERT_NOT_NULL(said);
    build_report *report = build_report_create(said);
    ASSERT_NOT_NULL(report);
    EXPECT_EQ(exit_ok, build_project_with(root, profile_debug, NULL, false, 0, NULL, 0, report));

    char text[8192] = "";
    (void)fflush(said);
    rewind(said);
    text[fread(text, 1, sizeof text - 1, said)] = '\0';

    EXPECT_NOT_NULL(strstr(text, "⚠ Warnings compiling `main.c`"));
    EXPECT_NOT_NULL(strstr(text, "unused variable"));
    EXPECT_NULL(strstr(text, "✗"));

    build_report_destroy(report);
    (void)fclose(said);
    char cmd[600];
    (void)fs_remove_tree(root);
}

/* A source under `~/.molto/cache/sources/…` is eighty columns saying no more
   than the coordinate on the line above, so the footer leaves it out. A path
   dependency inside the project is the opposite: it is somewhere the reader
   can go and look, and it is named the way they would type it. */
MOLTEST(a_dependency_inside_the_project_is_named_where_the_reader_can_find_it) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_depframe", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/modules/database/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));

    snprintf(path, sizeof path, "%s/modules/database/recipe.toml", root);
    EXPECT_TRUE(fs_write_file(path, "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                    "name = \"database\"\nversion = \"1.2.0\"\ntarget = \"any\"\n"
                                    "[artifacts]\ntype = \"source\"\n"
                                    "sources = [\"src/database.c\"]\ninclude = [\"src\"]\n"));
    snprintf(path, sizeof path, "%s/modules/database/src/database.c", root);
    EXPECT_TRUE(fs_write_file(path, "struct user { int id; };\n"
                                    "int db_lookup(struct user *u) {\n"
                                    "    return u->name;\n"
                                    "}\n"));
    /* Absolute, because a relative one is resolved against the directory molto
       was invoked from and this suite runs from the repository root. What the
       footer has to show is still the short form. */
    char manifest[1024];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[target]\nstd = \"c17\"\n"
             "[deps]\ndatabase = { path = \"%s/modules/database\" }\n",
             root);
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, manifest));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    FILE *said = tmpfile();
    ASSERT_NOT_NULL(said);
    build_report *report = build_report_create(said);
    ASSERT_NOT_NULL(report);
    EXPECT_EQ(exit_build_failure,
              build_project_with(root, profile_debug, NULL, false, 0, NULL, 0, report));

    char text[8192] = "";
    (void)fflush(said);
    rewind(said);
    text[fread(text, 1, sizeof text - 1, said)] = '\0';

    EXPECT_NOT_NULL(strstr(text, "= dependency: database"));
    EXPECT_NOT_NULL(strstr(text, "= source: modules/database\n"));
    /* Named relative to its own root, not to whoever is building it. */
    EXPECT_NOT_NULL(strstr(text, "Failed to compile `database.c`"));
    EXPECT_NOT_NULL(strstr(text, "modules/database/src/database.c:3"));

    build_report_destroy(report);
    (void)fclose(said);
    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(build_compiles_cpp_sources_with_the_cpp_driver) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_cpp", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    /* Asks for GCC on purpose. Clang picks the newest GCC installation for its
       C++ headers, and a machine with gcc-12 but no g++-12 leaves it unable to
       include <string> — a real property of the host, not of this build. */
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"cpp_app\"\n"
        "[target]\ncompiler = \"gcc\"\ncpp_std = \"c++17\"\n"));
    /* Uses <string> and a C++17 feature, so it only builds when the C++ driver
       and cpp_std are both applied. */
    snprintf(path, sizeof path, "%s/src/main.cpp", root);
    EXPECT_TRUE(fs_write_file(path,
        "#include <string>\n"
        "int main() {\n"
        "    if (auto text = std::string(\"molto\"); text.size() == 5) return 0;\n"
        "    return 1;\n"
        "}\n"));

    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);

    char binary[512];
    snprintf(binary, sizeof binary, "%s/build/debug/cpp_app" FS_EXECUTABLE_SUFFIX, root);
    ASSERT_TRUE(fs_path_exists(binary));
    char output[512] = "";
    const char *const run[] = { binary, NULL };
    EXPECT_EQ(0, process_capture_all(run, NULL, 0, output, sizeof output, NULL));

    (void)fs_remove_tree(root);
}

MOLTEST(build_honours_the_release_profile) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_release", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"fast\"\n"
        "[profile.release]\nopt_level = 2\ndebug_info = false\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    EXPECT_TRUE(build_project(root, profile_release, NULL, false, 0, NULL, 0) == exit_ok);

    /* Each profile gets its own output tree, so debug and release coexist. */
    char release_binary[512];
    snprintf(release_binary, sizeof release_binary, "%s/build/release/fast" FS_EXECUTABLE_SUFFIX, root);
    EXPECT_TRUE(fs_path_exists(release_binary));
    char debug_binary[512];
    snprintf(debug_binary, sizeof debug_binary, "%s/build/debug/fast" FS_EXECUTABLE_SUFFIX, root);
    EXPECT_FALSE(fs_path_exists(debug_binary));

    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);
    EXPECT_TRUE(fs_path_exists(debug_binary));
    EXPECT_TRUE(fs_path_exists(release_binary));

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(build_anchors_relative_includes_at_the_project_root) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_incl", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/vendor", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/deep/nested", root);
    EXPECT_TRUE(fs_make_dirs(path));

    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"incl\"\n"
        "[target]\nstd = \"c11\"\ninclude = [\"vendor\"]\n"));
    snprintf(path, sizeof path, "%s/vendor/config.h", root);
    EXPECT_TRUE(fs_write_file(path, "#define ANSWER 0\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "#include <config.h>\nint main(void){return ANSWER;}\n"));

    /* Built from a subdirectory: `include = ["vendor"]` describes the project,
       so it must not depend on where molto happened to be invoked. */
    char previous[4096];
    ASSERT_TRUE(getcwd(previous, sizeof previous) != NULL);
    char deep[512];
    snprintf(deep, sizeof deep, "%s/deep/nested", root);
    ASSERT_TRUE(chdir(deep) == 0);

    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);

    EXPECT_TRUE(chdir(previous) == 0);
    char cmd[600];
    (void)fs_remove_tree(root);
}

/* True if `text` ends with `suffix`. */
static bool ends_with(const char *text, const char *suffix) {
    const size_t length = strlen(text), tail = strlen(suffix);
    return length >= tail && strcmp(text + length - tail, suffix) == 0;
}

/*
 * A compiler that edits its own input while it "compiles" it.
 *
 * A real program rather than a `#!/bin/sh` file, because a shebang is a thing
 * the kernel honours and CreateProcess does not: on Windows the script never
 * ran, the build failed, and the test that installed it returned before
 * restoring C_COMPILER.
 *
 * It writes the object and the dependency list first, so the build itself
 * succeeds and what is under test is what the build then records.
 */
MOLTEST_FAKE(fake_racing_compiler) {
    const char *out = NULL, *dep = NULL, *src = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (strcmp(argv[i], "-MF") == 0 && i + 1 < argc)
            dep = argv[++i];
        else if (ends_with(argv[i], ".c"))
            src = argv[i];
    }

    FILE *file;
    if (out != NULL && (file = fopen(out, "wb")) != NULL)
        (void)fclose(file);
    if (dep != NULL && src != NULL && (file = fopen(dep, "wb")) != NULL) {
        fprintf(file, "o: %s\n", src);
        (void)fclose(file);
    }
    /* The edit that gives this stub its name: the source on disk is no longer
       what the object was built from. */
    if (src != NULL && (file = fopen(src, "ab")) != NULL) {
        fprintf(file, "/* edited mid-build */\n");
        (void)fclose(file);
    }

    const char *log = moltest_fake_setting("log");
    if (log != NULL && (file = fopen(log, "ab")) != NULL) {
        fprintf(file, "x\n");
        (void)fclose(file);
    }
    return 0;
}

/* Put C_COMPILER back the way it was found. The test below points it at a
   stub compiler, and unsetting it afterwards is not the same as restoring it:
   on a machine that reaches its compiler through C_COMPILER rather than
   through pickup — which is what CI is — every later test loses it. */
static void remember_env(const char *name, char *into, size_t size, bool *had) {
    const char *existing = getenv(name);
    *had = existing != NULL;
    if (existing != NULL)
        snprintf(into, size, "%s", existing);
}

static void restore_env(const char *name, const char *saved, bool had) {
    if (had)
        (void)setenv(name, saved, 1);
    else
        (void)unsetenv(name);
}

MOLTEST(build_does_not_record_an_object_for_a_source_that_changed_while_compiling) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_build_race", root, sizeof root));
    char tools[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_build_cc", tools, sizeof tools));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    ASSERT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    ASSERT_TRUE(fs_write_file(path, "[package]\nname = \"racy\"\nversion = \"0.1.0\"\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    ASSERT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    /* A compiler that edits its own input, which is what an editor saving
       during a build looks like from here. It produces the object and the
       dependency list first, so the build itself succeeds. */
    char log[512];
    snprintf(log, sizeof log, "%s/calls", tools);
    char spec[1024];
    snprintf(spec, sizeof spec, "set log %s\nbehave fake_racing_compiler\n", log);

    char wanted[512];
    snprintf(wanted, sizeof wanted, "%s/cc", tools);
    char compiler[MOLTEST_PATH];
    ASSERT_TRUE(moltest_fake_program(wanted, spec, compiler, sizeof compiler));

    char saved_cc[4096];
    bool had_cc;
    remember_env("C_COMPILER", saved_cc, sizeof saved_cc, &had_cc);
    ASSERT_TRUE(setenv("C_COMPILER", compiler, 1) == 0);

    /* Both builds run before anything is asserted, and the environment goes
       back before that. An ASSERT returns from the test, so one placed between
       here and the restore leaves C_COMPILER pointing at this stub for every
       test that follows -- which is the failure this file's own remember_env
       comment warns about, and which turned one broken stub on Windows into
       forty-eight red cases. */
    const int first_build = build_project(root, profile_debug, NULL, false, 0, NULL, 0);
    char *first = fs_read_file(log);
    const size_t after_first = first != NULL ? strlen(first) : 0;
    free(first);

    /* The object holds a compilation of content that is no longer on disk.
       Recording it would leave nothing to rebuild it, and the next link would
       quietly take the stale object — which is how a test suite ends up
       running against code that was already changed. */
    const int second_build = build_project(root, profile_debug, NULL, false, 0, NULL, 0);
    char *second = fs_read_file(log);
    const size_t after_second = second != NULL ? strlen(second) : 0;
    const bool second_ran = second != NULL;
    free(second);

    restore_env("C_COMPILER", saved_cc, had_cc);
    (void)fs_remove_tree(root);
    (void)fs_remove_tree(tools);

    ASSERT_EQ(exit_ok, first_build);
    ASSERT_TRUE(after_first > 0);
    ASSERT_EQ(exit_ok, second_build);
    ASSERT_TRUE(second_ran);
    EXPECT_TRUE(after_second > after_first);
}

/* The whole point of scoping flags, checked by the compiler rather than by
   reading a list: every claim below is a #error that fires if the flag reached
   a translation unit it had no business reaching. Before the scopes existed,
   this project failed to build on two of them. */
MOLTEST(a_dependencys_private_flags_reach_its_own_sources_and_nothing_else) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_scope", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/alpha", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/beta", root);
    EXPECT_TRUE(fs_make_dirs(path));

    /* alpha exports one define and keeps another. */
    snprintf(path, sizeof path, "%s/alpha/recipe.toml", root);
    EXPECT_TRUE(fs_write_file(path,
        "schema = 1\nform = \"source\"\nkind = \"package\"\n"
        "name = \"alpha\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
        "[artifacts]\ntype = \"source\"\ninclude = [\".\"]\ndefines = [\"ALPHA_API=1\"]\n"
        "[artifacts.private]\ndefines = [\"ALPHA_INTERNAL=1\"]\n"));
    snprintf(path, sizeof path, "%s/alpha/alpha.h", root);
    EXPECT_TRUE(fs_write_file(path, "int alpha_answer(void);\n"));
    snprintf(path, sizeof path, "%s/alpha/alpha.c", root);
    EXPECT_TRUE(fs_write_file(path,
        "#ifndef ALPHA_INTERNAL\n#error \"a package did not get its own private define\"\n#endif\n"
        "int alpha_answer(void) { return 1; }\n"));

    /* beta names nobody, so nothing of alpha's may appear on its line. */
    snprintf(path, sizeof path, "%s/beta/recipe.toml", root);
    EXPECT_TRUE(fs_write_file(path,
        "schema = 1\nform = \"source\"\nkind = \"package\"\n"
        "name = \"beta\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
        "[artifacts]\ntype = \"source\"\ninclude = [\".\"]\ndefines = [\"BETA_API=1\"]\n"));
    snprintf(path, sizeof path, "%s/beta/beta.h", root);
    EXPECT_TRUE(fs_write_file(path, "int beta_answer(void);\n"));
    snprintf(path, sizeof path, "%s/beta/beta.c", root);
    EXPECT_TRUE(fs_write_file(path,
        "#ifdef ALPHA_INTERNAL\n#error \"a sibling's private define reached beta\"\n#endif\n"
        "#ifdef ALPHA_API\n#error \"a sibling's interface reached beta\"\n#endif\n"
        "int beta_answer(void) { return 2; }\n"));

    snprintf(path, sizeof path, "%s/Project.toml", root);
    char manifest[1024];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"scoped\"\nversion = \"0.1.0\"\n"
             "[deps]\nalpha = { path = \"%s/alpha\" }\nbeta = { path = \"%s/beta\" }\n",
             root, root);
    EXPECT_TRUE(fs_write_file(path, manifest));

    /* The consumer gets both interfaces and neither private table. */
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path,
        "#include <alpha.h>\n#include <beta.h>\n"
        "#ifdef ALPHA_INTERNAL\n#error \"a private define reached the consumer\"\n#endif\n"
        "#ifndef ALPHA_API\n#error \"an interface did not reach the consumer\"\n#endif\n"
        "#ifndef BETA_API\n#error \"an interface did not reach the consumer\"\n#endif\n"
        "int main(void) { return alpha_answer() + beta_answer() - 3; }\n"));

    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(a_recipe_may_not_put_a_directory_outside_the_build_on_the_line) {
    /* A recipe is something a remote party wrote, and its `include` list lands
       on the consumer's own compile line. Nothing checked where it pointed
       until the document was held to RFC-0013's path rule — so this is the
       whole reason a native document is validated at all. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_bounds", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/greedy", root);
    EXPECT_TRUE(fs_make_dirs(path));

    snprintf(path, sizeof path, "%s/greedy/recipe.toml", root);
    EXPECT_TRUE(fs_write_file(path, "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                    "name = \"greedy\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                                    "[artifacts]\ntype = \"source\"\nsources = [\"greedy.c\"]\n"
                                    "include = [\".\", \"../../../../../../etc\"]\n"));
    snprintf(path, sizeof path, "%s/greedy/greedy.h", root);
    EXPECT_TRUE(fs_write_file(path, "int greedy_answer(void);\n"));
    snprintf(path, sizeof path, "%s/greedy/greedy.c", root);
    EXPECT_TRUE(fs_write_file(path, "int greedy_answer(void) { return 1; }\n"));

    char manifest[1024];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
             "[target]\nstd = \"c17\"\n"
             "[deps]\ngreedy = { path = \"%s/greedy\" }\n",
             root);
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, manifest));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    EXPECT_EQ(exit_build_failure, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(a_dependency_outside_the_project_is_still_a_directory_the_build_may_read) {
    /* The other half of the same rule, and the reason the bound is a fourth one
       rather than a stricter three: a sibling checkout is outside the
       workspace, outside the build directory and outside the cache, and the
       manifest named it on purpose. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_sibling", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/app/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/sibling", root);
    EXPECT_TRUE(fs_make_dirs(path));

    snprintf(path, sizeof path, "%s/sibling/recipe.toml", root);
    EXPECT_TRUE(fs_write_file(path, "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                                    "name = \"sibling\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                                    "[artifacts]\ntype = \"source\"\nsources = [\"sibling.c\"]\n"
                                    "include = [\".\"]\n"));
    snprintf(path, sizeof path, "%s/sibling/sibling.h", root);
    EXPECT_TRUE(fs_write_file(path, "int sibling_answer(void);\n"));
    snprintf(path, sizeof path, "%s/sibling/sibling.c", root);
    EXPECT_TRUE(fs_write_file(path, "int sibling_answer(void) { return 1; }\n"));

    /* Relative, and climbing out of the project: exactly the shape the three
       bounds refuse and this one allows. */
    snprintf(path, sizeof path, "%s/app/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                                    "[target]\nstd = \"c17\"\n"
                                    "[deps]\nsibling = { path = \"../sibling\" }\n"));
    snprintf(path, sizeof path, "%s/app/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "#include <sibling.h>\n"
                                    "int main(void) { return sibling_answer() - 1; }\n"));

    char app[600];
    snprintf(app, sizeof app, "%s/app", root);
    EXPECT_EQ(exit_ok, build_project(app, profile_debug, NULL, false, 0, NULL, 0));

    char cmd[600];
    (void)fs_remove_tree(root);
}

/* A library written against an older standard is compiled against it, in a
   project that asked for a newer one. Both halves are checked by the
   preprocessor, so the test fails whichever way the standard leaks. */
MOLTEST(a_dependency_compiles_against_the_standard_its_recipe_named) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_std", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/legacy", root);
    EXPECT_TRUE(fs_make_dirs(path));

    snprintf(path, sizeof path, "%s/legacy/recipe.toml", root);
    EXPECT_TRUE(fs_write_file(path,
        "schema = 1\nform = \"source\"\nkind = \"package\"\n"
        "name = \"legacy\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
        "[artifacts]\ntype = \"source\"\ninclude = [\".\"]\nstd = \"c99\"\n"));
    snprintf(path, sizeof path, "%s/legacy/legacy.h", root);
    EXPECT_TRUE(fs_write_file(path, "int legacy_answer(void);\n"));
    snprintf(path, sizeof path, "%s/legacy/legacy.c", root);
    EXPECT_TRUE(fs_write_file(path,
        "#if __STDC_VERSION__ != 199901L\n"
        "#error \"a dependency did not get the standard its recipe named\"\n#endif\n"
        "int legacy_answer(void) { return 0; }\n"));

    snprintf(path, sizeof path, "%s/Project.toml", root);
    char manifest[1024];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"modern\"\nversion = \"0.1.0\"\n"
             "[target]\nstd = \"c11\"\n"
             "[deps]\nlegacy = { path = \"%s/legacy\" }\n",
             root);
    EXPECT_TRUE(fs_write_file(path, manifest));

    /* The consumer keeps its own: a dependency's standard is about its sources
       and travels no further than they do. */
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path,
        "#include <legacy.h>\n"
        "#if __STDC_VERSION__ != 201112L\n"
        "#error \"a dependency's standard reached the consumer\"\n#endif\n"
        "int main(void) { return legacy_answer(); }\n"));

    EXPECT_TRUE(build_project(root, profile_debug, NULL, false, 0, NULL, 0) == exit_ok);

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(build_recompiles_when_the_env_changes) {
    /* [env] reaches the compiler and the linker, so it is part of what an
       object and a binary were built from. It used to reach neither
       fingerprint, which left the build mixing objects from two environments
       and no way to notice. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_env_stamp", root, sizeof root));

    char manifest[512];
    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    char manifest_path[512];
    snprintf(manifest_path, sizeof manifest_path, "%s/Project.toml", root);
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"flavoured\"\n[env]\nMOLTO_FLAVOUR = \"one\"\n");
    EXPECT_TRUE(fs_write_file(manifest_path, manifest));

    EXPECT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    char object[512];
    char binary[512];
    snprintf(object, sizeof object, "%s/build/debug/obj/src/main.c.o", root);
    snprintf(binary, sizeof binary, "%s/build/debug/flavoured" FS_EXECUTABLE_SUFFIX, root);
    ASSERT_TRUE(fs_path_exists(object));
    int64_t compiled_at = mtime_of(object);
    int64_t linked_at = mtime_of(binary);

    /* Same manifest, same fingerprint: the string has to be stable across runs
       or nothing would ever be up to date. */
    EXPECT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));
    EXPECT_TRUE(mtime_of(object) == compiled_at);
    EXPECT_TRUE(mtime_of(binary) == linked_at);

    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"flavoured\"\n[env]\nMOLTO_FLAVOUR = \"two\"\n");
    EXPECT_TRUE(fs_write_file(manifest_path, manifest));

    EXPECT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));
    EXPECT_TRUE(mtime_of(object) != compiled_at);
    EXPECT_TRUE(mtime_of(binary) != linked_at);

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(build_does_not_recompile_when_the_env_only_moves) {
    /* The order two variables were written in is not something the build ran
       differently, and a rebuild over it would also split the shared object
       cache between two projects declaring the same thing. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_env_order", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    char manifest_path[512];
    snprintf(manifest_path, sizeof manifest_path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(manifest_path, "[package]\nname = \"shuffled\"\n"
                                             "[env]\nZED = \"z\"\nALPHA = \"a\"\n"));

    EXPECT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    char object[512];
    snprintf(object, sizeof object, "%s/build/debug/obj/src/main.c.o", root);
    ASSERT_TRUE(fs_path_exists(object));
    int64_t compiled_at = mtime_of(object);

    EXPECT_TRUE(fs_write_file(manifest_path, "[package]\nname = \"shuffled\"\n"
                                             "[env]\nALPHA = \"a\"\nZED = \"z\"\n"));
    EXPECT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));
    EXPECT_TRUE(mtime_of(object) == compiled_at);

    char cmd[600];
    (void)fs_remove_tree(root);
}

MOLTEST(project_env_to_vars_maps_the_table_it_is_given) {
    char err[256] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\n"
                              "[env]\nALPHA = \"a\"\nBETA = \"b\"\n",
                              &ctx, err, sizeof err));

    process_env_var vars[PROJECT_MAX_ENV];
    ASSERT_EQ(2, project_env_to_vars(&ctx.env, vars, PROJECT_MAX_ENV));
    EXPECT_STREQ("ALPHA", vars[0].name);
    EXPECT_STREQ("a", vars[0].value);
    EXPECT_STREQ("BETA", vars[1].name);
    EXPECT_STREQ("b", vars[1].value);

    /* No table at all, and a caller with less room than there is to say. */
    EXPECT_EQ(0, project_env_to_vars(NULL, vars, PROJECT_MAX_ENV));
    EXPECT_EQ(1, project_env_to_vars(&ctx.env, vars, 1));
}

MOLTEST(env_fingerprint_says_nothing_when_there_is_no_env) {
    /* The whole compatibility story rests on this: no [env], nothing appended,
       so every workspace database and cached object already on disk still
       matches the command that made it. */
    char err[256] = "";
    project_ctx ctx;
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\n", &ctx, err, sizeof err));

    char out[PROJECT_ENV_FINGERPRINT_MAX];
    EXPECT_EQ(0, project_env_fingerprint(&ctx.env, out, sizeof out));
    EXPECT_STREQ("", out);

    EXPECT_EQ(0, project_env_fingerprint(NULL, out, sizeof out));
    EXPECT_STREQ("", out);
}

MOLTEST(env_fingerprint_separates_two_environments) {
    char err[256] = "";
    project_ctx one;
    project_ctx two;
    char first[PROJECT_ENV_FINGERPRINT_MAX];
    char second[PROJECT_ENV_FINGERPRINT_MAX];

    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\n[env]\nA = \"1\"\n",
                              &one, err, sizeof err));
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\n[env]\nA = \"2\"\n",
                              &two, err, sizeof err));
    EXPECT_TRUE(project_env_fingerprint(&one.env, first, sizeof first) > 0);
    EXPECT_TRUE(project_env_fingerprint(&two.env, second, sizeof second) > 0);
    EXPECT_STRNE(first, second);

    /* The same environment, written in either order, is one environment. */
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\n[env]\nA = \"1\"\nB = \"2\"\n",
                              &one, err, sizeof err));
    ASSERT_TRUE(project_parse("[package]\nname = \"app\"\n[env]\nB = \"2\"\nA = \"1\"\n",
                              &two, err, sizeof err));
    EXPECT_TRUE(project_env_fingerprint(&one.env, first, sizeof first) > 0);
    EXPECT_TRUE(project_env_fingerprint(&two.env, second, sizeof second) > 0);
    EXPECT_STREQ(first, second);
}

/* A build describes what it compiled, for the tools that parse this code
   without being the build (RFC-0007). */
MOLTEST(build_writes_the_compilation_database) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_cdb_build", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    ASSERT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    ASSERT_TRUE(fs_write_file(path,
        "[package]\nname = \"described\"\n"
        "[target]\nstd = \"c11\"\ndefines = [\"GREETING=\\\"hi\\\"\"]\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    ASSERT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));
    snprintf(path, sizeof path, "%s/src/util.c", root);
    ASSERT_TRUE(fs_write_file(path, "int used(void) { return 1; }\n"));

    /* -j 1 is the flag doing something observable: one worker compiles both
       units, which is the case a machine with one core has always had. */
    EXPECT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 1, NULL, 0));

    char database[512];
    snprintf(database, sizeof database, "%s/compile_commands.json", root);
    char *text = fs_read_file(database);
    ASSERT_NOT_NULL(text);
    json_document *doc = json_parse(text);
    free(text);
    ASSERT_NOT_NULL(doc);

    json_value array = json_root(doc);
    EXPECT_EQ(2, (long long)json_count(array));
    json_value entry = json_at(array, 0);
    EXPECT_STREQ("src/main.c", json_string(json_get(entry, "file")));
    EXPECT_STREQ("build/debug/obj/src/main.c.o", json_string(json_get(entry, "output")));

    /* A define with quotes in it survives the round trip: escaping it wrong is
       the difference between a database and an unparseable file. The depfile
       flags do not survive it, and neither does the path they name: they say
       nothing about the translation, and a tool that runs this line would
       write a depfile into build/ on the build's behalf. */
    bool found_define = false;
    bool found_depflag = false;
    json_value arguments = json_get(entry, "arguments");
    EXPECT_TRUE(json_count(arguments) > 3);
    for(size_t i = 0; i < json_count(arguments); i++) {
        const char *argument = json_string(json_at(arguments, i));
        if(argument == NULL)
            continue;
        found_define = found_define || strcmp(argument, "-DGREETING=\"hi\"") == 0;
        found_depflag = found_depflag || strcmp(argument, "-MMD") == 0 ||
                        strcmp(argument, "-MF") == 0 || strstr(argument, ".o.d") != NULL;
    }
    EXPECT_TRUE(found_define);
    EXPECT_FALSE(found_depflag);
    /* What is left is still a compile line: the object it produces is named. */
    bool found_output = false;
    for(size_t i = 0; i < json_count(arguments); i++) {
        const char *argument = json_string(json_at(arguments, i));
        found_output = found_output || (argument != NULL && strcmp(argument, "-o") == 0);
    }
    EXPECT_TRUE(found_output);
    json_free(doc);

    /* A rebuild that compiles nothing still describes everything: an editor
       asks what a file compiles as, and "it was up to date" is no answer. */
    EXPECT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));
    text = fs_read_file(database);
    ASSERT_NOT_NULL(text);
    doc = json_parse(text);
    free(text);
    ASSERT_NOT_NULL(doc);
    EXPECT_EQ(2, (long long)json_count(json_root(doc)));
    json_free(doc);

    /* The test build covers tests/ as well, so it is a superset of the one a
       plain build leaves. */
    snprintf(path, sizeof path, "%s/tests", root);
    ASSERT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/tests/first_test.c", root);
    ASSERT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    str_list binaries;
    str_list_init(&binaries);
    EXPECT_EQ(exit_ok, build_tests(root, profile_debug, NULL, false, 2, &binaries, NULL));
    str_list_free(&binaries);

    text = fs_read_file(database);
    ASSERT_NOT_NULL(text);
    doc = json_parse(text);
    free(text);
    ASSERT_NOT_NULL(doc);
    EXPECT_EQ(3, (long long)json_count(json_root(doc)));
    json_free(doc);

    /* A build that fails still describes what it tried to compile: a command
       line does not become wrong because the code it describes does not, and a
       broken build is when an editor that understands the project helps most. */
    snprintf(path, sizeof path, "%s/src/util.c", root);
    ASSERT_TRUE(fs_write_file(path, "int used(void) { return \n"));
    ASSERT_TRUE(remove(database) == 0);
    EXPECT_EQ(exit_build_failure, build_project(root, profile_debug, NULL, false, 0, NULL, 0));
    text = fs_read_file(database);
    ASSERT_NOT_NULL(text);
    doc = json_parse(text);
    free(text);
    ASSERT_NOT_NULL(doc);
    EXPECT_EQ(2, (long long)json_count(json_root(doc)));
    json_free(doc);

    char cmd[600];
    (void)fs_remove_tree(root);
}

/* --- libraries (RFC-0007) --- */

/* A project of one source, built as whatever `[package].artifact` names. */
static bool library_project(char *root, const char *artifact, const char *version) {
    char path[512];
    char manifest[512];
    snprintf(path, sizeof path, "%s/src", root);
    if(!fs_make_dirs(path))
        return false;
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"greet\"\nversion = \"%s\"\nartifact = \"%s\"\n", version,
             artifact);
    snprintf(path, sizeof path, "%s/Project.toml", root);
    if(!fs_write_file(path, manifest))
        return false;
    snprintf(path, sizeof path, "%s/src/greet.c", root);
    return fs_write_file(path, "const char *greet(void) { return \"hi\"; }\n");
}

static void remove_tree(const char *root) {
    char cmd[600];
    (void)fs_remove_tree(root);
}

/* The eight bytes every `ar` archive starts with. Checked rather than trusting
   the extension: a file named `.a` that is not an archive is exactly the
   failure this would otherwise miss. */
static bool is_ar_archive(const char *path) {
    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return false;
    char magic[8] = {0};
    const size_t read = fread(magic, 1, sizeof magic, file);
    (void)fclose(file);
    return read == sizeof magic && memcmp(magic, "!<arch>\n", sizeof magic) == 0;
}

/* Whether an archive carries a member of this name.
 *
 * Read as bytes and scanned by hand, because an archive is full of NULs and
 * anything that stops at the first one stops inside the first member's header —
 * which reads as "the member is not there" for every member there is. */
static bool archive_has_member(const char *path, const char *member) {
    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return false;
    char buffer[65536];
    const size_t read = fread(buffer, 1, sizeof buffer, file);
    (void)fclose(file);

    const size_t length = strlen(member);
    for(size_t i = 0; length <= read && i + length <= read; i++) {
        if(memcmp(buffer + i, member, length) == 0)
            return true;
    }
    return false;
}

MOLTEST(build_makes_a_static_library_when_the_manifest_asks_for_one) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_static", root, sizeof root));
    ASSERT_TRUE(library_project(root, "static", "0.1.0"));

    ASSERT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    char archive[512];
    snprintf(archive, sizeof archive, "%s/build/debug/libgreet.a", root);
    EXPECT_TRUE(is_ar_archive(archive));

    /* And no executable beside it: the manifest asked for one thing. */
    char binary[512];
    snprintf(binary, sizeof binary, "%s/build/debug/greet" FS_EXECUTABLE_SUFFIX, root);
    EXPECT_FALSE(fs_path_exists(binary));

    remove_tree(root);
}

/*
 * `ar r` replaces the members it is given and leaves every other one alone, so
 * an archive updated in place keeps the object of a source that was deleted —
 * present at link time, absent from the sources, and impossible to account for.
 * The archive is removed before it is written for exactly this.
 */
MOLTEST(a_static_library_forgets_an_object_whose_source_is_gone) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_stale", root, sizeof root));
    ASSERT_TRUE(library_project(root, "static", "0.1.0"));

    char extra[512];
    snprintf(extra, sizeof extra, "%s/src/bye.c", root);
    ASSERT_TRUE(fs_write_file(extra, "int bye(void) { return 1; }\n"));
    ASSERT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    char archive[512];
    snprintf(archive, sizeof archive, "%s/build/debug/libgreet.a", root);
    EXPECT_TRUE(archive_has_member(archive, "bye.c.o"));

    ASSERT_EQ(0, remove(extra));
    ASSERT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    EXPECT_FALSE(archive_has_member(archive, "bye.c.o"));
    EXPECT_TRUE(archive_has_member(archive, "greet.c.o"));

    remove_tree(root);
}

/*
 * The convention, on disk: the real file carries the whole version, and two
 * links point at it. A consumer linking `-lgreet` resolves through
 * `libgreet.so` and records the soname, which is what lets 1.2.3 be replaced by
 * 1.9.0 under a program that never relinks.
 */
MOLTEST(build_makes_a_shared_library_with_the_two_links_beside_it) {
#ifdef _WIN32
    /* Skipped by decision, not by accident, and not because the system will
       not cooperate -- the arrangement below would run here. What does not
       exist is the thing being arranged for: a shared library on Windows is a
       DLL with an import library beside it, no version in the name and no
       links to make, and molto does not build one yet. RFC-0017 records that,
       and why it is its own piece of work rather than a translation of these
       flags.

       Reported rather than deleted, so the day the DLL arrives this is the
       test waiting for it. */
    SKIP("a shared library on Windows is a DLL, which molto does not build yet "
         "(RFC-0017)");
#else
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_shared", root, sizeof root));
    ASSERT_TRUE(library_project(root, "shared", "1.2.3"));

    ASSERT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    char library[512];
    snprintf(library, sizeof library, "%s/build/debug/libgreet.so.1.2.3", root);
    EXPECT_TRUE(fs_path_exists(library));

    const char *const links[] = {"libgreet.so.1", "libgreet.so"};
    for(size_t i = 0; i < sizeof links / sizeof links[0]; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/build/debug/%s", root, links[i]);

        /* Both names really are there, whichever kind of link the system
           makes. This is the portable half, and the one a consumer depends
           on. */
        EXPECT_TRUE(fs_path_exists(path));

        /* And where there is a target to read, it is relative: both links sit
           beside the file they name, and an absolute one would write this
           machine's build directory inside an artifact whose purpose is to be
           copied elsewhere.

           A hard link has nothing to read back — it is a second name for the
           bytes rather than a note saying where they are — so the assertion
           applies where links carry targets and is silent where they do not.
           None of this is settled for Windows anyway: what a shared library
           should be *called* there is open in RFC-0017, and `libgreet.so.1.2.3`
           is not the answer. */
        char target[256] = "";
        if(fs_link_target(path, target, sizeof target))
            EXPECT_STREQ("libgreet.so.1.2.3", target);
    }

    remove_tree(root);
#endif
}

/* A guess would put the wrong number in a soname, which is the one place a
   wrong number is a promise about ABI — so the build stops instead. */
MOLTEST(a_shared_library_refuses_a_version_it_cannot_take_a_major_from) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_noversion", root, sizeof root));
    ASSERT_TRUE(library_project(root, "shared", "nightly"));

    EXPECT_NE(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    remove_tree(root);
}

/* Nothing changed, so nothing is archived again: remaking it would hand every
   consumer a new mtime to react to. */
MOLTEST(a_static_library_is_not_archived_again_for_nothing) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_fresh", root, sizeof root));
    ASSERT_TRUE(library_project(root, "static", "0.1.0"));

    ASSERT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));
    char archive[512];
    snprintf(archive, sizeof archive, "%s/build/debug/libgreet.a", root);
    const int64_t first = mtime_of(archive);

    ASSERT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));
    EXPECT_EQ(first, mtime_of(archive));

    remove_tree(root);
}

/* --- building for another platform (RFC-0017) --- */

/*
 * A build for elsewhere gets a directory of its own.
 *
 * It cannot share the host's: the objects are called the same and hold
 * different code, so one would silently be read as the other. The target that
 * is asked for here is this machine's own, because what is under test is where
 * the output lands rather than whether a cross toolchain exists.
 */
MOLTEST(build_for_a_target_puts_its_output_under_that_target) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_target", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"far\"\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    /* Resolution is bypassed on purpose: what is under test is where the
       output lands, not whether this machine owns a compiler that emits for
       somewhere else. `C_COMPILER` is the documented way to say "use this one
       and ask nobody", and it leaves the target deciding only the path. */
    char saved_cc[4096];
    bool had_cc;
    remember_env("C_COMPILER", saved_cc, sizeof saved_cc, &had_cc);
    ASSERT_TRUE(setenv("C_COMPILER", "cc", 1) == 0);

    const int code = build_project(root, profile_debug, "sparc-unknown-none-elf", false, 0, NULL, 0);
    restore_env("C_COMPILER", saved_cc, had_cc);
    ASSERT_EQ(exit_ok, code);

    char binary[512];
    snprintf(binary, sizeof binary, "%s/build/sparc-unknown-none-elf/debug/far" FS_EXECUTABLE_SUFFIX, root);
    EXPECT_TRUE(fs_path_exists(binary));

    /* And the objects went with it, rather than into the host's tree where the
       next ordinary build would read them as its own. */
    char object[512];
    snprintf(object, sizeof object, "%s/build/sparc-unknown-none-elf/debug/obj/src/main.c.o", root);
    EXPECT_TRUE(fs_path_exists(object));

    char host_tree[512];
    snprintf(host_tree, sizeof host_tree, "%s/build/debug", root);
    EXPECT_FALSE(fs_path_exists(host_tree));

    remove_tree(root);
}

/* And a build that asks for nothing keeps the path it always had: no project
   that never wanted a target sees one appear. */
MOLTEST(build_without_a_target_keeps_the_directory_it_always_had) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_notarget", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"near\"\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    ASSERT_EQ(exit_ok, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    char binary[512];
    snprintf(binary, sizeof binary, "%s/build/debug/near" FS_EXECUTABLE_SUFFIX, root);
    EXPECT_TRUE(fs_path_exists(binary));

    remove_tree(root);
}

/*
 * `[target].host` and another platform cannot both be true.
 *
 * pkg-config answers for the machine it runs on, so a cross build would compile
 * this host's headers into a binary for somewhere else — a build that succeeds
 * and produces something that cannot link there. Refused, and the message says
 * which of the two is the problem.
 */
MOLTEST(a_host_library_cannot_be_resolved_for_another_platform) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_hostcross", root, sizeof root));

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path, "[package]\nname = \"needy\"\n"
                                    "\n[target]\nhost = [\"some-toolkit\"]\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    EXPECT_EQ(exit_invalid_manifest,
              build_project(root, profile_debug, "sparc-unknown-none-elf", false, 0, NULL, 0));

    /* And the same manifest builds when nothing foreign is asked for — the
       refusal is about the pair, not about either one. */
    EXPECT_NE(exit_invalid_manifest, build_project(root, profile_debug, NULL, false, 0, NULL, 0));

    remove_tree(root);
}
