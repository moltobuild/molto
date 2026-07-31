#include <moltest.h>

#include <molto/build/profile.h>

#include <string.h>

MOLTEST(profile) {
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
