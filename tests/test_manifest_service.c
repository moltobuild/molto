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
}
