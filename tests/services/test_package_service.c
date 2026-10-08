#include <moltest.h>

#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/deps_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/package_service.h>
#include <molto/services/process_service.h>
#include <molto/util/sha256.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool write_at(const char *root, const char *file, const char *text) {
    char path[4096], parent[4096];
    if(!fs_format_path(path, sizeof path, "%s/%s", root, file))
        return false;
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    *slash = '\0';
    return fs_make_dirs(parent) && fs_write_file(path, text);
}

static bool library_at(const char *root, const char *extra) {
    char text[4096];
    snprintf(text, sizeof text,
             "[package]\nname = \"greet\"\nversion = \"1.0.0\"\nartifact = \"static\"\n"
             "license = \"MIT\"\nrepository = \"https://example.com/greet\"\n"
             "[target]\nstd = \"c17\"\ninclude = [\"include\", \"private\"]\n"
             "defines = [\"PRIVATE_ONLY\"]\n"
             "[interface]\ndefines = [\"PUBLIC_ABI\"]\n%s",
             extra);
    return write_at(root, "Project.toml", text) && write_at(root, "LICENSE", "MIT\n") &&
           write_at(root, "include/greet.h", "int greet(void);\n") &&
           write_at(root, "private/detail.h", "#define GREET_VALUE 7\n") &&
           write_at(root, "src/greet.c",
                    "#include <greet.h>\n#include <detail.h>\n"
                    "#if !defined(PRIVATE_ONLY) || !defined(PUBLIC_ABI)\n#error missing "
                    "options\n#endif\n"
                    "int greet(void) { return GREET_VALUE; }\n");
}

DESCRIBE(a_manifest_derives_public_and_private_dependency_options) {
    char root[MOLTEST_PATH], err[2048] = "";
    ASSERT_TRUE(moltest_temp_dir("molto_package", root, sizeof root));
    ASSERT_TRUE(library_at(root, ""));
    project_ctx *ctx = calloc(1, sizeof *ctx);
    recipe_artifacts *artifacts = calloc(1, sizeof *artifacts);
    ASSERT_TRUE(package_read(root, "greet", ctx, artifacts, err, sizeof err));
    EXPECT_EQ(recipe_artifact_source, artifacts->type);
    EXPECT_EQ(1u, artifacts->source_count);
    EXPECT_STREQ("src/greet.c", artifacts->sources[0]);
    EXPECT_STREQ("include", artifacts->options.include[0]);
    EXPECT_STREQ("PUBLIC_ABI", artifacts->options.defines[0]);
    EXPECT_STREQ("private", artifacts->private_options.include[0]);
    EXPECT_STREQ("PRIVATE_ONLY", artifacts->private_options.defines[0]);
    EXPECT_STREQ("c17", artifacts->std);
    free(ctx);
    free(artifacts);
    EXPECT_TRUE(fs_remove_tree(root));
}

DESCRIBE(interface_typos_and_repeating_include_are_manifest_errors) {
    project_ctx *ctx = calloc(1, sizeof *ctx);
    char err[512] = "";
    EXPECT_FALSE(project_parse("[package]\nname = \"greet\"\n[interface]\ndefiens = []\n", ctx, err,
                               sizeof err));
    EXPECT_NOT_NULL(strstr(err, "defiens"));
    EXPECT_FALSE(
        project_parse("[package]\nname = \"greet\"\n[interface]\ninclude = [\"./include/\"]\n", ctx,
                      err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "convention"));
    free(ctx);
}

DESCRIBE(a_dependency_ignores_its_development_tables) {
    char root[MOLTEST_PATH], err[2048] = "";
    ASSERT_TRUE(moltest_temp_dir("molto_package", root, sizeof root));
    ASSERT_TRUE(library_at(
        root, "[dev-deps]\nmissing = { nonsense = \"ignored\" }\n"
              "[test]\nmode = \"not_a_mode\"\n[env]\nBAD = 42\n"
              "[profile.debug]\nopt_level = \"ignored\"\n[plugins]\nmalicious = \"ignored\"\n"));
    project_ctx *ctx = calloc(1, sizeof *ctx);
    recipe_artifacts *artifacts = calloc(1, sizeof *artifacts);
    ASSERT_TRUE(package_read(root, "greet", ctx, artifacts, err, sizeof err));
    EXPECT_EQ(0u, ctx->dev_deps.count);
    EXPECT_EQ(0u, ctx->env.count);
    free(ctx);
    free(artifacts);
    EXPECT_TRUE(fs_remove_tree(root));
}

DESCRIBE(package_validation_lists_all_required_failures) {
    char root[MOLTEST_PATH], err[2048] = "";
    ASSERT_TRUE(moltest_temp_dir("molto_package", root, sizeof root));
    ASSERT_TRUE(write_at(root, "Project.toml",
                         "[package]\nname = \"greet\"\n[interface]\nentry = \"src/main.c\"\n"
                         "[target]\ninclude = [\"absent\"]\n"));
    project_ctx *ctx = calloc(1, sizeof *ctx);
    recipe_artifacts *artifacts = calloc(1, sizeof *artifacts);
    EXPECT_FALSE(package_read(root, "wrong_name", ctx, artifacts, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "dependency name"));
    EXPECT_NOT_NULL(strstr(err, "version"));
    EXPECT_NOT_NULL(strstr(err, "static or shared"));
    EXPECT_NOT_NULL(strstr(err, "absent"));
    EXPECT_NOT_NULL(strstr(err, "src/ needs"));
    EXPECT_NOT_NULL(strstr(err, "molto package"));
    free(ctx);
    free(artifacts);
    EXPECT_TRUE(fs_remove_tree(root));
}

DESCRIBE(two_carried_descriptions_are_an_error) {
    char root[MOLTEST_PATH], err[2048] = "";
    ASSERT_TRUE(moltest_temp_dir("molto_package", root, sizeof root));
    ASSERT_TRUE(library_at(root, ""));
    ASSERT_TRUE(write_at(root, "recipe.toml", "[artifacts]\ntype = \"source\"\n"));
    project_ctx *ctx = calloc(1, sizeof *ctx);
    recipe_artifacts *artifacts = calloc(1, sizeof *artifacts);
    EXPECT_FALSE(package_read(root, "greet", ctx, artifacts, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "both recipe.toml and Project.toml"));
    free(ctx);
    free(artifacts);
    EXPECT_TRUE(fs_remove_tree(root));
}

DESCRIBE(pruning_keeps_sources_headers_licenses_and_extra_globs) {
    char root[MOLTEST_PATH], err[2048] = "";
    ASSERT_TRUE(moltest_temp_dir("molto_package", root, sizeof root));
    ASSERT_TRUE(library_at(root, "[package]\nfiles = [\"third_party/**\", \"data/table.inc\"]\n"));
    ASSERT_TRUE(write_at(root, "src/main.c", "int main(void) { return 0; }\n"));
    ASSERT_TRUE(write_at(root, "NOTICE", "notice\n"));
    ASSERT_TRUE(write_at(root, "COPYING.txt", "copying\n"));
    ASSERT_TRUE(write_at(root, "third_party/nested/x.h", "header\n"));
    ASSERT_TRUE(write_at(root, "data/table.inc", "0\n"));
    ASSERT_TRUE(write_at(root, "tests/test.c", "unwanted\n"));
    ASSERT_TRUE(write_at(root, "docs/readme.md", "unwanted\n"));
    ASSERT_TRUE(write_at(root, ".git/config", "unwanted\n"));
    ASSERT_TRUE(package_prune(root, err, sizeof err));
    const char *const kept[] = {
        "Project.toml", "src/greet.c", "include/greet.h",        "private/detail.h", "NOTICE",
        "COPYING.txt",  "LICENSE",     "third_party/nested/x.h", "data/table.inc"};
    const char *const gone[] = {"src/main.c", "tests", "docs", ".git"};
    char path[4096];
    for(size_t i = 0; i < sizeof kept / sizeof kept[0]; i++) {
        ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/%s", root, kept[i]));
        EXPECT_TRUE(fs_path_exists(path));
    }
    for(size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) {
        ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/%s", root, gone[i]));
        EXPECT_FALSE(fs_path_exists(path));
    }
    EXPECT_TRUE(fs_remove_tree(root));
}

DESCRIBE(package_extra_files_fail_on_typos_and_escaping_paths) {
    project_ctx *ctx = calloc(1, sizeof *ctx);
    str_list files, kept;
    str_list_init(&files);
    str_list_init(&kept);
    ASSERT_TRUE(str_list_push(&files, "src/a.c"));
    ctx->file_count = 1;
    snprintf(ctx->files[0], PROJECT_OPT_LEN, "data/missing*");
    char err[512] = "";
    EXPECT_FALSE(package_select(ctx, &files, &kept, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "matches nothing"));
    snprintf(ctx->files[0], PROJECT_OPT_LEN, "../secret");
    EXPECT_FALSE(package_select(ctx, &files, &kept, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "inside the package"));
    str_list_free(&files);
    str_list_free(&kept);
    free(ctx);
}

static bool consumer_at(const char *root, const char *library, const char *source,
                        const char *extra) {
    char manifest[4096];
    snprintf(manifest, sizeof manifest,
             "[package]\nname = \"consumer\"\nversion = \"1.0.0\"\n"
             "[deps]\ngreet = { path = \"%s\" }\n%s",
             library, extra);
    return write_at(root, "Project.toml", manifest) && write_at(root, "src/consumer.c", source);
}

DESCRIBE(a_path_package_builds_with_private_options_and_is_never_pruned) {
    char lib[MOLTEST_PATH], app[MOLTEST_PATH], path[4096];
    ASSERT_TRUE(moltest_temp_dir("molto_package", lib, sizeof lib));
    ASSERT_TRUE(moltest_temp_dir("molto_consumer", app, sizeof app));
    ASSERT_TRUE(library_at(lib, ""));
    ASSERT_TRUE(write_at(lib, "src/main.c", "#error dependency main must never compile\n"));
    ASSERT_TRUE(write_at(lib, "tests/ignored.c", "#error dependency tests must never compile\n"));
    ASSERT_TRUE(consumer_at(
        app, lib,
        "#include <greet.h>\n#ifdef PRIVATE_ONLY\n#error private define leaked\n#endif\n"
        "#ifndef PUBLIC_ABI\n#error public define missing\n#endif\n"
        "int main(void) { return greet() == 7 ? 0 : 1; }\n",
        ""));
    ASSERT_EQ(exit_ok, build_project(app, profile_debug, NULL, false, 1, path, sizeof path));
    const char *run[] = {path, NULL};
    EXPECT_EQ(0, process_run(run));
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/tests/ignored.c", lib));
    EXPECT_TRUE(fs_path_exists(path));
    EXPECT_TRUE(fs_remove_tree(app));
    EXPECT_TRUE(fs_remove_tree(lib));
}

DESCRIBE(an_entry_archive_supplies_main_only_when_the_consumer_needs_it) {
    char lib[MOLTEST_PATH], app[MOLTEST_PATH], binary[4096], archive[4096];
    ASSERT_TRUE(moltest_temp_dir("molto_package", lib, sizeof lib));
    ASSERT_TRUE(moltest_temp_dir("molto_consumer", app, sizeof app));
    ASSERT_TRUE(library_at(lib, "entry = \"src/entry.c\"\n"));
    ASSERT_TRUE(write_at(lib, "src/entry.c", "int main(void) { return 19; }\n"));
    ASSERT_TRUE(consumer_at(app, lib, "int own_source(void) { return 0; }\n", ""));
    ASSERT_EQ(exit_ok, build_project(app, profile_debug, NULL, false, 1, binary, sizeof binary));
    const char *run[] = {binary, NULL};
    EXPECT_EQ(19, process_run(run));
    ASSERT_TRUE(fs_format_path(archive, sizeof archive, "%s/build/debug/greet.entry.a", app));
    EXPECT_TRUE(fs_path_exists(archive));
    ASSERT_TRUE(write_at(app, "src/consumer.c", "int main(void) { return 23; }\n"));
    ASSERT_EQ(exit_ok, build_project(app, profile_debug, NULL, false, 1, binary, sizeof binary));
    EXPECT_EQ(23, process_run(run));
    ASSERT_TRUE(write_at(app, "src/consumer.c", "int own_source(void) { return 0; }\n"));
    str_list binaries;
    str_list_init(&binaries);
    ASSERT_TRUE(write_at(app, "tests/test.c", "int main(void) { return 0; }\n"));
    EXPECT_EQ(exit_ok, build_tests(app, profile_debug, NULL, false, 1, &binaries, NULL));
    str_list_free(&binaries);
    EXPECT_TRUE(fs_remove_tree(app));
    EXPECT_TRUE(fs_remove_tree(lib));
}

DESCRIBE(the_package_command_assembles_only_tracked_files_and_builds_the_copy) {
    char root[MOLTEST_PATH], original[4096], path[4096], err[2048] = "";
    ASSERT_TRUE(moltest_temp_dir("molto_package", root, sizeof root));
    ASSERT_TRUE(fs_current_dir(original, sizeof original));
    ASSERT_TRUE(library_at(root, ""));
    ASSERT_TRUE(write_at(root, ".gitignore", "build/\ninclude/ignored.h\n"));
    ASSERT_TRUE(write_at(root, "include/ignored.h", "#define IGNORED_HEADER 1\n"));
    ASSERT_TRUE(write_at(root, "README.md", "not needed by consumers\n"));
    const char *init[] = {"git", "-C", root, "init", "--quiet", NULL};
    const char *add[] = {"git", "-C", root, "add", ".", NULL};
    ASSERT_EQ(0, process_capture_all(init, NULL, 0, err, sizeof err, NULL));
    ASSERT_EQ(0, process_capture_all(add, NULL, 0, err, sizeof err, NULL));
    ASSERT_EQ(0, chdir(root));
    int listed = package_command_run(true);
    int built = package_command_run(false);
    EXPECT_EQ(0, chdir(original));
    EXPECT_EQ(exit_ok, listed);
    EXPECT_EQ(exit_ok, built);
    ASSERT_TRUE(
        fs_format_path(path, sizeof path, "%s/build/package/greet-1.0.0/include/greet.h", root));
    EXPECT_TRUE(fs_path_exists(path));
    ASSERT_TRUE(
        fs_format_path(path, sizeof path, "%s/build/package/greet-1.0.0/include/ignored.h", root));
    EXPECT_FALSE(fs_path_exists(path));
    ASSERT_TRUE(
        write_at(root, "src/greet.c", "#include <ignored.h>\nint greet(void) { return 0; }\n"));
    ASSERT_EQ(0, chdir(root));
    int missing = package_command_run(false);
    EXPECT_EQ(0, chdir(original));
    EXPECT_NE(exit_ok, missing);
    EXPECT_TRUE(fs_remove_tree(root));
}

DESCRIBE(two_dependency_entries_are_refused_with_both_names) {
    char lib[MOLTEST_PATH], other[MOLTEST_PATH], app[MOLTEST_PATH], manifest[4096];
    ASSERT_TRUE(moltest_temp_dir("molto_package", lib, sizeof lib));
    ASSERT_TRUE(moltest_temp_dir("molto_package", other, sizeof other));
    ASSERT_TRUE(moltest_temp_dir("molto_consumer", app, sizeof app));
    ASSERT_TRUE(library_at(lib, "entry = \"src/entry.c\"\n"));
    ASSERT_TRUE(write_at(lib, "src/entry.c", "int main(void) { return 0; }\n"));
    ASSERT_TRUE(
        write_at(other, "Project.toml",
                 "[package]\nname = \"second\"\nversion = \"1.0.0\"\nartifact = \"static\"\n"
                 "[interface]\nentry = \"src/entry.c\"\n"));
    ASSERT_TRUE(write_at(other, "src/entry.c", "int main(void) { return 0; }\n"));
    snprintf(manifest, sizeof manifest, "second = { path = \"%s\" }\n", other);
    ASSERT_TRUE(consumer_at(app, lib, "int main(void) { return 0; }\n", manifest));
    EXPECT_EQ(exit_build_failure, build_project(app, profile_debug, NULL, false, 1, NULL, 0));
    EXPECT_TRUE(fs_remove_tree(app));
    EXPECT_TRUE(fs_remove_tree(lib));
    EXPECT_TRUE(fs_remove_tree(other));
}

DESCRIBE(a_fetched_manifest_is_pruned_before_install_and_old_stamps_are_refetched) {
    char root[MOLTEST_PATH], package[4096], archive[4096], cache[4096], out[4096], err[2048] = "";
    ASSERT_TRUE(moltest_temp_dir("molto_package", root, sizeof root));
    ASSERT_TRUE(fs_format_path(package, sizeof package, "%s/package", root));
    ASSERT_TRUE(library_at(package, ""));
    ASSERT_TRUE(write_at(package, "tests/unwanted.c", "unwanted\n"));
    ASSERT_TRUE(write_at(package, "src/main.c", "unwanted\n"));
    ASSERT_TRUE(fs_format_path(archive, sizeof archive, "%s/package.tar.gz", root));
    const char *tar[] = {"tar", "-czf", archive, "-C", package, ".", NULL};
    ASSERT_EQ(0, process_capture_all(tar, NULL, 0, err, sizeof err, NULL));
    source_spec spec = {.origin = source_origin_archive};
    snprintf(spec.location, sizeof spec.location, "file://%s", archive);
    ASSERT_TRUE(sha256_file(archive, spec.sha256));
    ASSERT_TRUE(fs_format_path(cache, sizeof cache, "%s/cache", root));
    const char *prior = getenv("MOLTO_CACHE");
    char *restore = prior ? strdup(prior) : NULL;
    ASSERT_EQ(0, setenv("MOLTO_CACHE", cache, 1));
    bool fetched =
        source_fetch_carried(&spec, "greet", "1.0.0", "any", out, sizeof out, err, sizeof err);
    char path[4096];
    EXPECT_TRUE(fetched);
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/tests", out));
    EXPECT_FALSE(fs_path_exists(path));
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/.molto-fetched", out));
    char *stamp = fs_read_file(path);
    EXPECT_STREQ(PACKAGE_PRUNE_STAMP, stamp);
    free(stamp);
    EXPECT_TRUE(fs_write_file(path, "ok\n"));
    EXPECT_TRUE(write_at(out, "docs/stale", "old cached tree\n"));
    bool refetched =
        source_fetch_carried(&spec, "greet", "1.0.0", "any", out, sizeof out, err, sizeof err);
    EXPECT_TRUE(refetched);
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/docs", out));
    EXPECT_FALSE(fs_path_exists(path));
    /* A registry recipe directing the same fetch is entitled to the whole tree. */
    bool recipe =
        source_fetch(&spec, "recipe_directed", "1.0.0", "any", out, sizeof out, err, sizeof err);
    EXPECT_TRUE(recipe);
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/tests/unwanted.c", out));
    EXPECT_TRUE(fs_path_exists(path));
    if(restore)
        EXPECT_EQ(0, setenv("MOLTO_CACHE", restore, 1));
    else
        EXPECT_EQ(0, unsetenv("MOLTO_CACHE"));
    free(restore);
    EXPECT_TRUE(fs_remove_tree(root));
}

DESCRIBE(git_fetches_a_single_commit_and_manifest_packages_keep_no_git_history) {
    char root[MOLTEST_PATH], package[4096], cache[4096], out[4096], err[2048] = "", head[128] = "";
    ASSERT_TRUE(moltest_temp_dir("molto_package", root, sizeof root));
    ASSERT_TRUE(fs_format_path(package, sizeof package, "%s/package", root));
    ASSERT_TRUE(library_at(package, ""));
    const char *init[] = {"git", "-C", package, "init", "--quiet", NULL};
    const char *add[] = {"git", "-C", package, "add", ".", NULL};
    const char *commit[] = {"git",
                            "-C",
                            package,
                            "-c",
                            "user.name=Test Author",
                            "-c",
                            "user.email=test@example.com",
                            "-c",
                            "commit.gpgsign=false",
                            "commit",
                            "--quiet",
                            "-m",
                            "test: create package fixture",
                            NULL};
    ASSERT_EQ(0, process_capture_all(init, NULL, 0, err, sizeof err, NULL));
    ASSERT_EQ(0, process_capture_all(add, NULL, 0, err, sizeof err, NULL));
    ASSERT_EQ(0, process_capture_all(commit, NULL, 0, err, sizeof err, NULL));
    ASSERT_TRUE(write_at(package, "README.md", "a second commit\n"));
    ASSERT_EQ(0, process_capture_all(add, NULL, 0, err, sizeof err, NULL));
    ASSERT_EQ(0, process_capture_all(commit, NULL, 0, err, sizeof err, NULL));
    const char *revision[] = {"git", "-C", package, "rev-parse", "HEAD", NULL};
    ASSERT_EQ(0, process_capture(revision, head, sizeof head));
    head[strcspn(head, "\r\n")] = '\0';
    source_spec spec = {.origin = source_origin_git};
    snprintf(spec.location, sizeof spec.location, "%s", package);
    snprintf(spec.reference, sizeof spec.reference, "%s", head);
    ASSERT_TRUE(fs_format_path(cache, sizeof cache, "%s/cache", root));
    const char *prior = getenv("MOLTO_CACHE");
    char *restore = prior ? strdup(prior) : NULL;
    ASSERT_EQ(0, setenv("MOLTO_CACHE", cache, 1));
    bool fetched = source_fetch(&spec, "raw", head, "any", out, sizeof out, err, sizeof err);
    EXPECT_TRUE(fetched);
    const char *count[] = {"git", "-C", out, "rev-list", "--count", "HEAD", NULL};
    EXPECT_EQ(0, process_capture(count, err, sizeof err));
    EXPECT_STREQ("1\n", err);
    bool carried =
        source_fetch_carried(&spec, "greet", head, "any", out, sizeof out, err, sizeof err);
    EXPECT_TRUE(carried);
    char path[4096];
    EXPECT_TRUE(fs_format_path(path, sizeof path, "%s/.git", out));
    EXPECT_FALSE(fs_path_exists(path));
    EXPECT_TRUE(fs_format_path(path, sizeof path, "%s/README.md", out));
    EXPECT_FALSE(fs_path_exists(path));
    if(restore)
        EXPECT_EQ(0, setenv("MOLTO_CACHE", restore, 1));
    else
        EXPECT_EQ(0, unsetenv("MOLTO_CACHE"));
    free(restore);
    EXPECT_TRUE(fs_remove_tree(root));
}

DESCRIBE(a_duplicate_dependency_main_diagnostic_points_to_interface_entry) {
    char lib[MOLTEST_PATH], app[MOLTEST_PATH], output[16384] = "";
    ASSERT_TRUE(moltest_temp_dir("molto_package", lib, sizeof lib));
    ASSERT_TRUE(moltest_temp_dir("molto_consumer", app, sizeof app));
    ASSERT_TRUE(library_at(lib, ""));
    ASSERT_TRUE(write_at(lib, "src/unmarked.c", "int main(void) { return 0; }\n"));
    ASSERT_TRUE(consumer_at(app, lib, "int main(void) { return 0; }\n", ""));
    FILE *said = tmpfile();
    ASSERT_NOT_NULL(said);
    build_report *report = build_report_create(said);
    EXPECT_EQ(exit_build_failure,
              build_project_with(app, profile_debug, NULL, false, 1, NULL, 0, NULL, report));
    build_report_destroy(report);
    rewind(said);
    (void)fread(output, 1, sizeof output - 1, said);
    fclose(said);
    EXPECT_NOT_NULL(strstr(output, "dependency 'greet'"));
    EXPECT_NOT_NULL(strstr(output, "[interface].entry"));
    EXPECT_TRUE(fs_remove_tree(app));
    EXPECT_TRUE(fs_remove_tree(lib));
}
