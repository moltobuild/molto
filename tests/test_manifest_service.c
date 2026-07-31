#include <moltest.h>

#include <molto/services/manifest_service.h>

#include <stdlib.h>
#include <string.h>

MOLTEST(manifest_service) {
    /* Valid snake_case names. */
    EXPECT_TRUE(manifest_is_valid_name("my_app"));
    EXPECT_TRUE(manifest_is_valid_name("http2"));
    EXPECT_TRUE(manifest_is_valid_name("a"));

    /* Invalid names. */
    EXPECT_TRUE(!manifest_is_valid_name(NULL));
    EXPECT_TRUE(!manifest_is_valid_name(""));
    EXPECT_TRUE(!manifest_is_valid_name("My_App")); /* uppercase */
    EXPECT_TRUE(!manifest_is_valid_name("1abc"));   /* leading digit */
    EXPECT_TRUE(!manifest_is_valid_name("a-b"));    /* hyphen */
    EXPECT_TRUE(!manifest_is_valid_name("a/b"));    /* slash */

    /* Rendering a valid manifest. */
    char *toml = manifest_render_default("my_app");
    EXPECT_TRUE(toml != NULL);
    if (toml != NULL) {
        EXPECT_TRUE(strstr(toml, "name = \"my_app\"") != NULL);
        EXPECT_TRUE(strstr(toml, "[package]") != NULL);
        EXPECT_TRUE(strstr(toml, "[profile.release]") != NULL);
        free(toml);
    }

    /* Invalid name yields no manifest. */
    EXPECT_TRUE(manifest_render_default("Bad Name") == NULL);
}
