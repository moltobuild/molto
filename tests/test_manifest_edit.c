#include <moltest.h>

#include <molto/project/manifest_edit.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Editing a manifest without rewriting it.
 *
 * Every case here is really the same claim: the file is the user's, and what
 * the command was not asked to change comes back byte for byte. Asserting on
 * the whole text rather than on a re-parse is deliberate — a round trip that
 * agreed with itself while reformatting everything would pass. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];
    char path[PATH_MAX_LEN];
} sandbox;

static bool sandbox_open(sandbox *at, const char *manifest) {
    snprintf(at->root, sizeof at->root, "%s", "/tmp/molto_edit_XXXXXX");
    return mkdtemp(at->root) != NULL &&
           fs_format_path(at->path, sizeof at->path, "%s/Project.toml", at->root) &&
           fs_write_file(at->path, manifest);
}

static void sandbox_close(const sandbox *at) { (void)fs_remove_tree(at->root); }

/* Reads the edited manifest back. Caller frees. */
static char *read_back(const sandbox *at) { return fs_read_file(at->path); }

static const char *const COMMENTED = "# What this project is.\n"
                                     "[package]\n"
                                     "name    = \"app\"      # aligned on purpose\n"
                                     "version = \"0.1.0\"\n"
                                     "\n"
                                     "# Things we depend on.\n"
                                     "[deps]\n"
                                     "yyjson = \"0.10.0\"   # the JSON reader\n";

/* The whole point: a manifest is a file someone wrote. */
MOLTEST(adding_a_dependency_keeps_every_comment_and_every_space) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at, COMMENTED));

    char err[512] = "";
    ASSERT_TRUE(manifest_add_dep(at.path, "deps", "sqlite", "\"3.53.4\"", err, sizeof err));

    char *text = read_back(&at);
    ASSERT_NOT_NULL(text);
    EXPECT_NOT_NULL(strstr(text, "# What this project is.\n"));
    EXPECT_NOT_NULL(strstr(text, "name    = \"app\"      # aligned on purpose\n"));
    EXPECT_NOT_NULL(strstr(text, "# Things we depend on.\n"));
    EXPECT_NOT_NULL(strstr(text, "yyjson = \"0.10.0\"   # the JSON reader\n"));
    EXPECT_NOT_NULL(strstr(text, "sqlite = \"3.53.4\"\n"));

    free(text);
    sandbox_close(&at);
}

/* A new entry joins the table it belongs to, not the end of the file. */
MOLTEST(a_new_entry_goes_inside_its_table) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at, "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                                  "[deps]\na = \"1.0.0\"\n\n[target]\nstd = \"c17\"\n"));

    char err[512] = "";
    ASSERT_TRUE(manifest_add_dep(at.path, "deps", "b", "\"2.0.0\"", err, sizeof err));

    char *text = read_back(&at);
    ASSERT_NOT_NULL(text);
    /* Between the table it belongs to and the one after it. */
    EXPECT_TRUE(strstr(text, "b = \"2.0.0\"") < strstr(text, "[target]"));
    EXPECT_TRUE(strstr(text, "a = \"1.0.0\"") < strstr(text, "b = \"2.0.0\""));

    free(text);
    sandbox_close(&at);
}

/* Re-adding is a version bump, and it happens where the entry already is. */
MOLTEST(re_adding_replaces_in_place_and_keeps_the_comment) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at, COMMENTED));

    char err[512] = "";
    ASSERT_TRUE(manifest_add_dep(at.path, "deps", "yyjson", "\"0.11.0\"", err, sizeof err));

    char *text = read_back(&at);
    ASSERT_NOT_NULL(text);
    EXPECT_NOT_NULL(strstr(text, "yyjson = \"0.11.0\" # the JSON reader\n"));
    EXPECT_NULL(strstr(text, "0.10.0"));

    free(text);
    sandbox_close(&at);
}

/* A table that is not there yet is appended, and the file keeps parsing. */
MOLTEST(a_missing_table_is_created) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at, "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"));

    char err[512] = "";
    ASSERT_TRUE(manifest_add_dep(at.path, "dev-deps", "moltest", "\"0.4.1\"", err, sizeof err));

    char *text = read_back(&at);
    ASSERT_NOT_NULL(text);
    EXPECT_NOT_NULL(strstr(text, "\n[dev-deps]\nmoltest = \"0.4.1\"\n"));

    free(text);
    sandbox_close(&at);
}

/* One package is one version (RFC-0008), so a name cannot sit in both tables.
   Caught here, where the message can name the table, rather than later as a
   resolution conflict against yourself. */
MOLTEST(a_name_cannot_be_in_both_tables) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at, "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                                  "[deps]\npng = \"1.6.40\"\n"));

    char err[512] = "";
    EXPECT_FALSE(manifest_add_dep(at.path, "dev-deps", "png", "\"1.5.30\"", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "[deps]"));

    /* And the file is untouched. */
    char *text = read_back(&at);
    ASSERT_NOT_NULL(text);
    EXPECT_NULL(strstr(text, "dev-deps"));

    free(text);
    sandbox_close(&at);
}

MOLTEST(removing_takes_out_one_line_and_nothing_else) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at, COMMENTED));

    char err[512] = "";
    ASSERT_TRUE(manifest_remove_dep(at.path, "yyjson", err, sizeof err));

    char *text = read_back(&at);
    ASSERT_NOT_NULL(text);
    EXPECT_NULL(strstr(text, "yyjson"));
    /* The table header and every comment stay: removing the last entry of a
       table does not remove the table. */
    EXPECT_NOT_NULL(strstr(text, "# Things we depend on.\n[deps]\n"));
    EXPECT_NOT_NULL(strstr(text, "# What this project is.\n"));

    free(text);
    sandbox_close(&at);
}

/* The long form is a table, so removing it takes the header with it. */
MOLTEST(removing_the_long_form_takes_the_whole_table) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at, "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                                  "[deps.http]\npath = \"modules/http\"\n\n"
                                  "[target]\nstd = \"c17\"\n"));

    char err[512] = "";
    ASSERT_TRUE(manifest_remove_dep(at.path, "http", err, sizeof err));

    char *text = read_back(&at);
    ASSERT_NOT_NULL(text);
    EXPECT_NULL(strstr(text, "http"));
    EXPECT_NOT_NULL(strstr(text, "[target]\nstd = \"c17\"\n"));

    free(text);
    sandbox_close(&at);
}

/* Removing what is not there is an error. Silence would leave the user
   believing a dependency is gone when the next build will compile it. */
MOLTEST(removing_what_is_not_there_says_so) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at, "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"));

    char err[512] = "";
    EXPECT_FALSE(manifest_remove_dep(at.path, "absent", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "not a dependency"));

    sandbox_close(&at);
}

/* An edit that would produce a manifest Molto cannot read is refused, and the
   file on disk is left exactly as it was. */
MOLTEST(an_edit_that_would_break_the_manifest_is_refused) {
    sandbox at;
    ASSERT_TRUE(sandbox_open(&at, "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"));

    char err[512] = "";
    /* A range: valid TOML, refused by the manifest reader (RFC-0008). */
    EXPECT_FALSE(manifest_add_dep(at.path, "deps", "png", "\"^1.6.0\"", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "unreadable manifest"));

    char *text = read_back(&at);
    ASSERT_NOT_NULL(text);
    EXPECT_NULL(strstr(text, "png"));

    free(text);
    sandbox_close(&at);
}

MOLTEST(finding_which_table_holds_a_dependency) {
    EXPECT_STREQ("deps", manifest_find_dep("[deps]\na = \"1.0.0\"\n", "a"));
    EXPECT_STREQ("dev-deps", manifest_find_dep("[dev-deps]\nb = \"1.0.0\"\n", "b"));
    EXPECT_STREQ("deps", manifest_find_dep("[deps.c]\npath = \"x\"\n", "c"));
    EXPECT_NULL(manifest_find_dep("[deps]\na = \"1.0.0\"\n", "z"));
    /* A key of another table is not a dependency, whatever it is called. */
    EXPECT_NULL(manifest_find_dep("[target]\nstd = \"c17\"\n", "std"));
}
