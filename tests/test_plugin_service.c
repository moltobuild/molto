#include <moltest.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/plugin_service.h>

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
    snprintf(box->root, sizeof box->root, "%s", "/tmp/molto_plugins_XXXXXX");
    if(mkdtemp(box->root) == NULL)
        return false;

    snprintf(box->home, sizeof box->home, "%s/home", box->root);
    snprintf(box->installed, sizeof box->installed, "%s/.molto/plugins/bin", box->home);
    snprintf(box->elsewhere, sizeof box->elsewhere, "%s/elsewhere", box->root);
    snprintf(box->log, sizeof box->log, "%s/log", box->root);

    (void)remember("HOME", box->old_home, sizeof box->old_home);
    (void)remember("PATH", box->old_path, sizeof box->old_path);

    return fs_make_dirs(box->installed) && fs_make_dirs(box->elsewhere)
        && setenv("HOME", box->home, 1) == 0 && setenv("PATH", box->elsewhere, 1) == 0;
}

static void sandbox_teardown(sandbox *box) {
    (void)setenv("HOME", box->old_home, 1);
    (void)setenv("PATH", box->old_path, 1);
    char command[128];
    snprintf(command, sizeof command, "rm -rf %s", box->root);
    (void)system(command);
}

/* Write an executable `molto-<name>` into `dir` that records how it was called
   and exits with `code`. Output goes to the log rather than to the terminal,
   so a passing suite stays quiet. */
static bool plant(const sandbox *box, const char *dir, const char *name, int code) {
    char path[320];
    snprintf(path, sizeof path, "%s/molto-%s", dir, name);

    char script[1024];
    snprintf(script, sizeof script,
             "#!/bin/sh\n"
             "echo \"%s $*\" >> %s\n"
             "exit %d\n",
             dir, box->log, code);
    return fs_write_file(path, script) && chmod(path, 0755) == 0;
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
