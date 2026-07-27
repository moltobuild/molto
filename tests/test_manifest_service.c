#include "test_framework.h"
#include "tests.h"

#include <molto/services/manifest_service.h>

#include <stdlib.h>
#include <string.h>

void suite_manifest_service(void) {
    /* Valid snake_case names. */
    CHECK(manifest_is_valid_name("my_app"));
    CHECK(manifest_is_valid_name("http2"));
    CHECK(manifest_is_valid_name("a"));

    /* Invalid names. */
    CHECK(!manifest_is_valid_name(NULL));
    CHECK(!manifest_is_valid_name(""));
    CHECK(!manifest_is_valid_name("My_App")); /* uppercase */
    CHECK(!manifest_is_valid_name("1abc"));   /* leading digit */
    CHECK(!manifest_is_valid_name("a-b"));    /* hyphen */
    CHECK(!manifest_is_valid_name("a/b"));    /* slash */

    /* Rendering a valid manifest. */
    char *toml = manifest_render_default("my_app");
    CHECK(toml != NULL);
    if (toml != NULL) {
        CHECK(strstr(toml, "name = \"my_app\"") != NULL);
        CHECK(strstr(toml, "[package]") != NULL);
        CHECK(strstr(toml, "[profile.release]") != NULL);
        free(toml);
    }

    /* Invalid name yields no manifest. */
    CHECK(manifest_render_default("Bad Name") == NULL);

    /* Reading the package name back from a manifest. */
    const char *manifest =
        "[package]\n"
        "name = \"demo_app\"\n"
        "version = \"0.1.0\"\n"
        "\n"
        "[profile.release]\n"
        "opt_level = 2\n"
        "debug_info = true\n";
    char name[64] = "";
    CHECK(manifest_read_name(manifest, name, sizeof name));
    CHECK(strcmp(name, "demo_app") == 0);
    CHECK(!manifest_read_name("[package]\nversion = \"0.1.0\"\n", name, sizeof name));

    /* Reading a profile overrides the seeded defaults. */
    manifest_profile settings = { .opt_level = 0, .debug_info = false };
    CHECK(manifest_read_profile(manifest, "release", &settings));
    CHECK(settings.opt_level == 2);
    CHECK(settings.debug_info == true);

    /* Absent section returns false and leaves the defaults untouched. */
    manifest_profile untouched = { .opt_level = 9, .debug_info = false };
    CHECK(!manifest_read_profile(manifest, "bench", &untouched));
    CHECK(untouched.opt_level == 9);

    /* A section with only one key keeps the other default. */
    const char *partial = "[profile.debug]\nopt_level = 1\n";
    manifest_profile mixed = { .opt_level = 0, .debug_info = true };
    CHECK(manifest_read_profile(partial, "debug", &mixed));
    CHECK(mixed.opt_level == 1);
    CHECK(mixed.debug_info == true); /* untouched default */
}
