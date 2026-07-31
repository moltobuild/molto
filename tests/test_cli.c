#include <moltest.h>

#include <molto/cli.h>
#include <molto/util/cli.h>

#include <stdbool.h>
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
    const cli_app app = { "testapp", "9.9.9", "tagline", commands, 1 };
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
