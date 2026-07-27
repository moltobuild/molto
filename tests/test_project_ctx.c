#include "test_framework.h"
#include "tests.h"

#include <molto/project/project_ctx.h>

#include <string.h>

void suite_project_ctx(void) {
    const char *manifest =
        "[package]\n"
        "name = \"demo_app\"      # the package\n"
        "version = \"1.2.3\"\n"
        "artifact = \"shared\"\n"
        "\n"
        "[profile.release]\n"
        "opt_level = 2\n"
        "debug_info = true\n";

    char err[256] = "";
    project_ctx ctx;
    CHECK(project_parse(manifest, &ctx, err, sizeof err));

    CHECK(strcmp(ctx.project_name, "demo_app") == 0);
    CHECK(strcmp(ctx.version, "1.2.3") == 0);
    CHECK(ctx.artifact == artifact_shared);

    /* Declared profile overrides the built-in defaults. */
    CHECK(ctx.profile.release.opt_level == 2);
    CHECK(ctx.profile.release.debug_info == true);

    /* Undeclared profiles keep their built-in defaults. */
    CHECK(ctx.profile.debug.opt_level == 0 && ctx.profile.debug.debug_info == true);
    CHECK(ctx.profile.bench.opt_level == 3 && ctx.profile.bench.debug_info == false);
    CHECK(ctx.profile.custom.opt_level == 2 && ctx.profile.custom.debug_info == true);

    /* Defaults when optional package fields are absent. */
    project_ctx minimal;
    CHECK(project_parse("[package]\nname = \"tiny\"\n", &minimal, err, sizeof err));
    CHECK(strcmp(minimal.version, "0.0.0") == 0);
    CHECK(minimal.artifact == artifact_static);

    /* Missing or invalid name is an error. */
    CHECK(!project_parse("[package]\nversion = \"1.0.0\"\n", &ctx, err, sizeof err));
    CHECK(!project_parse("[package]\nname = \"Bad_Name\"\n", &ctx, err, sizeof err));

    /* Unknown artifact kind is an error. */
    CHECK(!project_parse("[package]\nname = \"x\"\nartifact = \"weird\"\n",
                         &ctx, err, sizeof err));

    /* A malformed manifest surfaces the parser's line-tagged error. */
    err[0] = '\0';
    CHECK(!project_parse("[package\nname = \"x\"\n", &ctx, err, sizeof err));
    CHECK(strstr(err, "Project.toml:1") != NULL);
}
