#include <moltest.h>

#include <molto/build/compile_flags.h>

#include <stdio.h>
#include <string.h>

/* The rules these cover are contracts from RFC-0003, not conveniences: a
   relative include is anchored at the project root, and `flags` goes through
   verbatim. Both the build and `molto lint` depend on them agreeing. */

static void seed_options(project_options *options) {
    memset(options, 0, sizeof *options);
}

MOLTEST(compile_flags_anchor_a_relative_include_at_the_root) {
    str_list argv;
    str_list_init(&argv);

    /* The compiler runs wherever the user invoked molto from, which is not
       necessarily the root, so "vendor" has to become the project's vendor/. */
    ASSERT_TRUE(compile_flags_push_include(&argv, "/home/u/app", "vendor"));
    ASSERT_EQ(1, (int)str_list_count(&argv));
    EXPECT_STREQ("-I/home/u/app/vendor", str_list_get(&argv, 0));

    str_list_free(&argv);
}

MOLTEST(compile_flags_leave_an_absolute_include_alone) {
    str_list argv;
    str_list_init(&argv);

    ASSERT_TRUE(compile_flags_push_include(&argv, "/home/u/app", "/usr/local/include"));
    EXPECT_STREQ("-I/usr/local/include", str_list_get(&argv, 0));

    str_list_free(&argv);
}

MOLTEST(compile_flags_prefix_defines_and_pass_raw_flags_verbatim) {
    project_options options;
    seed_options(&options);
    snprintf(options.defines[0], PROJECT_OPT_LEN, "%s", "FOO=1");
    options.define_count = 1;
    snprintf(options.include[0], PROJECT_OPT_LEN, "%s", "include");
    options.include_count = 1;
    /* A flag is an escape hatch: whatever it is, it reaches the compiler as
       written. Anchoring or rewriting one would break that promise. */
    snprintf(options.flags[0], PROJECT_OPT_LEN, "%s", "-fno-omit-frame-pointer");
    options.flag_count = 1;

    str_list argv;
    str_list_init(&argv);
    ASSERT_TRUE(compile_flags_push_options(&argv, "/root", &options));

    ASSERT_EQ(3, (int)str_list_count(&argv));
    EXPECT_STREQ("-DFOO=1", str_list_get(&argv, 0));
    EXPECT_STREQ("-I/root/include", str_list_get(&argv, 1));
    EXPECT_STREQ("-fno-omit-frame-pointer", str_list_get(&argv, 2));

    str_list_free(&argv);
}

MOLTEST(compile_flags_push_the_standard_of_the_language_of_the_unit) {
    project_target target;
    memset(&target, 0, sizeof target);
    snprintf(target.std, sizeof target.std, "%s", "c17");
    snprintf(target.cpp_std, sizeof target.cpp_std, "%s", "c++20");

    str_list argv;
    str_list_init(&argv);
    ASSERT_TRUE(compile_flags_push_std(&argv, &target, false));
    ASSERT_TRUE(compile_flags_push_std(&argv, &target, true));

    ASSERT_EQ(2, (int)str_list_count(&argv));
    EXPECT_STREQ("-std=c17", str_list_get(&argv, 0));
    EXPECT_STREQ("-std=c++20", str_list_get(&argv, 1));

    str_list_free(&argv);
}

MOLTEST(compile_flags_push_no_standard_when_none_is_declared) {
    project_target target;
    memset(&target, 0, sizeof target);

    str_list argv;
    str_list_init(&argv);
    /* Nothing to say is not a failure: the compiler keeps its own default. */
    ASSERT_TRUE(compile_flags_push_std(&argv, &target, false));
    EXPECT_EQ(0, (int)str_list_count(&argv));

    str_list_free(&argv);
}

MOLTEST(compile_flags_pick_the_cpp_driver_only_for_cpp_units) {
    resolved_toolchain chain;
    memset(&chain, 0, sizeof chain);
    snprintf(chain.cc, sizeof chain.cc, "%s", "/usr/bin/gcc");
    snprintf(chain.cxx, sizeof chain.cxx, "%s", "/usr/bin/g++");

    EXPECT_STREQ("/usr/bin/gcc", compile_flags_driver(&chain, false));
    EXPECT_STREQ("/usr/bin/g++", compile_flags_driver(&chain, true));
}

MOLTEST(compile_flags_report_a_missing_cpp_driver_as_null) {
    resolved_toolchain chain;
    memset(&chain, 0, sizeof chain);
    snprintf(chain.cc, sizeof chain.cc, "%s", "/usr/bin/gcc");

    /* Saying so beats invoking the C driver and letting the linker fail with
       something unrelated. */
    EXPECT_NULL(compile_flags_driver(&chain, true));
    EXPECT_STREQ("/usr/bin/gcc", compile_flags_driver(&chain, false));
}
