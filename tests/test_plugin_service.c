#include <moltest.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/plugin_service.h>
#include <molto/services/recipe_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * A sandbox with its own HOME and its own PATH, so resolution is exercised
 * against directories this test made rather than against whatever the machine
 * running it happens to have installed.
 */
typedef struct {
    char root[64];
    char home[128];      /* $HOME, holding .molto/plugins/bin */
    char installed[192]; /* ~/.molto/plugins/bin */
    char recipes[192];   /* ~/.molto/plugins/recipes */
    char elsewhere[128]; /* a directory placed on PATH */
    char log[192];       /* what a stub plugin wrote when it ran */
    char old_home[4096];
    char old_path[4096];
} sandbox;

static bool remember(const char *name, char *out, size_t size) {
    const char *value = getenv(name);
    snprintf(out, size, "%s", value != NULL ? value : "");
    return true;
}

static bool sandbox_setup(sandbox *box) {
    if(!moltest_temp_dir("molto_plugins", box->root, sizeof box->root))
        return false;

    snprintf(box->home, sizeof box->home, "%s/home", box->root);
    snprintf(box->installed, sizeof box->installed, "%s/.molto/plugins/bin", box->home);
    snprintf(box->recipes, sizeof box->recipes, "%s/.molto/plugins/recipes", box->home);
    snprintf(box->elsewhere, sizeof box->elsewhere, "%s/elsewhere", box->root);
    snprintf(box->log, sizeof box->log, "%s/log", box->root);

    (void)remember("HOME", box->old_home, sizeof box->old_home);
    (void)remember("PATH", box->old_path, sizeof box->old_path);

    return fs_make_dirs(box->installed) && fs_make_dirs(box->recipes)
        && fs_make_dirs(box->elsewhere) && setenv("HOME", box->home, 1) == 0
        && setenv("PATH", box->elsewhere, 1) == 0;
}

static void sandbox_teardown(sandbox *box) {
    (void)setenv("HOME", box->old_home, 1);
    (void)setenv("PATH", box->old_path, 1);
    char command[128];
    (void)fs_remove_tree(box->root);
}

/* Write an executable `molto-<name>` into `dir` that records how it was called
   and exits with `code`. Output goes to the log rather than to the terminal,
   so a passing suite stays quiet. */
/* A plugin that records the directory it was found in and the arguments it was
   handed, then leaves with the status it was told to. A real program: a
   `#!/bin/sh` file is not something CreateProcess can start. */
MOLTEST_FAKE(fake_plugin) {
    const char *log = moltest_fake_setting("log");
    const char *dir = moltest_fake_setting("dir");
    FILE *file;
    if (log != NULL && (file = fopen(log, "ab")) != NULL) {
        fprintf(file, "%s", dir != NULL ? dir : "");
        for (int i = 1; i < argc; i++)
            fprintf(file, " %s", argv[i]);
        fprintf(file, "\n");
        (void)fclose(file);
    }
    const char *code = moltest_fake_setting("exit");
    return code != NULL ? atoi(code) : 0;
}

static bool plant(const sandbox *box, const char *dir, const char *name, int code) {
    char path[320];
    snprintf(path, sizeof path, "%s/molto-%s", dir, name);

    char spec[1024];
    if (snprintf(spec, sizeof spec, "set log %s\nset dir %s\nset exit %d\nbehave fake_plugin\n",
                 box->log, dir, code)
        >= (int)sizeof spec)
        return false;
    return moltest_fake_program(path, spec, NULL, 0);
}

/* The single line the stub wrote, or "" when it never ran. */
static void read_log(const sandbox *box, char *out, size_t size) {
    out[0] = '\0';
    char *content = fs_read_file(box->log);
    if(content == NULL)
        return;
    snprintf(out, size, "%s", content);
    free(content);
}

/* --- names --- */

MOLTEST(plugin_name_valid_accepts_what_a_recipe_would_carry) {
    EXPECT_TRUE(plugin_name_valid("deb"));
    EXPECT_TRUE(plugin_name_valid("appimage"));
    EXPECT_TRUE(plugin_name_valid("my-plugin"));
    EXPECT_TRUE(plugin_name_valid("my_plugin"));
    EXPECT_TRUE(plugin_name_valid("c2rust"));
    EXPECT_TRUE(plugin_name_valid("7zip"));
}

MOLTEST(plugin_name_valid_refuses_a_name_that_could_leave_the_directory) {
    /* The name becomes a filename, so a separator or a `..` is the whole
       reason this check exists. */
    EXPECT_FALSE(plugin_name_valid("../evil"));
    EXPECT_FALSE(plugin_name_valid("a/b"));
    EXPECT_FALSE(plugin_name_valid(".."));
    EXPECT_FALSE(plugin_name_valid("a.b"));
}

MOLTEST(plugin_name_valid_refuses_the_rest) {
    EXPECT_FALSE(plugin_name_valid(NULL));
    EXPECT_FALSE(plugin_name_valid(""));
    EXPECT_FALSE(plugin_name_valid("Deb"));  /* uppercase */
    EXPECT_FALSE(plugin_name_valid("-deb")); /* leading separator */
    EXPECT_FALSE(plugin_name_valid("_deb"));
    EXPECT_FALSE(plugin_name_valid("a b"));

    char too_long[PLUGIN_NAME_MAX + 8];
    memset(too_long, 'a', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = '\0';
    EXPECT_FALSE(plugin_name_valid(too_long));
}

/* --- where they are looked for --- */

MOLTEST(plugin_dir_hangs_off_home) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char dir[PLUGIN_PATH_MAX];
    EXPECT_TRUE(plugin_dir(dir, sizeof dir));
    EXPECT_STREQ(box.installed, dir);

    sandbox_teardown(&box);
}

MOLTEST(plugin_resolve_finds_nothing_when_nothing_provides_it) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char path[PLUGIN_PATH_MAX];
    EXPECT_FALSE(plugin_resolve("deb", path, sizeof path));

    sandbox_teardown(&box);
}

MOLTEST(plugin_resolve_finds_one_on_path) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.elsewhere, "deb", 0));

    char path[PLUGIN_PATH_MAX];
    EXPECT_TRUE(plugin_resolve("deb", path, sizeof path));
    EXPECT_TRUE(strstr(path, "elsewhere/molto-deb") != NULL);

    sandbox_teardown(&box);
}

MOLTEST(plugin_resolve_prefers_the_installed_one_over_path) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.elsewhere, "deb", 0));
    ASSERT_TRUE(plant(&box, box.installed, "deb", 0));

    /* What `molto plugin install` put there wins over whatever a shell profile
       happened to put earlier on PATH. */
    char path[PLUGIN_PATH_MAX];
    EXPECT_TRUE(plugin_resolve("deb", path, sizeof path));
    EXPECT_TRUE(strstr(path, ".molto/plugins/bin/molto-deb") != NULL);

    sandbox_teardown(&box);
}

MOLTEST(plugin_resolve_ignores_a_file_that_is_not_executable) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char path[320];
    snprintf(path, sizeof path, "%s/molto-deb", box.elsewhere);
    ASSERT_TRUE(fs_write_file(path, "not a program"));

    char found[PLUGIN_PATH_MAX];
    EXPECT_FALSE(plugin_resolve("deb", found, sizeof found));

    sandbox_teardown(&box);
}

MOLTEST(plugin_resolve_refuses_an_invalid_name_without_looking) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char path[PLUGIN_PATH_MAX];
    EXPECT_FALSE(plugin_resolve("../deb", path, sizeof path));
    EXPECT_FALSE(plugin_resolve("", path, sizeof path));

    sandbox_teardown(&box);
}

/* --- running one --- */

MOLTEST(plugin_run_propagates_the_plugins_own_exit_code) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.elsewhere, "deb", 7));

    char path[PLUGIN_PATH_MAX];
    ASSERT_TRUE(plugin_resolve("deb", path, sizeof path));

    /* A subcommand's answer reaches the caller untranslated: 7 is the plugin
       speaking, not a Molto failure. */
    EXPECT_EQ(7, plugin_run(path, 0, NULL));

    sandbox_teardown(&box);
}

MOLTEST(plugin_run_hands_the_arguments_through_untouched) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.elsewhere, "deb", 0));

    char path[PLUGIN_PATH_MAX];
    ASSERT_TRUE(plugin_resolve("deb", path, sizeof path));

    char *args[] = { "--output", "x.deb", "--strip" };
    EXPECT_EQ(0, plugin_run(path, 3, args));

    char log[512];
    read_log(&box, log, sizeof log);
    EXPECT_TRUE(strstr(log, "--output x.deb --strip") != NULL);

    sandbox_teardown(&box);
}

MOLTEST(plugin_run_reports_a_plugin_that_never_ran) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char missing[PLUGIN_PATH_MAX];
    snprintf(missing, sizeof missing, "%s/molto-nothing", box.elsewhere);

    /* Nothing there to execute: the plugin broke, which is not the same fact
       as a plugin that ran and answered. */
    EXPECT_EQ(exit_plugin_failure, plugin_run(missing, 0, NULL));

    sandbox_teardown(&box);
}

/* --- the recipe beside a plugin --- */

/* A complete plugin recipe, so a test that cares about one key does not have
   to restate the other six. */
#define A_PLUGIN_RECIPE \
    "schema = 2\n" \
    "form = \"binary\"\n" \
    "kind = \"tool\"\n" \
    "name = \"deb\"\n" \
    "version = \"1.2.0\"\n" \
    "target = \"x86_64-unknown-linux-gnu\"\n" \
    "\n" \
    "[tool]\n" \
    "kind = \"plugin\"\n" \
    "\n" \
    "[plugin]\n" \
    "capabilities = [\"packager\", \"command\"]\n" \
    "extensions = []\n" \
    "permissions = [\"project.read\", \"process.spawn\"]\n" \
    "ir_schema = 1\n" \
    "molto_min = \"0.17.0\"\n"

static bool write_recipe(const sandbox *box, const char *name, const char *body) {
    char path[320];
    snprintf(path, sizeof path, "%s/%s.toml", box->recipes, name);
    return fs_write_file(path, body);
}

MOLTEST(plugin_read_recipe_reads_what_the_plugin_declared) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.installed, "deb", 0));
    ASSERT_TRUE(write_recipe(&box, "deb", A_PLUGIN_RECIPE));

    recipe_coordinate coordinate;
    recipe_plugin plugin;
    char err[256] = "";
    ASSERT_TRUE(plugin_read_recipe("deb", &coordinate, &plugin, err, sizeof err));

    EXPECT_STREQ("1.2.0", coordinate.version);
    EXPECT_STREQ("x86_64-unknown-linux-gnu", coordinate.target);
    EXPECT_EQ(2, plugin.capability_count);
    EXPECT_STREQ("packager", plugin.capabilities[0]);
    EXPECT_STREQ("command", plugin.capabilities[1]);
    EXPECT_EQ(2, plugin.permission_count);
    EXPECT_STREQ("process.spawn", plugin.permissions[1]);
    EXPECT_EQ(0, plugin.extension_count);
    EXPECT_EQ(1, plugin.ir_schema);
    EXPECT_STREQ("0.17.0", plugin.molto_min);

    sandbox_teardown(&box);
}

MOLTEST(plugin_read_recipe_says_so_when_there_is_none) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.elsewhere, "hello", 0));

    /* A plugin being developed against PATH has no recipe, and that is a fact
       to report rather than an error. */
    char err[256] = "";
    EXPECT_FALSE(plugin_read_recipe("hello", NULL, NULL, err, sizeof err));
    EXPECT_TRUE(strstr(err, "no recipe") != NULL);

    sandbox_teardown(&box);
}

MOLTEST(plugin_read_recipe_refuses_one_without_a_plugin_table) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(write_recipe(&box, "deb",
                             "schema = 1\nform = \"binary\"\nkind = \"tool\"\n"
                             "name = \"deb\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"));

    /* A recipe with no [plugin] declares nothing, and reading it as "asks for
       nothing" would be the dangerous reading. */
    char err[256] = "";
    EXPECT_FALSE(plugin_read_recipe("deb", NULL, NULL, err, sizeof err));
    EXPECT_TRUE(strstr(err, "[plugin]") != NULL);

    sandbox_teardown(&box);
}

MOLTEST(plugin_read_recipe_refuses_a_plugin_table_under_the_schema_that_added_it) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(write_recipe(&box, "deb",
                             "schema = 1\nform = \"binary\"\nkind = \"tool\"\n"
                             "name = \"deb\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                             "\n[plugin]\ncapabilities = [\"command\"]\n"));

    /* `[plugin]` is schema 2. Carrying it while declaring 1 makes a recipe that
       a molto predating plugins accepts, skips the table of, and installs — so
       the permissions are shown to nobody. Refused here so it is not written. */
    char err[256] = "";
    EXPECT_FALSE(plugin_read_recipe("deb", NULL, NULL, err, sizeof err));
    EXPECT_TRUE(strstr(err, "schema") != NULL);

    sandbox_teardown(&box);
}

MOLTEST(plugin_read_recipe_refuses_a_plugin_table_with_no_schema_at_all) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(write_recipe(&box, "deb",
                             "form = \"binary\"\nkind = \"tool\"\n"
                             "name = \"deb\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                             "\n[plugin]\ncapabilities = [\"command\"]\n"));

    /* The sneakier half of the same hazard: an absent schema means 1, so a
       recipe that simply never mentions one is the case above written by
       omission rather than by claim. */
    char err[256] = "";
    EXPECT_FALSE(plugin_read_recipe("deb", NULL, NULL, err, sizeof err));
    EXPECT_TRUE(strstr(err, "schema") != NULL);

    sandbox_teardown(&box);
}

MOLTEST(plugin_read_recipe_refuses_one_that_provides_no_capability) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(write_recipe(&box, "deb",
                             "schema = 2\nform = \"binary\"\nkind = \"tool\"\n"
                             "name = \"deb\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                             "\n[plugin]\ncapabilities = []\n"));

    char err[256] = "";
    EXPECT_FALSE(plugin_read_recipe("deb", NULL, NULL, err, sizeof err));
    EXPECT_TRUE(strstr(err, "capabilities") != NULL);

    sandbox_teardown(&box);
}

MOLTEST(plugin_read_recipe_keeps_a_permission_it_does_not_recognise) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(write_recipe(&box, "deb",
                             "schema = 2\nform = \"binary\"\nkind = \"tool\"\n"
                             "name = \"deb\"\nversion = \"1.0.0\"\ntarget = \"any\"\n"
                             "\n[plugin]\ncapabilities = [\"command\"]\n"
                             "permissions = [\"invented.by.nobody\"]\n"));

    /* Dropping it would report the plugin as asking for less than it does,
       which is the one place ignoring the unknown is dangerous (RFC-0014). */
    recipe_plugin plugin;
    char err[256] = "";
    ASSERT_TRUE(plugin_read_recipe("deb", NULL, &plugin, err, sizeof err));
    EXPECT_EQ(1, plugin.permission_count);
    EXPECT_STREQ("invented.by.nobody", plugin.permissions[0]);

    sandbox_teardown(&box);
}

/* --- listing --- */

MOLTEST(plugin_list_finds_nothing_when_nothing_is_installed) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    plugin_entry entries[PLUGIN_MAX_LISTED];
    size_t count = 99;
    EXPECT_TRUE(plugin_list(entries, PLUGIN_MAX_LISTED, &count));
    EXPECT_EQ(0, count);

    sandbox_teardown(&box);
}

MOLTEST(plugin_list_reports_both_origins) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.installed, "deb", 0));
    ASSERT_TRUE(plant(&box, box.elsewhere, "hello", 0));

    plugin_entry entries[PLUGIN_MAX_LISTED];
    size_t count = 0;
    ASSERT_TRUE(plugin_list(entries, PLUGIN_MAX_LISTED, &count));
    ASSERT_EQ(2, count);

    /* Installed first, because that is the copy that would run. */
    EXPECT_STREQ("deb", entries[0].name);
    EXPECT_EQ(plugin_origin_installed, entries[0].origin);
    EXPECT_STREQ("hello", entries[1].name);
    EXPECT_EQ(plugin_origin_path, entries[1].origin);

    sandbox_teardown(&box);
}

MOLTEST(plugin_list_names_a_plugin_once_however_many_places_hold_it) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.installed, "deb", 0));
    ASSERT_TRUE(plant(&box, box.elsewhere, "deb", 0));

    plugin_entry entries[PLUGIN_MAX_LISTED];
    size_t count = 0;
    ASSERT_TRUE(plugin_list(entries, PLUGIN_MAX_LISTED, &count));

    /* One name, one entry, and it is the one plugin_resolve would run. */
    ASSERT_EQ(1, count);
    EXPECT_EQ(plugin_origin_installed, entries[0].origin);

    sandbox_teardown(&box);
}

MOLTEST(plugin_list_sorts_by_name) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.installed, "rpm", 0));
    ASSERT_TRUE(plant(&box, box.installed, "deb", 0));
    ASSERT_TRUE(plant(&box, box.installed, "lambda", 0));

    plugin_entry entries[PLUGIN_MAX_LISTED];
    size_t count = 0;
    ASSERT_TRUE(plugin_list(entries, PLUGIN_MAX_LISTED, &count));
    ASSERT_EQ(3, count);

    /* readdir's order is the filesystem's; two runs must agree anyway. */
    EXPECT_STREQ("deb", entries[0].name);
    EXPECT_STREQ("lambda", entries[1].name);
    EXPECT_STREQ("rpm", entries[2].name);

    sandbox_teardown(&box);
}

MOLTEST(plugin_list_says_which_ones_carry_a_recipe) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.installed, "deb", 0));
    ASSERT_TRUE(write_recipe(&box, "deb", A_PLUGIN_RECIPE));
    ASSERT_TRUE(plant(&box, box.installed, "rpm", 0));

    plugin_entry entries[PLUGIN_MAX_LISTED];
    size_t count = 0;
    ASSERT_TRUE(plugin_list(entries, PLUGIN_MAX_LISTED, &count));
    ASSERT_EQ(2, count);

    EXPECT_TRUE(entries[0].has_recipe);
    EXPECT_FALSE(entries[1].has_recipe);

    sandbox_teardown(&box);
}

MOLTEST(plugin_list_ignores_what_is_not_a_plugin) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char path[320];
    /* No prefix, an unusable name, and a file nobody may execute. */
    snprintf(path, sizeof path, "%s/notaplugin", box.installed);
    ASSERT_TRUE(fs_write_file(path, "#!/bin/sh\n"));
    ASSERT_TRUE(chmod(path, 0755) == 0);
    snprintf(path, sizeof path, "%s/molto-Deb", box.installed);
    ASSERT_TRUE(fs_write_file(path, "#!/bin/sh\n"));
    ASSERT_TRUE(chmod(path, 0755) == 0);
    snprintf(path, sizeof path, "%s/molto-quiet", box.installed);
    ASSERT_TRUE(fs_write_file(path, "#!/bin/sh\n"));

    plugin_entry entries[PLUGIN_MAX_LISTED];
    size_t count = 0;
    ASSERT_TRUE(plugin_list(entries, PLUGIN_MAX_LISTED, &count));
    EXPECT_EQ(0, count);

    sandbox_teardown(&box);
}

MOLTEST(plugin_list_reports_a_listing_that_does_not_fit) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(plant(&box, box.installed, "deb", 0));
    ASSERT_TRUE(plant(&box, box.installed, "rpm", 0));

    /* A plugin missing from a listing is one nobody knows is installed, so the
       overflow is reported rather than quietly cut short. */
    plugin_entry entries[1];
    size_t count = 0;
    EXPECT_FALSE(plugin_list(entries, 1, &count));

    sandbox_teardown(&box);
}
