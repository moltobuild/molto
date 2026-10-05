#include <moltest.h>

#include <molto/commands/add_command.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/source_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* `molto add git+<url>[#<ref>]`: the spelling Molto.lock uses for a git
 * source, accepted on the command line. Parsing is pure and checked without a
 * network; telling a branch from a tag is checked against a local repository,
 * which git treats exactly as it treats a remote one. */

typedef struct {
    char name[128];
    char url[512];
    char ref[128];
    char err[256];
} parsed;

static bool parse(const char *spec, parsed *out) {
    memset(out, 0, sizeof *out);
    return add_git_spec_parse(spec, out->name, sizeof out->name, out->url, sizeof out->url,
                              out->ref, sizeof out->ref, out->err, sizeof out->err);
}

DESCRIBE(git_spec_names_the_package_after_the_repository) {
    parsed at;
    ASSERT_TRUE(parse("git+https://github.com/moltobuild/moltest", &at));
    EXPECT_STREQ("moltest", at.name);
    EXPECT_STREQ("https://github.com/moltobuild/moltest", at.url);
    EXPECT_STREQ("", at.ref);
}

DESCRIBE(git_spec_drops_a_trailing_slash_and_dot_git) {
    parsed at;
    ASSERT_TRUE(parse("git+https://example.com/org/zlib.git/", &at));
    EXPECT_STREQ("zlib", at.name);
    EXPECT_STREQ("https://example.com/org/zlib.git/", at.url);
}

DESCRIBE(git_spec_takes_a_reference_after_the_hash) {
    parsed at;
    ASSERT_TRUE(parse("git+https://github.com/moltobuild/moltest#v0.1.0", &at));
    EXPECT_STREQ("https://github.com/moltobuild/moltest", at.url);
    EXPECT_STREQ("v0.1.0", at.ref);
}

/* The `@` belongs to the URL: it is the user of an ssh remote, not a version. */
DESCRIBE(git_spec_keeps_an_at_sign_in_the_url) {
    parsed at;
    ASSERT_TRUE(parse("git+ssh://git@github.com/moltobuild/moltest.git#master", &at));
    EXPECT_STREQ("moltest", at.name);
    EXPECT_STREQ("ssh://git@github.com/moltobuild/moltest.git", at.url);
    EXPECT_STREQ("master", at.ref);

    ASSERT_TRUE(parse("git+git@github.com:moltobuild/moltest.git", &at));
    EXPECT_STREQ("moltest", at.name);
}

DESCRIBE(git_spec_refuses_what_a_manifest_cannot_hold) {
    parsed at;
    EXPECT_FALSE(parse("https://github.com/moltobuild/moltest", &at)); /* no prefix */
    EXPECT_FALSE(parse("git+", &at));
    EXPECT_FALSE(parse("git+https://example.com/a#", &at));             /* empty ref */
    EXPECT_FALSE(parse("git+https://example.com/a\"b", &at));           /* ends a string */
    EXPECT_FALSE(parse("git+https://example.com/a b", &at));
    EXPECT_FALSE(parse("git+--upload-pack=x/repo", &at));               /* an option */
    EXPECT_FALSE(parse("git+https://example.com/repo#--force", &at));
}

DESCRIBE(git_spec_refuses_a_repository_that_is_not_a_package_name) {
    parsed at;
    EXPECT_FALSE(parse("git+https://github.com/moltobuild/moltest-coverage", &at));
    /* And says how to name it instead. */
    EXPECT_NOT_NULL(strstr(at.err, "--git"));
}

/* A repository on `main` with one commit, a tag `v1` and a branch `next`.
   False when this machine has no usable git. */
static bool make_repo(const char *root, char *repo, size_t size, char *head, size_t head_size) {
    if (!fs_format_path(repo, size, "%s/repo", root))
        return false;
    char script[2048];
    if (!fs_format_path(script, sizeof script,
                        "mkdir -p '%s' && cd '%s' && git init -q -b main . && "
                        "git config user.email t@t && git config user.name t && "
                        "echo x > f && git add f && git -c commit.gpgsign=false commit -qm one && "
                        "git tag v1 && git branch next && git rev-parse HEAD",
                        repo, repo))
        return false;
    const char *argv[] = { "sh", "-c", script, NULL };
    if (process_capture(argv, head, head_size) != 0)
        return false;
    head[strcspn(head, " \t\r\n")] = '\0';
    return true;
}

DESCRIBE(git_reference_key_tells_branch_tag_and_commit_apart) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_addgit", root, sizeof root));
    char repo[MOLTEST_PATH + 16];
    char head[128] = "";
    if (!make_repo(root, repo, sizeof repo, head, sizeof head)) {
        (void)fs_remove_tree(root);
        SKIP("this machine has no usable git");
    }

    const char *key = NULL;
    char ref[128];
    char err[256] = "";

    /* No reference: the default branch, asked of the repository. */
    ASSERT_TRUE(source_git_reference_key(repo, NULL, &key, ref, sizeof ref, err, sizeof err));
    EXPECT_STREQ("branch", key);
    EXPECT_STREQ("main", ref);

    ASSERT_TRUE(source_git_reference_key(repo, "v1", &key, ref, sizeof ref, err, sizeof err));
    EXPECT_STREQ("tag", key);

    ASSERT_TRUE(source_git_reference_key(repo, "next", &key, ref, sizeof ref, err, sizeof err));
    EXPECT_STREQ("branch", key);

    ASSERT_TRUE(source_git_reference_key(repo, head, &key, ref, sizeof ref, err, sizeof err));
    EXPECT_STREQ("rev", key);
    EXPECT_STREQ(head, ref);

    EXPECT_FALSE(source_git_reference_key(repo, "nope", &key, ref, sizeof ref, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "nope"));

    (void)fs_remove_tree(root);
}

DESCRIBE(add_with_git_and_no_reference_writes_the_default_branch) {
    /* `molto add dep --git <url>` used to write `{ git = "<url>" }` and stop:
       a manifest no build accepts, because a git source caches under a commit
       and needs a branch, tag or rev. `git+<url>` already asked the repository
       for its default branch; this spelling now does the same. */
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_addgit_noref", root, sizeof root));
    char repo[MOLTEST_PATH + 16];
    char head[128] = "";
    if (!make_repo(root, repo, sizeof repo, head, sizeof head)) {
        (void)fs_remove_tree(root);
        SKIP("this machine has no usable git");
    }
    char manifest[MOLTEST_PATH + 32];
    ASSERT_TRUE(fs_format_path(manifest, sizeof manifest, "%s/Project.toml", root));
    ASSERT_TRUE(fs_write_file(manifest, "[package]\nname = \"app\"\n"));

    char previous[4096];
    ASSERT_TRUE(getcwd(previous, sizeof previous) != NULL);
    ASSERT_TRUE(chdir(root) == 0);
    const int code = add_command_run("dep", NULL, "git", repo, NULL, true);
    EXPECT_TRUE(chdir(previous) == 0);

    EXPECT_EQ(0, code);
    char *text = fs_read_file(manifest);
    ASSERT_NOT_NULL(text);
    char expected[MOLTEST_PATH + 96];
    snprintf(expected, sizeof expected, "dep = { git = \"%s\", branch = \"main\" }", repo);
    EXPECT_NOT_NULL(strstr(text, expected));
    free(text);

    (void)fs_remove_tree(root);
}
