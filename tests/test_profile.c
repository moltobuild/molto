#include "test_framework.h"
#include "tests.h"

#include <molto/build/profile.h>

#include <string.h>

void suite_profile(void) {
    build_profile profile = profile_debug;

    CHECK(profile_parse("debug", &profile) && profile == profile_debug);
    CHECK(profile_parse("release", &profile) && profile == profile_release);
    CHECK(profile_parse("bench", &profile) && profile == profile_bench);
    CHECK(profile_parse("custom", &profile) && profile == profile_custom);

    CHECK(!profile_parse("nope", &profile));
    CHECK(!profile_parse(NULL, &profile));

    CHECK(strcmp(profile_name(profile_debug), "debug") == 0);
    CHECK(strcmp(profile_name(profile_release), "release") == 0);
    CHECK(strcmp(profile_name(profile_bench), "bench") == 0);
    CHECK(strcmp(profile_name(profile_custom), "custom") == 0);
}
