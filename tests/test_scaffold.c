#include <moltest.h>

#include <molto/cli.h>
#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/scaffold_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

MOLTEST(scaffold) {
    char root[MOLTEST_PATH];
    EXPECT_TRUE(moltest_temp_dir("molto_scaffold", root, sizeof root));

    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    EXPECT_TRUE(scaffold_project(project, "demo", project_kind_binary) == exit_ok);

    char path[700];
    /* Layout: Project.toml, src/, tests/, and a starter src/main.c. */
    snprintf(path, sizeof path, "%s/Project.toml", project);
    EXPECT_TRUE(fs_path_exists(path));
    snprintf(path, sizeof path, "%s/src", project);
    EXPECT_TRUE(fs_is_dir(path));
    snprintf(path, sizeof path, "%s/tests", project);
    EXPECT_TRUE(fs_is_dir(path));

    snprintf(path, sizeof path, "%s/src/main.c", project);
    EXPECT_TRUE(fs_path_exists(path));
    char *main_c = fs_read_file(path);
    EXPECT_TRUE(main_c != NULL);
    if (main_c != NULL) {
        EXPECT_TRUE(strstr(main_c, "int main(void)") != NULL);
        EXPECT_TRUE(strstr(main_c, "Hello, world!") != NULL);
        free(main_c);
    }

    /* Re-scaffolding must not clobber an existing main.c (and reports the
       manifest already exists). */
    EXPECT_TRUE(fs_write_file(path, "int main(void) { return 7; }\n"));
    EXPECT_TRUE(scaffold_project(project, "demo", project_kind_binary) == exit_invalid_manifest);
    char *kept = fs_read_file(path);
    EXPECT_TRUE(kept != NULL && strstr(kept, "return 7") != NULL);
    free(kept);

    char cmd[700];
    (void)fs_remove_tree(root);
}

MOLTEST(scaffold_creates_the_include_directory_the_manifest_declares) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_include", root, sizeof root));

    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    ASSERT_TRUE(scaffold_project(project, "demo", project_kind_binary) == exit_ok);

    /* The generated manifest declares include = ["include"], so the directory
       has to exist: a manifest that points at nothing is worse than one that
       says nothing. */
    char path[700];
    snprintf(path, sizeof path, "%s/include", project);
    EXPECT_TRUE(fs_is_dir(path));

    char cmd[700];
    (void)fs_remove_tree(root);
}

MOLTEST(scaffold_ignores_the_directories_molto_owns) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_ignore", root, sizeof root));

    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    ASSERT_TRUE(scaffold_project(project, "demo", project_kind_binary) == exit_ok);

    /* Without this, `git add -A` on a fresh project commits the build output
       and the workspace database, which is binary and changes on every build. */
    char path[700];
    snprintf(path, sizeof path, "%s/.gitignore", project);
    ASSERT_TRUE(fs_path_exists(path));
    char *ignore = fs_read_file(path);
    ASSERT_NOT_NULL(ignore);
    EXPECT_NOT_NULL(strstr(ignore, "/build/"));
    EXPECT_NOT_NULL(strstr(ignore, "/.bin/"));
    /* And the compilation database, which every build rewrites from the
       manifest and the tree: committing it would put one developer's absolute
       paths in everyone else's checkout. */
    EXPECT_NOT_NULL(strstr(ignore, "/compile_commands.json"));
    free(ignore);

    char cmd[700];
    (void)fs_remove_tree(root);
}

MOLTEST(scaffold_keeps_an_existing_gitignore) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_keepignore", root, sizeof root));

    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    ASSERT_TRUE(fs_make_dirs(project));

    /* A project being adopted may already have one: it is the user's file. */
    char path[700];
    snprintf(path, sizeof path, "%s/.gitignore", project);
    ASSERT_TRUE(fs_write_file(path, "*.log\n"));

    ASSERT_TRUE(scaffold_project(project, "demo", project_kind_binary) == exit_ok);

    char *ignore = fs_read_file(path);
    ASSERT_NOT_NULL(ignore);
    EXPECT_NOT_NULL(strstr(ignore, "*.log"));
    EXPECT_NULL(strstr(ignore, "/build/"));
    free(ignore);

    char cmd[700];
    (void)fs_remove_tree(root);
}

/* Whether `path` under `project` exists, and holds `needle` when one is given. */
static bool holds(const char *project, const char *relative, const char *needle) {
    char path[700];
    snprintf(path, sizeof path, "%s/%s", project, relative);
    if (needle == NULL)
        return fs_path_exists(path);
    char *text = fs_read_file(path);
    const bool found = text != NULL && strstr(text, needle) != NULL;
    free(text);
    return found;
}

MOLTEST(scaffold_library_writes_a_header_a_source_and_a_moltest_suite) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_lib", root, sizeof root));
    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    ASSERT_TRUE(scaffold_project(project, "demo", project_kind_library) == exit_ok);

    EXPECT_TRUE(holds(project, "include/demo.h", "#ifndef DEMO_H"));
    EXPECT_TRUE(holds(project, "include/demo.h", "int demo_add(int a, int b);"));
    EXPECT_TRUE(holds(project, "src/demo.c", "#include <demo.h>"));
    EXPECT_TRUE(holds(project, "tests/test_demo.c", "#include <moltest.h>"));
    EXPECT_TRUE(holds(project, "tests/test_demo.c", "DESCRIBE(demo_add_sums_its_arguments)"));
    EXPECT_TRUE(holds(project, "Project.toml", "artifact = \"static\""));
    EXPECT_TRUE(holds(project, "Project.toml", "[dev-deps]"));
    /* A library has nothing to run, so it gets no entry point. */
    EXPECT_FALSE(holds(project, "src/main.c", NULL));

    (void)fs_remove_tree(root);
}

MOLTEST(scaffold_library_keeps_existing_sources) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_libkeep", root, sizeof root));
    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    char dir[700];
    snprintf(dir, sizeof dir, "%s/src", project);
    ASSERT_TRUE(fs_make_dirs(dir));
    char path[700];
    snprintf(path, sizeof path, "%s/src/demo.c", project);
    ASSERT_TRUE(fs_write_file(path, "int mine;\n"));

    ASSERT_TRUE(scaffold_project(project, "demo", project_kind_library) == exit_ok);
    EXPECT_TRUE(holds(project, "src/demo.c", "int mine;"));

    (void)fs_remove_tree(root);
}

MOLTEST(scaffold_binary_declares_an_executable) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_bin", root, sizeof root));
    char project[600];
    snprintf(project, sizeof project, "%s/demo", root);
    ASSERT_TRUE(scaffold_project(project, "demo", project_kind_binary) == exit_ok);

    EXPECT_TRUE(holds(project, "Project.toml", "artifact = \"executable\""));
    EXPECT_FALSE(holds(project, "Project.toml", "[dev-deps]"));
    EXPECT_TRUE(holds(project, "src/main.c", "int main(void)"));
    EXPECT_FALSE(holds(project, "include/demo.h", NULL));

    (void)fs_remove_tree(root);
}

/* `molto new` with no flag is a library; `--bin` asks for a program, and both
   flags at once is a usage error that writes nothing. */
MOLTEST(new_makes_a_library_unless_asked_for_a_binary) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_newkind", root, sizeof root));
    char previous[1024];
    ASSERT_NOT_NULL(getcwd(previous, sizeof previous));
    ASSERT_TRUE(chdir(root) == 0);

    char *plain[] = { "molto", "new", "plain" };
    EXPECT_EQ(exit_ok, cli_run(3, plain));
    char *bin[] = { "molto", "new", "tool", "--bin" };
    EXPECT_EQ(exit_ok, cli_run(4, bin));
    char *lib[] = { "molto", "new", "explicit", "--lib" };
    EXPECT_EQ(exit_ok, cli_run(4, lib));
    char *both[] = { "molto", "new", "neither", "--lib", "--bin" };
    EXPECT_EQ(exit_usage_error, cli_run(5, both));

    EXPECT_TRUE(chdir(previous) == 0);

    EXPECT_TRUE(holds(root, "plain/Project.toml", "artifact = \"static\""));
    EXPECT_TRUE(holds(root, "explicit/Project.toml", "artifact = \"static\""));
    EXPECT_TRUE(holds(root, "tool/Project.toml", "artifact = \"executable\""));
    EXPECT_FALSE(holds(root, "neither", NULL));

    (void)fs_remove_tree(root);
}
