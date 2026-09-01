#include <moltest.h>

#include <molto/cli.h>
#include <molto/commands/plugin_command.h>
#include <molto/exit_code.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* A HOME of its own, so the listing describes what this test installed rather
   than what the machine running it happens to carry. PATH is emptied for the
   same reason. */
typedef struct {
    char root[64];
    char bin[192];
    char old_home[4096];
    char old_path[4096];
} home;

static bool home_setup(home *box) {
    if(!moltest_temp_dir("molto_plugin_cmd", box->root, sizeof box->root))
        return false;
    snprintf(box->bin, sizeof box->bin, "%s/.molto/plugins/bin", box->root);

    const char *home_value = getenv("HOME");
    const char *path_value = getenv("PATH");
    snprintf(box->old_home, sizeof box->old_home, "%s", home_value != NULL ? home_value : "");
    snprintf(box->old_path, sizeof box->old_path, "%s", path_value != NULL ? path_value : "");

    return fs_make_dirs(box->bin) && setenv("HOME", box->root, 1) == 0
        && setenv("PATH", "", 1) == 0;
}

static void home_teardown(home *box) {
    (void)setenv("HOME", box->old_home, 1);
    (void)setenv("PATH", box->old_path, 1);
    char command[128];
    (void)fs_remove_tree(box->root);
}

static bool install(const home *box, const char *name) {
    char path[320];
    snprintf(path, sizeof path, "%s/molto-%s", box->bin, name);
    /* A real program, not a `#!/bin/sh` file: a shebang is honoured by the
       kernel and ignored by CreateProcess. */
    return moltest_fake_program(path, "exit 0\n", NULL, 0);
}

MOLTEST(plugin_command_lists_an_empty_machine_without_failing) {
    home box;
    ASSERT_TRUE(home_setup(&box));

    /* Nothing installed is an answer, not a failure. */
    EXPECT_EQ(exit_ok, plugin_command_run("list", NULL, NULL, false));

    home_teardown(&box);
}

MOLTEST(plugin_command_defaults_to_listing) {
    home box;
    ASSERT_TRUE(home_setup(&box));
    ASSERT_TRUE(install(&box, "deb"));

    EXPECT_EQ(exit_ok, plugin_command_run(NULL, NULL, NULL, false));
    EXPECT_EQ(exit_ok, plugin_command_run("list", NULL, NULL, false));

    home_teardown(&box);
}

MOLTEST(plugin_command_refuses_an_action_it_does_not_have) {
    EXPECT_EQ(exit_usage_error, plugin_command_run("instal", NULL, NULL, false));
    EXPECT_EQ(exit_usage_error, plugin_command_run("uninstall", "deb", NULL, false));
}

MOLTEST(plugin_command_info_needs_a_name) {
    EXPECT_EQ(exit_usage_error, plugin_command_run("info", NULL, NULL, false));
}

MOLTEST(plugin_command_info_reports_a_plugin_that_is_not_there) {
    home box;
    ASSERT_TRUE(home_setup(&box));

    /* Not a usage error: the command was used correctly and the answer is that
       nothing provides that name. */
    EXPECT_EQ(exit_dependency_failure, plugin_command_run("info", "nosuch", NULL, false));

    home_teardown(&box);
}

MOLTEST(plugin_command_info_works_without_a_recipe) {
    home box;
    ASSERT_TRUE(home_setup(&box));
    ASSERT_TRUE(install(&box, "deb"));

    /* Installed by hand, so nothing recorded what it asked for. What is known
       is still worth printing. */
    EXPECT_EQ(exit_ok, plugin_command_run("info", "deb", NULL, false));

    home_teardown(&box);
}

MOLTEST(cli_has_command_knows_the_built_ins) {
    /* What tells `plugin list` that a `molto-build` on PATH is unreachable. */
    EXPECT_TRUE(cli_has_command("build"));
    EXPECT_TRUE(cli_has_command("plugin"));
    EXPECT_FALSE(cli_has_command("deb"));
    EXPECT_FALSE(cli_has_command(""));
}

MOLTEST(plugin_command_install_needs_a_name) {
    EXPECT_EQ(exit_usage_error, plugin_command_run("install", NULL, NULL, true));
}

MOLTEST(plugin_command_remove_needs_a_name) {
    EXPECT_EQ(exit_usage_error, plugin_command_run("remove", NULL, NULL, false));
}

MOLTEST(plugin_command_refuses_to_remove_what_it_did_not_install) {
    home box;
    ASSERT_TRUE(home_setup(&box));

    /* A plugin that arrived on PATH was put there by someone else. Removing
       nothing and reporting success would be worse than saying so. */
    EXPECT_EQ(exit_dependency_failure, plugin_command_run("remove", "deb", NULL, false));

    home_teardown(&box);
}

MOLTEST(plugin_command_removes_what_it_installed) {
    home box;
    ASSERT_TRUE(home_setup(&box));
    ASSERT_TRUE(install(&box, "deb"));

    EXPECT_EQ(exit_ok, plugin_command_run("remove", "deb", NULL, false));
    /* Gone for good: a second removal has nothing to find. */
    EXPECT_EQ(exit_dependency_failure, plugin_command_run("remove", "deb", NULL, false));

    home_teardown(&box);
}
