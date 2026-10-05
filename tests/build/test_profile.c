#include <moltest.h>

#include <molto/build/profile.h>

#include <string.h>

DESCRIBE(profile) {
    build_profile profile = profile_debug;

    EXPECT_TRUE(profile_parse("debug", &profile) && profile == profile_debug);
    EXPECT_TRUE(profile_parse("release", &profile) && profile == profile_release);
    EXPECT_TRUE(profile_parse("bench", &profile) && profile == profile_bench);
    EXPECT_TRUE(profile_parse("custom", &profile) && profile == profile_custom);

    EXPECT_TRUE(!profile_parse("nope", &profile));
    EXPECT_TRUE(!profile_parse(NULL, &profile));

    EXPECT_TRUE(strcmp(profile_name(profile_debug), "debug") == 0);
    EXPECT_TRUE(strcmp(profile_name(profile_release), "release") == 0);
    EXPECT_TRUE(strcmp(profile_name(profile_bench), "bench") == 0);
    EXPECT_TRUE(strcmp(profile_name(profile_custom), "custom") == 0);
}

/* RFC-0019: a fifth profile, named like the others. */
DESCRIBE(coverage_is_a_profile) {
    build_profile profile = profile_debug;
    EXPECT_TRUE(profile_parse("coverage", &profile) && profile == profile_coverage);
    EXPECT_STREQ("coverage", profile_name(profile_coverage));
}

/* GCC and Clang build it; MSVC has no gcov coverage and is refused. A vendor
   Molto could not name (C_COMPILER) is not refused for being unnamed. */
DESCRIBE(coverage_needs_a_gcc_or_clang_toolchain) {
    EXPECT_TRUE(profile_coverage_supported("gcc"));
    EXPECT_TRUE(profile_coverage_supported("clang"));
    EXPECT_TRUE(profile_coverage_supported("apple-clang"));
    EXPECT_TRUE(profile_coverage_supported(""));
    EXPECT_FALSE(profile_coverage_supported("msvc"));
}
