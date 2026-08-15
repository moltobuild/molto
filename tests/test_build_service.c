#include <moltest.h>

#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>

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
    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_ok);
    char binary[512];
    snprintf(binary, sizeof binary, "%s/build/debug/demo_app", root);
    EXPECT_TRUE(fs_path_exists(binary));
    int64_t linked_at = mtime_of(binary);

    /* A no-op rebuild must NOT re-link (the binary is left untouched). */
    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(binary) == linked_at);

    /* Touching a header recompiles the units that include it. main.c includes
       util.h, so editing util.h must rebuild main.c and re-link. */
    char util_header[512];
    snprintf(util_header, sizeof util_header, "%s/src/util.h", root);
    EXPECT_TRUE(fs_write_file(util_header, "int answer(void); /* touched */\n"));
    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(binary) > linked_at);
    linked_at = mtime_of(binary); /* rebase for the next step */

    /* Changing a source triggers recompilation and a re-link. */
    char main_path[512];
    snprintf(main_path, sizeof main_path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(main_path,
        "#include <stdio.h>\n"
        "int main(void) { printf(\"hi again\\n\"); return 0; }\n"));
    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_ok);
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
    EXPECT_TRUE(build_project(empty, profile_debug, false, NULL, 0) == exit_invalid_manifest);
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
    EXPECT_TRUE(build_project(lib_root, profile_debug, false, NULL, 0) == exit_ok);
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
    EXPECT_TRUE(build_project(fp_root, profile_debug, false, NULL, 0) == exit_ok);
    char fp_obj[512];
    snprintf(fp_obj, sizeof fp_obj, "%s/build/debug/obj/src/main.c.o", fp_root);
    int64_t compiled_at = mtime_of(fp_obj);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"fp\"\n[profile.debug]\nopt_level = 2\ndebug_info = true\n"));
    EXPECT_TRUE(build_project(fp_root, profile_debug, false, NULL, 0) == exit_ok);
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
    EXPECT_TRUE(build_project(def_root, profile_debug, false, NULL, 0) == exit_ok);
    snprintf(cmd, sizeof cmd, "rm -rf %s", def_root);
    (void)system(cmd);
}

MOLTEST(build_keeps_the_units_that_compiled_when_another_fails) {
    char root[] = "/tmp/molto_partial_XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);

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
    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_build_failure);

    /* ...but good.c did compile, and its object is recorded as up to date. */
    char good_object[512];
    snprintf(good_object, sizeof good_object, "%s/build/debug/obj/src/good.c.o", root);
    ASSERT_TRUE(fs_path_exists(good_object));
    int64_t compiled_at = mtime_of(good_object);

    /* A second attempt only retries the broken unit: the good object is left
       alone instead of being thrown away and rebuilt. */
    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_build_failure);
    EXPECT_TRUE(mtime_of(good_object) == compiled_at);

    /* No stale depfile is left behind for the unit that failed. */
    char bad_depfile[512];
    snprintf(bad_depfile, sizeof bad_depfile, "%s/build/debug/obj/src/bad.c.o.d", root);
    EXPECT_FALSE(fs_path_exists(bad_depfile));

    /* Fixing the broken unit completes the build. */
    snprintf(path, sizeof path, "%s/src/bad.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));
    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_ok);
    EXPECT_TRUE(mtime_of(good_object) == compiled_at); /* still untouched */

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(build_compiles_cpp_sources_with_the_cpp_driver) {
    char root[] = "/tmp/molto_cpp_XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);

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

    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_ok);

    char binary[512];
    snprintf(binary, sizeof binary, "%s/build/debug/cpp_app", root);
    ASSERT_TRUE(fs_path_exists(binary));
    char cmd[600];
    snprintf(cmd, sizeof cmd, "%s > /dev/null 2>&1", binary);
    EXPECT_TRUE(system(cmd) == 0);

    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(build_honours_the_release_profile) {
    char root[] = "/tmp/molto_release_XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);

    char path[512];
    snprintf(path, sizeof path, "%s/src", root);
    EXPECT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/Project.toml", root);
    EXPECT_TRUE(fs_write_file(path,
        "[package]\nname = \"fast\"\n"
        "[profile.release]\nopt_level = 2\ndebug_info = false\n"));
    snprintf(path, sizeof path, "%s/src/main.c", root);
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 0; }\n"));

    EXPECT_TRUE(build_project(root, profile_release, false, NULL, 0) == exit_ok);

    /* Each profile gets its own output tree, so debug and release coexist. */
    char release_binary[512];
    snprintf(release_binary, sizeof release_binary, "%s/build/release/fast", root);
    EXPECT_TRUE(fs_path_exists(release_binary));
    char debug_binary[512];
    snprintf(debug_binary, sizeof debug_binary, "%s/build/debug/fast", root);
    EXPECT_FALSE(fs_path_exists(debug_binary));

    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_ok);
    EXPECT_TRUE(fs_path_exists(debug_binary));
    EXPECT_TRUE(fs_path_exists(release_binary));

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(build_anchors_relative_includes_at_the_project_root) {
    char root[] = "/tmp/molto_incl_XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);

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

    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_ok);

    EXPECT_TRUE(chdir(previous) == 0);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(build_does_not_record_an_object_for_a_source_that_changed_while_compiling) {
    char root[] = "/tmp/molto_build_race_XXXXXX";
    ASSERT_NOT_NULL(mkdtemp(root));
    char tools[] = "/tmp/molto_build_cc_XXXXXX";
    ASSERT_NOT_NULL(mkdtemp(tools));

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
    char compiler[512];
    snprintf(compiler, sizeof compiler, "%s/cc", tools);
    char script[1536];
    snprintf(script, sizeof script,
             "#!/bin/sh\n"
             "out=''; dep=''; src=''; prev=''; log=%s/calls\n"
             "for a in \"$@\"; do\n"
             "  [ \"$prev\" = '-o' ] && out=\"$a\"\n"
             "  [ \"$prev\" = '-MF' ] && dep=\"$a\"\n"
             "  case \"$a\" in *.c) src=\"$a\";; esac\n"
             "  prev=\"$a\"\n"
             "done\n"
             "[ -n \"$out\" ] && : > \"$out\"\n"
             "[ -n \"$dep\" ] && [ -n \"$src\" ] && echo \"o: $src\" > \"$dep\"\n"
             "[ -n \"$src\" ] && echo '/* edited mid-build */' >> \"$src\"\n"
             "echo x >> $log\n",
             tools);
    ASSERT_TRUE(fs_write_file(compiler, script));
    ASSERT_TRUE(chmod(compiler, 0755) == 0);
    ASSERT_TRUE(setenv("C_COMPILER", compiler, 1) == 0);

    ASSERT_EQ(exit_ok, build_project(root, profile_debug, false, NULL, 0));

    char log[512];
    snprintf(log, sizeof log, "%s/calls", tools);
    char *first = fs_read_file(log);
    ASSERT_NOT_NULL(first);
    size_t after_first = strlen(first);
    free(first);

    /* The object holds a compilation of content that is no longer on disk.
       Recording it would leave nothing to rebuild it, and the next link would
       quietly take the stale object — which is how a test suite ends up
       running against code that was already changed. */
    ASSERT_EQ(exit_ok, build_project(root, profile_debug, false, NULL, 0));
    char *second = fs_read_file(log);
    ASSERT_NOT_NULL(second);
    EXPECT_TRUE(strlen(second) > after_first);
    free(second);

    (void)unsetenv("C_COMPILER");
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s %s", root, tools);
    (void)system(cmd);
}

/* The whole point of scoping flags, checked by the compiler rather than by
   reading a list: every claim below is a #error that fires if the flag reached
   a translation unit it had no business reaching. Before the scopes existed,
   this project failed to build on two of them. */
MOLTEST(a_dependencys_private_flags_reach_its_own_sources_and_nothing_else) {
    char root[] = "/tmp/molto_scope_XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);

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

    EXPECT_TRUE(build_project(root, profile_debug, false, NULL, 0) == exit_ok);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
