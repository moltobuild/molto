#include <moltest.h>
#include <moltest_mock.h>

#include <molto/services/manifest_service.h>
#include <molto/services/process_service.h>
#include <molto/services/scaffold_service.h>
#include <molto/services/source_service.h>

#include <stdio.h>
#include <string.h>

/*
 * What `source_git_reference_key` makes of git's answers, with git faked.
 *
 * An isolated test (RFC-0021): it replaces src/services/process_service.c, so
 * process_capture() below is the one this binary has. The real repository in
 * test_add_git.c covers the answers a working git gives; these are the ones it
 * cannot be made to give on demand — not installed, unreachable, a listing
 * that names nothing.
 */

MOCK_VALUE_FUNC(int, process_capture, const char *const *, char *, size_t);
/* Never called here. source_service.c clones with it, and the linker takes an
   object whole, so whatever any function of it calls needs a definition. */
MOCK_VALUE_FUNC(int, process_run, const char *const *);

/* What the fake prints, and the command it was run with. argv lives in the
   caller's frame, so it is copied during the call rather than read after. */
static const char *answer;
static char ran[256];

static int git_answers(const char *const *argv, char *out, size_t out_size) {
    ran[0] = '\0';
    for(size_t i = 0; argv[i] != NULL; i++) {
        const size_t used = strlen(ran);
        snprintf(ran + used, sizeof ran - used, "%s%s", i > 0 ? " " : "", argv[i]);
    }
    snprintf(out, out_size, "%s", answer != NULL ? answer : "");
    return 0;
}

static const char *key;
static char ref[128];
static char err[256];

static bool ask(const char *reference) {
    key = NULL;
    ref[0] = '\0';
    err[0] = '\0';
    return source_git_reference_key("https://example.com/repo", reference, &key, ref, sizeof ref,
                                    err, sizeof err);
}

DESCRIBE(a_missing_git_says_git_is_not_installed) {
    process_capture_mock.return_val = 127;
    EXPECT_FALSE(ask("v1"));
    EXPECT_NOT_NULL(strstr(err, "git is not installed"));
    EXPECT_FALSE(ask(NULL));
    EXPECT_NOT_NULL(strstr(err, "git is not installed"));
}

DESCRIBE(a_repository_git_cannot_reach_is_named) {
    process_capture_mock.return_val = 128;
    EXPECT_FALSE(ask("v1"));
    EXPECT_NOT_NULL(strstr(err, "could not reach the repository"));
    EXPECT_NOT_NULL(strstr(err, "https://example.com/repo"));
}

DESCRIBE(a_name_that_is_both_a_tag_and_a_branch_is_the_tag) {
    answer = "1111111111111111111111111111111111111111\trefs/heads/v1\n"
             "2222222222222222222222222222222222222222\trefs/tags/v1\n";
    process_capture_mock.custom_fake = git_answers;
    ASSERT_TRUE(ask("v1"));
    EXPECT_STREQ("tag", key);
    EXPECT_STREQ("v1", ref);
    EXPECT_STREQ("git ls-remote https://example.com/repo refs/tags/v1 refs/heads/v1", ran);
}

DESCRIBE(a_reference_the_listing_does_not_name_is_refused) {
    answer = "";
    process_capture_mock.custom_fake = git_answers;
    EXPECT_FALSE(ask("nope"));
    EXPECT_NOT_NULL(strstr(err, "no such branch or tag"));
}

DESCRIBE(a_commit_id_asks_git_nothing) {
    ASSERT_TRUE(ask("0123456789abcdef0123456789abcdef01234567"));
    EXPECT_STREQ("rev", key);
    EXPECT_EQ(0, (int)process_capture_mock.call_count);
}

DESCRIBE(the_default_branch_is_read_from_the_symref) {
    answer = "ref: refs/heads/trunk\tHEAD\n"
             "3333333333333333333333333333333333333333\tHEAD\n";
    process_capture_mock.custom_fake = git_answers;
    ASSERT_TRUE(ask(NULL));
    EXPECT_STREQ("branch", key);
    EXPECT_STREQ("trunk", ref);
    EXPECT_STREQ("git ls-remote --symref https://example.com/repo HEAD", ran);
}

DESCRIBE(a_repository_that_names_no_default_branch_asks_for_a_ref) {
    answer = "3333333333333333333333333333333333333333\tHEAD\n";
    process_capture_mock.custom_fake = git_answers;
    EXPECT_FALSE(ask(NULL));
    EXPECT_NOT_NULL(strstr(err, "add #<ref>"));
}

DESCRIBE(the_newest_release_is_asked_of_the_repositorys_tags) {
    answer = "1111111111111111111111111111111111111111\trefs/tags/v0.2.0\n"
             "2222222222222222222222222222222222222222\trefs/tags/v0.3.0\n";
    process_capture_mock.custom_fake = git_answers;
    char tag[64] = "";
    ASSERT_TRUE(source_git_newest_release("https://example.com/repo", tag, sizeof tag, err,
                                          sizeof err));
    EXPECT_STREQ("v0.3.0", tag);
    EXPECT_STREQ("git ls-remote --tags --refs https://example.com/repo", ran);
}

DESCRIBE(a_repository_without_releases_says_so) {
    answer = "";
    process_capture_mock.custom_fake = git_answers;
    char tag[64] = "";
    EXPECT_FALSE(source_git_newest_release("https://example.com/repo", tag, sizeof tag, err,
                                           sizeof err));
    EXPECT_NOT_NULL(strstr(err, "no release"));
}

DESCRIBE(a_new_library_pins_the_newest_moltest) {
    answer = "1111111111111111111111111111111111111111\trefs/tags/v0.4.2\n";
    process_capture_mock.custom_fake = git_answers;
    char tag[64] = "";
    scaffold_newest_moltest_tag(tag, sizeof tag);
    EXPECT_STREQ("v0.4.2", tag);
}

DESCRIBE(offline_a_new_library_pins_the_release_molto_knows) {
    /* Never a branch: without an answer, the newest release this molto was
       built knowing of. */
    process_capture_mock.return_val = 128;
    char tag[64] = "";
    scaffold_newest_moltest_tag(tag, sizeof tag);
    EXPECT_STREQ(MANIFEST_MOLTEST_KNOWN_TAG, tag);
}
