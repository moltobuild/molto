#include <moltest.h>

#include <molto/commands/publish_command.h>
#include <molto/exit_code.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Everything a recipe can get wrong, checked before a byte is sent.
 *
 * Only the refusals are exercised here. A recipe that passes validation goes
 * on to load a credential and reach a registry, which a unit test has no
 * business doing -- so the seam this file tests is exactly the one that
 * decides whether the network is touched at all. */

#define RECIPE_MAX 1024

typedef struct {
    char dir[64];
    char recipe[128];
} fixture;

static bool write_recipe(fixture *at, const char *text) {
    snprintf(at->dir, sizeof at->dir, "%s", "/tmp/molto_publish_XXXXXX");
    if (mkdtemp(at->dir) == NULL)
        return false;
    snprintf(at->recipe, sizeof at->recipe, "%s/recipe.toml", at->dir);

    FILE *file = fopen(at->recipe, "w");
    if (file == NULL)
        return false;
    fputs(text, file);
    return fclose(file) == 0;
}

static void discard(const fixture *at) {
    (void)remove(at->recipe);
    (void)rmdir(at->dir);
}

/* Publishes `text` without sending anything, and answers the exit code. */
static int publish(const char *text) {
    fixture at;
    if (!write_recipe(&at, text))
        return -1;
    const int code = publish_command_run(at.recipe, NULL, true);
    discard(&at);
    return code;
}

static const char *const SOURCE_RECIPE = "schema = 1\n"
                                         "form = \"source\"\n"
                                         "kind = \"package\"\n"
                                         "name = \"sqlite\"\n"
                                         "version = \"3.53.4\"\n"
                                         "target = \"any\"\n"
                                         "\n"
                                         "[source]\n"
                                         "archive = \"https://sqlite.org/a.zip\"\n"
                                         "sha256 = \"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\"\n"
                                         "\n"
                                         "[build]\n"
                                         "system = \"none\"\n"
                                         "\n"
                                         "[artifacts]\n"
                                         "type = \"source\"\n";

/* Drops the named table header and everything under it, up to the next one. */
static void without_table(const char *text, const char *table, char *out, size_t out_size) {
    const char *start = strstr(text, table);
    if (start == NULL) {
        snprintf(out, out_size, "%s", text);
        return;
    }
    const char *rest = strstr(start + strlen(table), "\n[");
    const size_t head = (size_t)(start - text);
    snprintf(out, out_size, "%.*s%s", (int)head, text, rest == NULL ? "" : rest + 1);
}

MOLTEST(publish_refuses_a_form_it_cannot_act_on) {
    /* Guessing would publish a document into a catalogue as if it were bytes. */
    char text[RECIPE_MAX];
    snprintf(text, sizeof text, "%s", SOURCE_RECIPE);
    char *form = strstr(text, "\"source\"");
    ASSERT_NOT_NULL(form);
    memcpy(form, "\"binari\"", 8);

    EXPECT_EQ(exit_invalid_manifest, publish(text));
}

MOLTEST(publish_requires_the_tables_a_source_recipe_is_made_of) {
    char text[RECIPE_MAX];

    without_table(SOURCE_RECIPE, "[source]", text, sizeof text);
    EXPECT_EQ(exit_invalid_manifest, publish(text));

    without_table(SOURCE_RECIPE, "[build]", text, sizeof text);
    EXPECT_EQ(exit_invalid_manifest, publish(text));

    without_table(SOURCE_RECIPE, "[artifacts]", text, sizeof text);
    EXPECT_EQ(exit_invalid_manifest, publish(text));
}

MOLTEST(publish_refuses_a_source_toolchain) {
    /* Nobody is going to build GCC on the machine that wants it, which is the
       whole reason a toolchain is distributed already built. */
    char text[RECIPE_MAX];
    snprintf(text, sizeof text, "%s", SOURCE_RECIPE);
    char *kind = strstr(text, "\"package\"");
    ASSERT_NOT_NULL(kind);
    memcpy(kind, "\"toolchn\"", 9);

    EXPECT_EQ(exit_invalid_manifest, publish(text));
}

MOLTEST(publish_refuses_an_archive_for_a_recipe_that_has_none) {
    /* Reported rather than ignored: naming a file means believing bytes are
       being published, and for a source recipe none ever will be. */
    fixture at;
    ASSERT_TRUE(write_recipe(&at, SOURCE_RECIPE));

    EXPECT_EQ(exit_usage_error, publish_command_run(at.recipe, "/tmp/nothing.tar.zst", true));
    discard(&at);
}

MOLTEST(publish_still_requires_the_table_a_binary_recipe_carries) {
    /* The binary form is untouched: it is what every toolchain and tool in the
       registry was published as. */
    static const char *const missing_package = "kind = \"package\"\n"
                                               "name = \"yyjson\"\n"
                                               "version = \"0.10.0\"\n"
                                               "target = \"any\"\n";

    EXPECT_EQ(exit_invalid_manifest, publish(missing_package));
}

MOLTEST(publish_reads_a_recipe_that_never_declared_a_form) {
    /* `form` is new, and the recipes published before it existed cannot be
       made to declare it. Absent means binary, so this one gets as far as
       looking for its archive -- which is a usage error, not a manifest one. */
    static const char *const older = "kind = \"package\"\n"
                                     "name = \"yyjson\"\n"
                                     "version = \"0.10.0\"\n"
                                     "target = \"any\"\n"
                                     "\n"
                                     "[package]\n"
                                     "include = [\"include\"]\n";

    EXPECT_EQ(exit_usage_error, publish(older));
}

MOLTEST(publish_reports_a_recipe_that_is_not_there) {
    EXPECT_EQ(exit_invalid_manifest, publish_command_run("/tmp/no_such_recipe.toml", NULL, true));
}

/* --- what the tables say, and not merely that they are there --- */

/* SOURCE_RECIPE with `what` swapped for `into`. */
static void replacing(const char *what, const char *into, char *out, size_t out_size) {
    const char *at = strstr(SOURCE_RECIPE, what);
    if (at == NULL) {
        snprintf(out, out_size, "%s", SOURCE_RECIPE);
        return;
    }
    const size_t head = (size_t)(at - SOURCE_RECIPE);
    snprintf(out, out_size, "%.*s%s%s", (int)head, SOURCE_RECIPE, into, at + strlen(what));
}

/* Each of these publishes cleanly while only the tables' presence is checked,
   and then fails in the build of everyone who depends on it. A coordinate is
   immutable, so the recipe that spent it cannot be corrected — only superseded
   by a version nobody is asking for. */
MOLTEST(publish_refuses_a_build_system_no_consumer_can_run) {
    char text[RECIPE_MAX];
    replacing("system = \"none\"", "system = \"scons\"", text, sizeof text);
    EXPECT_EQ(exit_invalid_manifest, publish(text));
}

MOLTEST(publish_refuses_an_artifact_type_nothing_can_build) {
    char text[RECIPE_MAX];
    replacing("type = \"source\"", "type = \"header_only\"", text, sizeof text);
    EXPECT_EQ(exit_invalid_manifest, publish(text));
}

MOLTEST(publish_refuses_a_standard_no_compiler_knows) {
    char text[RECIPE_MAX];
    replacing("type = \"source\"", "type = \"source\"\nstd = \"c47\"", text, sizeof text);
    EXPECT_EQ(exit_invalid_manifest, publish(text));
}

/* A URL is a promise about a location and not about content. Without the digest
   a consumer fetches whatever upstream serves today (RFC-0009). */
MOLTEST(publish_refuses_an_archive_with_no_digest_beside_it) {
    char text[RECIPE_MAX];
    replacing("sha256 = \"1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d\"\n", "",
              text, sizeof text);
    EXPECT_EQ(exit_invalid_manifest, publish(text));
}

/* The whole recipe, unmodified, gets past validation and on to the credential
   it needs -- which is what says the four refusals above are refusing something
   specific rather than everything. */
MOLTEST(publish_lets_a_sound_source_recipe_through_to_its_registry) {
    EXPECT_NE(exit_invalid_manifest, publish(SOURCE_RECIPE));
}

/* Every other build system does its own configuring, so a recipe that both
   names one and supplies its output says two contradictory things about who is
   in charge (RFC-0009). */
MOLTEST(publish_refuses_a_provision_beside_a_build_system_that_configures) {
    char text[RECIPE_MAX];
    snprintf(text, sizeof text,
             "%s\n[[provide]]\nfile = \"config.h\"\nfrom = \"config.h.generic\"\n", SOURCE_RECIPE);
    char *system = strstr(text, "\"none\"");
    ASSERT_NOT_NULL(system);
    memcpy(system, "\"make\"", 6);

    EXPECT_EQ(exit_invalid_manifest, publish(text));
}

MOLTEST(publish_lets_a_provision_through_beside_system_none) {
    char text[RECIPE_MAX];
    snprintf(text, sizeof text,
             "%s\n[[provide]]\nfile = \"config.h\"\nfrom = \"config.h.generic\"\n", SOURCE_RECIPE);

    EXPECT_NE(exit_invalid_manifest, publish(text));
}

/* A coordinate is immutable, so a version nothing can depend on is a coordinate
   spent for good. `pcre2@10.47` passed every other rule and then could not be
   named by a manifest, which refuses two components as "not a version". */
MOLTEST(publish_refuses_a_package_version_no_manifest_could_name) {
    char text[RECIPE_MAX];
    snprintf(text, sizeof text, "%s", SOURCE_RECIPE);
    char *version = strstr(text, "\"3.53.4\"");
    ASSERT_NOT_NULL(version);
    memcpy(version, "\"3.53\"\n ", 8);

    EXPECT_EQ(exit_invalid_manifest, publish(text));
}

/* And leaves the other two kinds alone: a toolchain called 13.2.0-x86_64 is a
   real thing, and RFC-0009 asks semver of a package alone. */
MOLTEST(publish_lets_a_toolchain_name_itself_what_it_likes) {
    static const char *const toolchain = "kind = \"toolchain\"\n"
                                         "name = \"gcc\"\n"
                                         "version = \"13.2.0-x86_64\"\n"
                                         "target = \"x86_64-unknown-linux-gnu\"\n"
                                         "\n"
                                         "[toolchain]\n"
                                         "cc = \"bin/gcc\"\n";

    EXPECT_NE(exit_invalid_manifest, publish(toolchain));
}
