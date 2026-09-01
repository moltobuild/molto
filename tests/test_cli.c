#include <moltest.h>

#include <molto/cli.h>
#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/util/cli.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* What the test command handler saw on the last run. */
static struct {
    bool called;
    const char *profile;
    bool verbose;
    const char *positional;
    int forwarded_count;
    const char *forwarded0;
} seen;

static int capture_handler(const cli_args *args) {
    seen.called = true;
    seen.profile = cli_args_option(args, "--profile");
    seen.verbose = cli_args_flag(args, "--verbose");
    seen.positional = cli_args_positional(args, 0);
    char *const *forwarded = cli_args_forwarded(args, &seen.forwarded_count);
    seen.forwarded0 = seen.forwarded_count > 0 ? forwarded[0] : NULL;
    return 0;
}

static const cli_option test_options[] = {
    { "--profile", 'p', cli_opt_value, "<name>", "profile", "debug" },
    { "--verbose", 'v', cli_opt_flag, NULL, "verbose", NULL },
};

static int run_app(int argc, char **argv) {
    const cli_command commands[] = {
        { "do", "a test command", "<name>", test_options,
          sizeof test_options / sizeof test_options[0], capture_handler },
    };
    const cli_app app = { "testapp", "9.9.9", "tagline", commands, 1, NULL };
    memset(&seen, 0, sizeof seen);
    return cli_app_run(&app, argc, argv);
}

MOLTEST(cli) {
    EXPECT_TRUE(strlen(cli_version()) > 0);

    /* Value option "--opt value" plus a positional. */
    char *a1[] = { "testapp", "do", "--profile", "release", "x" };
    EXPECT_TRUE(run_app(5, a1) == 0);
    EXPECT_TRUE(seen.called);
    EXPECT_TRUE(seen.profile != NULL && strcmp(seen.profile, "release") == 0);
    EXPECT_TRUE(seen.positional != NULL && strcmp(seen.positional, "x") == 0);

    /* "--opt=value" form. */
    char *a2[] = { "testapp", "do", "--profile=bench" };
    EXPECT_TRUE(run_app(3, a2) == 0 && strcmp(seen.profile, "bench") == 0);

    /* Short "-p value" and boolean flag "-v". */
    char *a3[] = { "testapp", "do", "-p", "custom", "-v" };
    EXPECT_TRUE(run_app(5, a3) == 0);
    EXPECT_TRUE(strcmp(seen.profile, "custom") == 0);
    EXPECT_TRUE(seen.verbose == true);

    /* Short attached value "-pdebug". */
    char *a4[] = { "testapp", "do", "-pdebug" };
    EXPECT_TRUE(run_app(3, a4) == 0 && strcmp(seen.profile, "debug") == 0);

    /* Default reported when the option is absent. */
    char *a5[] = { "testapp", "do" };
    EXPECT_TRUE(run_app(2, a5) == 0);
    EXPECT_TRUE(seen.profile != NULL && strcmp(seen.profile, "debug") == 0);
    EXPECT_TRUE(seen.verbose == false);

    /* Forwarded arguments after "--". */
    char *a6[] = { "testapp", "do", "--", "alpha", "beta" };
    EXPECT_TRUE(run_app(5, a6) == 0);
    EXPECT_TRUE(seen.forwarded_count == 2);
    EXPECT_TRUE(seen.forwarded0 != NULL && strcmp(seen.forwarded0, "alpha") == 0);

    /* Unknown option -> usage error (4), handler not called. */
    char *a7[] = { "testapp", "do", "--nope" };
    EXPECT_TRUE(run_app(3, a7) == 4);
    EXPECT_TRUE(!seen.called);

    /* "<command> --help" -> ok (0), handler not called. */
    char *a8[] = { "testapp", "do", "--help" };
    EXPECT_TRUE(run_app(3, a8) == 0);
    EXPECT_TRUE(!seen.called);

    /* Unknown command -> usage error (4). */
    char *a9[] = { "testapp", "nope" };
    EXPECT_TRUE(run_app(2, a9) == 4);

    /* Missing value for a value option -> usage error (4). */
    char *a10[] = { "testapp", "do", "--profile" };
    EXPECT_TRUE(run_app(3, a10) == 4);
    EXPECT_TRUE(!seen.called);
}

/*
 * The version is written in two places and has to agree with itself.
 *
 * `Project.toml` is what the ecosystem reads; the `#define` in src/cli.c is what
 * `-V` reports. Nothing keeps them together but the discipline of editing both
 * in one commit, and the failure is quiet: a binary answering with the version
 * before last looks exactly like one that was never rebuilt, which is the single
 * question `-V` exists to settle.
 */
#define MANIFEST_PATH   "Project.toml"
#define PACKAGE_SECTION "[package]"
#define VERSION_KEY     "version = \""

static bool manifest_version(const char *text, char *out, size_t out_size) {
    const char *section = strstr(text, PACKAGE_SECTION);
    if(section == NULL)
        return false;

    const char *key = strstr(section, VERSION_KEY);
    if(key == NULL)
        return false;
    key += sizeof VERSION_KEY - 1;

    const char *end = strchr(key, '"');
    if(end == NULL)
        return false;

    size_t length = (size_t)(end - key);
    if(length == 0 || length >= out_size)
        return false;
    memcpy(out, key, length);
    out[length] = '\0';
    return true;
}

MOLTEST(cli_reports_the_version_the_manifest_declares) {
    char *text = fs_read_file(MANIFEST_PATH);
    if(text == NULL)
        SKIP("the manifest is only there when the suite runs from the repository root");

    char declared[64];
    bool read = manifest_version(text, declared, sizeof declared);
    free(text);
    ASSERT_TRUE(read);

    EXPECT_STREQ(declared, cli_version());
}

MOLTEST(cli_refuses_a_jobs_count_that_is_not_one) {
    /* Every one of these is rejected before the command looks for a workspace,
       so nothing is compiled here — which is also what makes the check worth
       having: a build told to take two workers and silently taking the machine
       would be found out much later, on a laptop with a fan. */
    char *not_a_number[] = { "molto", "build", "-j", "many" };
    EXPECT_EQ(exit_usage_error, cli_run(4, not_a_number));

    char *zero[] = { "molto", "build", "--jobs", "0" };
    EXPECT_EQ(exit_usage_error, cli_run(4, zero));

    char *negative[] = { "molto", "test", "-j", "-2" };
    EXPECT_EQ(exit_usage_error, cli_run(4, negative));

    char *trailing[] = { "molto", "lint", "-j", "4x" };
    EXPECT_EQ(exit_usage_error, cli_run(4, trailing));

    char *absurd[] = { "molto", "fmt", "-j", "4096" };
    EXPECT_EQ(exit_usage_error, cli_run(4, absurd));
}

/* --- the unknown-command hook (RFC-0014) --- */

/* What the fallback saw, and what it should answer. */
static struct {
    bool called;
    const char *name;
    int argc;
    const char *argv0;
    bool claim; /* what the handler returns */
    int code;   /* what it writes into *exit_code when it claims the name */
} fallback;

static bool capture_unknown(const char *name, int argc, char **argv, int *exit_code) {
    fallback.called = true;
    fallback.name = name;
    fallback.argc = argc;
    fallback.argv0 = argc > 0 ? argv[0] : NULL;
    if(!fallback.claim)
        return false;
    *exit_code = fallback.code;
    return true;
}

static int run_app_with_fallback(int argc, char **argv, bool claim, int code) {
    const cli_command commands[] = {
        { "do", "a test command", "<name>", test_options,
          sizeof test_options / sizeof test_options[0], capture_handler },
    };
    const cli_app app = { "testapp", "9.9.9", "tagline", commands, 1, capture_unknown };
    memset(&seen, 0, sizeof seen);
    memset(&fallback, 0, sizeof fallback);
    fallback.claim = claim;
    fallback.code = code;
    return cli_app_run(&app, argc, argv);
}

MOLTEST(cli_hands_an_unknown_name_to_the_fallback) {
    char *argv[] = { "testapp", "deb", "--output", "x.deb" };
    EXPECT_EQ(7, run_app_with_fallback(4, argv, true, 7));

    EXPECT_TRUE(fallback.called);
    EXPECT_STREQ("deb", fallback.name);

    /* Unparsed, because the framework has no option spec for a name it does
       not know and would otherwise reject flags that are valid to whoever
       handles it. */
    EXPECT_EQ(2, fallback.argc);
    EXPECT_STREQ("--output", fallback.argv0);
}

MOLTEST(cli_reports_the_usual_error_when_the_fallback_declines) {
    char *argv[] = { "testapp", "typo" };
    EXPECT_EQ(exit_usage_error, run_app_with_fallback(2, argv, false, 0));

    /* Asked, and said no — so the user still gets the list of what does exist,
       which is the right answer for a typo. */
    EXPECT_TRUE(fallback.called);
}

MOLTEST(cli_never_offers_a_built_in_command_to_the_fallback) {
    /* The table is searched first, so nothing installed can take a name Molto
       already answers. */
    char *argv[] = { "testapp", "do", "x" };
    EXPECT_EQ(0, run_app_with_fallback(3, argv, true, 7));

    EXPECT_FALSE(fallback.called);
    EXPECT_TRUE(seen.called);
}

MOLTEST(cli_without_a_fallback_still_refuses_an_unknown_name) {
    char *argv[] = { "testapp", "deb" };
    EXPECT_EQ(exit_usage_error, run_app(2, argv));
}

MOLTEST(only_build_takes_a_target) {
    /* `--target` reads as "build for that platform". `run` and `test` finish by
       starting what they built, and a binary for another platform does not
       start here — so taking the flag and ignoring it is worse than refusing
       it.

       The three shared one option table until this was noticed, and
       `molto test --target x86_64-w64-mingw32` exited 0 having built for this
       machine instead. Driven through the real command table rather than the
       synthetic one above, because what is under test is which table each
       command was given. */
    char *refused[] = {"molto", "test", "--target", "x86_64-w64-mingw32", NULL};
    EXPECT_EQ(exit_usage_error, cli_run(4, refused));

    char *refused_short[] = {"molto", "run", "-t", "x86_64-w64-mingw32", NULL};
    EXPECT_EQ(exit_usage_error, cli_run(4, refused_short));

    /* And build still has it. `--help` stops before any work is done, so this
       asks the parser and nothing else. */
    char *accepted[] = {"molto", "build", "--target", "x86_64-w64-mingw32", "--help", NULL};
    EXPECT_EQ(exit_ok, cli_run(5, accepted));
}
