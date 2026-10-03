#include <moltest.h>

#include <molto/commands/lint_command.h>
#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/lint_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Stand-ins for the compiler and the linter, so these exercise Molto's side of
   the contract without depending on what this machine has. Each logs its argv
   and echoes a canned transcript, the compiler on stderr and the linter on
   stdout — which is exactly how the real ones differ. */
typedef struct {
    char root[64];
    char tools[64];
    char compiler[128];
    char linter[128];
    char log[128];
    char pickup[128];
    char xcrun[128];
    char saved_cc[4096];
    char saved_tidy[4096];
    char saved_pickup[4096];
    char saved_sdk[4096];
    char saved_xcrun[4096];
    bool had_cc;
    bool had_tidy;
    bool had_pickup;
    bool had_sdk;
    bool had_xcrun;
} lint_fixture;

/* True if `text` ends with `suffix`. */
static bool ends_with(const char *text, const char *suffix) {
    const size_t length = strlen(text), tail = strlen(suffix);
    return length >= tail && strcmp(text + length - tail, suffix) == 0;
}

/*
 * A stub that logs its argv, echoes a canned transcript, and — like a real
 * compiler — honours -MF by writing a dependency list. The cache reads that
 * list to learn which headers a file included (RFC-0006); without it there is
 * nothing to watch and nothing is ever recorded.
 *
 * The transcript travels in a file rather than in a setting: a setting is one
 * line and a compiler's output is many.
 */
MOLTEST_FAKE(fake_lint_compiler) {
    const char *log = moltest_fake_setting("log");
    FILE *file;
    /* SDKROOT is how lint tells a linter where the macOS SDK is, so a tool that
       was handed one says so at the start of its line. */
    const char *sdk = getenv("SDKROOT");
    char prefix[512];
    snprintf(prefix, sizeof prefix, "SDKROOT=%s", sdk != NULL ? sdk : "");
    if (log != NULL)
        (void)moltest_log_argv(log, sdk != NULL ? prefix : NULL, argc, argv);

    const char *src = NULL, *dep = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-MF") == 0 && i + 1 < argc)
            dep = argv[++i];
        else if (ends_with(argv[i], ".c"))
            src = argv[i];
    }
    if (dep != NULL && src != NULL && (file = fopen(dep, "wb")) != NULL) {
        fprintf(file, "out.o: %s\n", src);
        (void)fclose(file);
    }

    /* The edit an editor makes while the compiler is still running. */
    const char *edit = moltest_fake_setting("edit");
    if (edit != NULL && src != NULL && (file = fopen(src, "ab")) != NULL) {
        fprintf(file, "%s\n", edit);
        (void)fclose(file);
    }

    const char *transcript = moltest_fake_setting("transcript");
    const char *stream = moltest_fake_setting("stream");
    if (transcript != NULL) {
        char *text = fs_read_file(transcript);
        if (text != NULL) {
            fprintf(stream != NULL && strcmp(stream, "2") == 0 ? stderr : stdout, "%s\n", text);
            free(text);
        }
    }

    const char *code = moltest_fake_setting("exit");
    return code != NULL ? atoi(code) : 0;
}

static bool write_stub(const char *path, const char *log, const char *transcript,
                       const char *stream, int exit_code) {
    char transcript_path[MOLTEST_PATH];
    if (snprintf(transcript_path, sizeof transcript_path, "%s.transcript", path)
        >= (int)sizeof transcript_path)
        return false;
    if (!fs_write_file(transcript_path, transcript))
        return false;

    char spec[2048];
    if (snprintf(spec, sizeof spec,
                 "set log %s\n"
                 "set transcript %s\n"
                 "set stream %s\n"
                 "set exit %d\n"
                 "behave fake_lint_compiler\n",
                 log, transcript_path, stream, exit_code)
        >= (int)sizeof spec)
        return false;
    return moltest_fake_program(path, spec, NULL, 0);
}

/* A stand-in for `xcrun`, the one place macOS says where its SDK is. It logs
   that it was asked, so a test can tell an SDK that was looked up from one
   that was taken from the environment. */
MOLTEST_FAKE(fake_xcrun) {
    const char *log = moltest_fake_setting("log");
    if (log != NULL)
        (void)moltest_log_argv(log, "xcrun", argc, argv);
    const char *sdk = moltest_fake_setting("sdk");
    if (sdk != NULL)
        printf("%s\n", sdk);
    const char *code = moltest_fake_setting("exit");
    return code != NULL ? atoi(code) : 0;
}

static bool write_xcrun(const lint_fixture *fixture, const char *sdk, int exit_code) {
    char spec[1024];
    if (snprintf(spec, sizeof spec,
                 "set log %s\n"
                 "set sdk %s\n"
                 "set exit %d\n"
                 "behave fake_xcrun\n",
                 fixture->log, sdk, exit_code)
        >= (int)sizeof spec)
        return false;
    return moltest_fake_program(fixture->xcrun, spec, NULL, 0);
}

static bool write_file(const char *root, const char *relative, const char *body) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", root, relative);

    char directory[256];
    snprintf(directory, sizeof directory, "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (!fs_make_dirs(directory))
            return false;
    }
    return fs_write_file(path, body);
}

static void remember_env(const char *name, char *into, size_t size, bool *had) {
    const char *existing = getenv(name);
    *had = existing != NULL;
    if (existing != NULL)
        snprintf(into, size, "%s", existing);
}

static void restore_env(const char *name, const char *saved, bool had) {
    if (had)
        (void)setenv(name, saved, 1);
    else
        (void)unsetenv(name);
}

static bool fixture_setup(lint_fixture *fixture, const char *compiler_says,
                          int compiler_exit, const char *linter_says) {
    if (!moltest_temp_dir("molto_lint_bin", fixture->tools, sizeof fixture->tools) || !moltest_temp_dir("molto_lint_ws", fixture->root, sizeof fixture->root))
        return false;

    snprintf(fixture->compiler, sizeof fixture->compiler, "%s/cc", fixture->tools);
    snprintf(fixture->linter, sizeof fixture->linter, "%s/tidy", fixture->tools);
    snprintf(fixture->log, sizeof fixture->log, "%s/calls", fixture->tools);

    remember_env("C_COMPILER", fixture->saved_cc, sizeof fixture->saved_cc,
                 &fixture->had_cc);
    remember_env("MOLTO_CLANG_TIDY", fixture->saved_tidy, sizeof fixture->saved_tidy,
                 &fixture->had_tidy);
    remember_env("MOLTO_PICKUP", fixture->saved_pickup, sizeof fixture->saved_pickup,
                 &fixture->had_pickup);
    remember_env("SDKROOT", fixture->saved_sdk, sizeof fixture->saved_sdk,
                 &fixture->had_sdk);
    remember_env("MOLTO_XCRUN", fixture->saved_xcrun, sizeof fixture->saved_xcrun,
                 &fixture->had_xcrun);

    /* Every test gets an xcrun of its own, so none of them depends on whether
       this Mac has the Command Line Tools — and an SDKROOT that `make` or an
       IDE exported is not mistaken for one the test chose. */
    snprintf(fixture->xcrun, sizeof fixture->xcrun, "%s/xcrun", fixture->tools);
    if (!write_xcrun(fixture, "/fake/sdk", 0))
        return false;
    (void)unsetenv("SDKROOT");

    /* A compiler diagnoses on stderr; clang-tidy prints to stdout. */
    if (!write_stub(fixture->compiler, fixture->log, compiler_says, "2", compiler_exit))
        return false;
    if (linter_says != NULL
        && !write_stub(fixture->linter, fixture->log, linter_says, "1", 0))
        return false;

    /* "No linter" means pickup reporting none, not a path that does not run:
       an override naming a missing binary is a different failure, and lint is
       right to report that one. */
    snprintf(fixture->pickup, sizeof fixture->pickup, "%s/pickup", fixture->tools);
    if (!write_stub(fixture->pickup, fixture->log,
                    "[[tool]]\nkind = \"formatter\"\nname = \"clang-format\"\n"
                    "path = \"/bin/sh\"\n", "1", 0))
        return false;

    return setenv("C_COMPILER", fixture->compiler, 1) == 0
        && setenv("MOLTO_PICKUP", fixture->pickup, 1) == 0
        && setenv("MOLTO_XCRUN", fixture->xcrun, 1) == 0
        && (linter_says != NULL
                ? setenv("MOLTO_CLANG_TIDY", fixture->linter, 1) == 0
                : unsetenv("MOLTO_CLANG_TIDY") == 0)
        && write_file(fixture->root, "Project.toml",
                      "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n"
                      "\n[target]\nstd = \"c17\"\ndefines = [\"FOO=1\"]\n")
        && write_file(fixture->root, "src/main.c", "int main(void){return 0;}\n");
}

static void fixture_teardown(lint_fixture *fixture) {
    restore_env("C_COMPILER", fixture->saved_cc, fixture->had_cc);
    restore_env("MOLTO_CLANG_TIDY", fixture->saved_tidy, fixture->had_tidy);
    restore_env("MOLTO_PICKUP", fixture->saved_pickup, fixture->had_pickup);
    restore_env("SDKROOT", fixture->saved_sdk, fixture->had_sdk);
    restore_env("MOLTO_XCRUN", fixture->saved_xcrun, fixture->had_xcrun);
    char cmd[256];
    (void)fs_remove_tree(fixture->root);
    (void)fs_remove_tree(fixture->tools);
}

static int run_lint(const lint_fixture *fixture, diagnostic_list *out) {
    const lint_request request = {
        .profile = profile_debug,
        .refresh_toolchain = false,
        .refresh_tools = false,
    };
    diagnostic_list_init(out);
    return lint_project(fixture->root, &request, out);
}

/* What a compiler says about one file. */
#define COMPILER_TRANSCRIPT \
    "src/main.c: In function 'main':\n" \
    "src/main.c:1:16: warning: unused variable 'x' [-Wunused-variable]"

MOLTEST(lint_collects_what_the_compiler_reports) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    EXPECT_EQ(1, (int)diagnostic_count_severity(&found, diagnostic_severity_warning));
    /* The line the compiler could not be parsed from is kept, not dropped. */
    EXPECT_EQ(1, (int)diagnostic_count_severity(&found, diagnostic_severity_unknown));

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_also_runs_the_linter_that_pickup_reports) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0,
        "src/main.c:1:5: error: an assignment within an 'if' condition is bug-prone "
        "[bugprone-assignment-in-if-condition]"));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    EXPECT_EQ(1, (int)diagnostic_count_severity(&found, diagnostic_severity_error));
    EXPECT_EQ(1, (int)diagnostic_count_severity(&found, diagnostic_severity_warning));

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_falls_back_to_the_compiler_when_there_is_no_linter) {
    lint_fixture fixture;
    /* The compiler pass is what RFC-0005 promises with nothing installed, so a
       machine without a linter still gets a useful answer. */
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));
    EXPECT_TRUE(diagnostic_list_count(&found) > 0);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_passes_the_project_settings_and_produces_no_build_output) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_NOT_NULL(strstr(log, "-fsyntax-only"));
    /* The manifest's defines decide what even compiles, so lint has to see the
       same ones the build does. */
    EXPECT_NOT_NULL(strstr(log, "-DFOO=1"));
    EXPECT_NOT_NULL(strstr(log, "-std=c17"));
    /* -MMD is asked for: the dependency list is what tells the cache which
       headers a file read (RFC-0006), and it goes to .bin/, not to build/. */
    EXPECT_NOT_NULL(strstr(log, "-MMD"));
    /* And nothing that would produce an artifact. */
    EXPECT_NULL(strstr(log, " -c "));
    EXPECT_NULL(strstr(log, " -o "));
    free(log);

    /* The defining property of the command: it analyses and builds nothing. */
    char build_dir[256];
    snprintf(build_dir, sizeof build_dir, "%s/build", fixture.root);
    EXPECT_FALSE(fs_path_exists(build_dir));

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

/* A sibling package the project depends on by path, exporting a header
   directory and a define. */
static bool add_dependency(const lint_fixture *fixture) {
    return write_file(fixture->root, "modules/greet/recipe.toml",
                      "schema = 1\nform = \"source\"\nkind = \"package\"\n"
                      "name = \"greet\"\nversion = \"0.1.0\"\ntarget = \"any\"\n"
                      "[artifacts]\ntype = \"source\"\nsources = [\"src/greet.c\"]\n"
                      "include = [\"include\"]\ndefines = [\"GREET_STATIC=1\"]\n") &&
           write_file(fixture->root, "modules/greet/include/greet.h", "int greet(void);\n") &&
           write_file(fixture->root, "modules/greet/src/greet.c", "int greet(void){return 42;}\n") &&
           write_file(fixture->root, "Project.toml",
                      "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n"
                      "\n[target]\nstd = \"c17\"\n"
                      "\n[deps]\ngreet = { path = \"modules/greet\" }\n");
}

MOLTEST(lint_analyses_against_what_the_dependencies_export) {
    /* Lint used to resolve nothing at all, so a project with any dependency was
       told its own sources could not find their headers — a diagnostic that
       blames the user for a file Molto never looked for. The defines matter for
       the same reason the manifest's do: a `#ifdef` decides what compiles, and
       a linter that saw different ones reports on code the build never sees. */
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));
    ASSERT_TRUE(add_dependency(&fixture));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_NOT_NULL(strstr(log, "modules/greet/include"));
    EXPECT_NOT_NULL(strstr(log, "-DGREET_STATIC=1"));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

/* A pkg-config that answers for one made-up toolkit, so these tests do not
   depend on what this machine has installed. It volunteers options that are not
   include or link flags, because dropping those is part of the contract. */
MOLTEST_FAKE(fake_pkg_config) {
    if (argc < 2)
        return 0;
    if (strcmp(argv[1], "--exists") == 0)
        return argc > 2 && strcmp(argv[2], "toykit") == 0 ? 0 : 1;
    if (strcmp(argv[1], "--modversion") == 0)
        printf("1.2.3\n");
    else if (strcmp(argv[1], "--cflags") == 0)
        printf("-I/opt/toykit/include -pthread -D_REENTRANT\n");
    return 0;
}

static bool write_resolver(const char *path) {
    return moltest_fake_program(path, "behave fake_pkg_config\n", NULL, 0);
}

MOLTEST(lint_analyses_against_what_the_host_provides) {
    /* The build resolves `[target].host` (RFC-0016) and lint has to resolve it
       too. While it did not, a project naming a toolkit compiled cleanly and
       then reported `file not found` for that toolkit's header on every one of
       its sources — which is not a finding about the code, it is lint being
       unable to read it, and it buried everything lint had to say. */
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));

    char resolver[256];
    snprintf(resolver, sizeof resolver, "%s/pkg-config", fixture.tools);
    ASSERT_TRUE(write_resolver(resolver));
    ASSERT_EQ(0, setenv("MOLTO_PKG_CONFIG", resolver, 1));
    ASSERT_TRUE(write_file(fixture.root, "Project.toml",
                           "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n"
                           "\n[target]\nstd = \"c17\"\nhost = [\"toykit\"]\n"));

    diagnostic_list found;
    const int status = run_lint(&fixture, &found);
    (void)unsetenv("MOLTO_PKG_CONFIG");
    ASSERT_EQ(exit_ok, status);

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);

    /* As -isystem, which is the distinction the build already draws by marking
       these paths `system`: a toolkit's headers are not this project's code,
       and analysing them buries whatever lint had to say about the code that
       is. */
    EXPECT_NOT_NULL(strstr(log, "-isystem"));
    EXPECT_NOT_NULL(strstr(log, "/opt/toykit/include"));

    /* And only what the contract keeps. An option a `.pc` file volunteered is a
       compile option entering the build from outside the manifest that was
       reviewed, and lint must analyse what the build compiles. */
    EXPECT_NULL(strstr(log, "-D_REENTRANT"));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_resolves_the_standard_the_way_the_build_does) {
    /* A compile line is composed from the document now, where `-std` is a
       unit-scope option and unit scope reaches the line last (RFC-0013). So
       `[target].std` wins over a `-std=` written by hand into `[target].flags`,
       and lint has to reach the same answer: one that composed them the other
       way round would analyse the file as a different language than the one it
       is compiled as. */
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));
    ASSERT_TRUE(write_file(fixture.root, "Project.toml",
                           "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n"
                           "\n[target]\nstd = \"c17\"\nflags = [\"-std=gnu11\"]\n"));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    const char *hand_written = strstr(log, "-std=gnu11");
    const char *declared = strstr(log, "-std=c17");
    ASSERT_NOT_NULL(hand_written);
    ASSERT_NOT_NULL(declared);
    /* The last one on the line is the one the compiler takes. */
    EXPECT_TRUE(declared > hand_written);
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_hands_the_compile_arguments_to_the_linter_after_the_separator) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "", 0, ""));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    /* A linter that does not see the build's flags is analysing other code.
       They follow the separator; where in them the standard falls is
       `push_compile_arguments`' business, and it puts it last. */
    const char *separator = strstr(log, " -- ");
    ASSERT_NOT_NULL(separator);
    EXPECT_NOT_NULL(strstr(separator, "-std=c17"));
    EXPECT_NOT_NULL(strstr(log, "--config-file="));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_orders_the_diagnostics_by_source) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));
    ASSERT_TRUE(write_file(fixture.root, "src/aaa.c", "int a(void){return 0;}\n"));
    ASSERT_TRUE(write_file(fixture.root, "src/zzz.c", "int z(void){return 0;}\n"));

    /* Two runs over one tree must report the same thing in the same order,
       however the pool happened to schedule them. */
    diagnostic_list first;
    diagnostic_list second;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &first));
    ASSERT_EQ(exit_ok, run_lint(&fixture, &second));

    ASSERT_EQ((int)diagnostic_list_count(&first), (int)diagnostic_list_count(&second));
    for (size_t i = 0; i < diagnostic_list_count(&first); i++) {
        EXPECT_STREQ(diagnostic_list_get(&first, i)->message,
                     diagnostic_list_get(&second, i)->message);
    }

    diagnostic_list_free(&first);
    diagnostic_list_free(&second);
    fixture_teardown(&fixture);
}

MOLTEST(lint_skips_the_sources_linter_json_excludes) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "", 0, NULL));
    ASSERT_TRUE(write_file(fixture.root, "src/vendor/third.c", "int t(void){return 0;}\n"));
    ASSERT_TRUE(write_file(fixture.root, "linter.json",
                           "{\"exclude\": [\"src/vendor/**\"]}\n"));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_NULL(strstr(log, "third.c"));
    EXPECT_NOT_NULL(strstr(log, "main.c"));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_reports_a_tool_that_failed_without_saying_why) {
    lint_fixture fixture;
    /* A tool that fails silently still has to fail the lint, which it does by
       producing an error diagnostic like any other. */
    ASSERT_TRUE(fixture_setup(&fixture, "", 4, NULL));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));
    EXPECT_EQ(1, (int)diagnostic_count_severity(&found, diagnostic_severity_error));

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_refuses_a_rule_it_cannot_translate) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "", 0, ""));
    ASSERT_TRUE(write_file(fixture.root, "linter.json",
                           "{\"rules\": {\"no_such_rule\": \"error\"}}\n"));

    diagnostic_list found;
    /* Validated before a single process is spawned, so a rule that cannot be
       translated does not cost N runs before it is reported. */
    EXPECT_EQ(exit_invalid_manifest, run_lint(&fixture, &found));

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_reports_an_invalid_configuration) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "", 0, NULL));
    ASSERT_TRUE(write_file(fixture.root, "linter.json", "{\"rules\": \n"));

    diagnostic_list found;
    EXPECT_EQ(exit_invalid_manifest, run_lint(&fixture, &found));

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

/* --- the result cache (RFC-0006) --- */

/* How many times an analysis pass ran, from the log every stub appends to.
   Only the compiler is counted: pickup is asked again on every run when it
   reports no linter, because there is no answer to record, and that has
   nothing to do with whether a file was re-analysed. */
static int invocations(const lint_fixture *fixture) {
    char *log = fs_read_file(fixture->log);
    if (log == NULL)
        return 0;
    int passes = 0;
    for (const char *at = strstr(log, "-fsyntax-only"); at != NULL;
         at = strstr(at + 1, "-fsyntax-only"))
        passes++;
    free(log);
    return passes;
}

/* Everything a run would print, so two runs can be compared as the user would
   see them and not merely by counting diagnostics. */
static char *rendered(const diagnostic_list *list, const char *root) {
    char *out = calloc(1, 8192);
    if (out == NULL)
        return NULL;
    size_t used = 0;
    for (size_t i = 0; i < diagnostic_list_count(list); i++) {
        char line[2048] = "";
        if (diagnostic_format(diagnostic_list_get(list, i), root, line, sizeof line))
            used += (size_t)snprintf(out + used, 8192 - used, "%s\n", line);
    }
    return out;
}

static int run_lint_refreshing(const lint_fixture *fixture, diagnostic_list *out,
                               bool refresh_analysis) {
    const lint_request request = {
        .profile = profile_debug,
        .refresh_toolchain = false,
        .refresh_tools = false,
        .refresh_analysis = refresh_analysis,
    };
    diagnostic_list_init(out);
    return lint_project(fixture->root, &request, out);
}

MOLTEST(lint_replays_a_recorded_result_instead_of_running_the_tools_again) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));

    diagnostic_list first;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &first));
    int after_first = invocations(&fixture);
    EXPECT_TRUE(after_first > 0);

    diagnostic_list second;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &second));

    /* The work avoided is the process that would have run. */
    EXPECT_EQ(after_first, invocations(&fixture));

    /* RFC-0006's obligation: a cached run is indistinguishable from an uncached
       one except in how long it took. A warning replayed as silence would be a
       false pass in CI, which is the whole reason this is not a boolean. */
    char *was = rendered(&first, fixture.root);
    char *is = rendered(&second, fixture.root);
    ASSERT_NOT_NULL(was);
    ASSERT_NOT_NULL(is);
    EXPECT_STREQ(was, is);
    EXPECT_EQ(1, (int)diagnostic_count_severity(&second, diagnostic_severity_warning));

    free(was);
    free(is);
    diagnostic_list_free(&first);
    diagnostic_list_free(&second);
    fixture_teardown(&fixture);
}

MOLTEST(lint_analyses_a_source_again_once_it_changes) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));

    diagnostic_list first;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &first));
    int after_first = invocations(&fixture);

    /* Content, not timestamp: the store confirms a changed mtime with a hash,
       so a file that was only touched is correctly left alone.

       The replacement differs in length, and that is load-bearing rather
       than incidental. `current_hash` memoises on (mtime, size) and only
       reaches for the hash when one of the two moves. Windows stamps a
       write from a clock that advances every 15.6ms rather than from the
       100ns field it writes into: measured on this machine, 198 of 200
       back-to-back writes left `ftLastWriteTime` identical. The edit this
       test used to make was the same 26 bytes as the file it replaced, so
       on a runner quick enough to fit `run_lint` inside one tick both keys
       matched, the store answered from the memo without opening the file,
       and the case failed for a reason that had nothing to do with linting. */
    ASSERT_TRUE(write_file(fixture.root, "src/main.c",
                           "int main(void){int changed = 1; return changed;}\n"));

    diagnostic_list second;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &second));
    EXPECT_TRUE(invocations(&fixture) > after_first);

    diagnostic_list_free(&first);
    diagnostic_list_free(&second);
    fixture_teardown(&fixture);
}

MOLTEST(lint_analyses_a_source_again_once_the_env_changes) {
    /* The tools run in the project's [env], so a recorded diagnostic answers
       for one environment and not another. */
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));

    diagnostic_list first;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &first));
    int after_first = invocations(&fixture);

    ASSERT_TRUE(write_file(fixture.root, "Project.toml",
                           "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n"
                           "\n[target]\nstd = \"c17\"\ndefines = [\"FOO=1\"]\n"
                           "\n[env]\nCPATH = \"/opt/include\"\n"));

    diagnostic_list second;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &second));
    EXPECT_TRUE(invocations(&fixture) > after_first);

    diagnostic_list_free(&first);
    diagnostic_list_free(&second);
    fixture_teardown(&fixture);
}

MOLTEST(lint_analyses_everything_again_when_asked_to_refresh) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));

    diagnostic_list first;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &first));
    int after_first = invocations(&fixture);

    /* The escape hatch for a tool the fingerprint cannot describe. */
    diagnostic_list second;
    ASSERT_EQ(exit_ok, run_lint_refreshing(&fixture, &second, true));
    EXPECT_TRUE(invocations(&fixture) > after_first);

    diagnostic_list_free(&first);
    diagnostic_list_free(&second);
    fixture_teardown(&fixture);
}

MOLTEST(lint_does_not_record_a_tool_that_failed_without_explaining_itself) {
    lint_fixture fixture;
    /* Exits non-zero and says nothing: Molto synthesises an error for it. */
    ASSERT_TRUE(fixture_setup(&fixture, "", 1, NULL));

    diagnostic_list first;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &first));
    int after_first = invocations(&fixture);
    EXPECT_EQ(1, (int)diagnostic_count_severity(&first, diagnostic_severity_error));

    /* Recording that would replay a failure the next run might not have, and
       would never retry it. A broken tool has to be asked again. */
    diagnostic_list second;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &second));
    EXPECT_TRUE(invocations(&fixture) > after_first);

    diagnostic_list_free(&first);
    diagnostic_list_free(&second);
    fixture_teardown(&fixture);
}

MOLTEST(lint_does_not_record_a_result_for_content_that_is_already_gone) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0, NULL));

    /* A compiler slow enough for the file to be edited while it runs, which is
       what an editor saving during a lint does. */
    char transcript_path[MOLTEST_PATH];
    snprintf(transcript_path, sizeof transcript_path, "%s.transcript", fixture.compiler);
    ASSERT_TRUE(fs_write_file(
        transcript_path, "src/main.c:1:16: warning: unused variable 'x' [-Wunused-variable]"));

    char spec[1024];
    snprintf(spec, sizeof spec,
             "set log %s\n"
             "set transcript %s\n"
             "set stream 2\n"
             "set exit 0\n"
             "set edit int changed_while_running(void);\n"
             "behave fake_lint_compiler\n",
             fixture.log, transcript_path);
    ASSERT_TRUE(moltest_fake_program(fixture.compiler, spec, NULL, 0));

    diagnostic_list first;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &first));
    int after_first = invocations(&fixture);

    /* Recording here would pin diagnostics about content that no longer exists
       under the signature of the content that replaced it — and nothing would
       ever invalidate them again. The file has to be analysed afresh. */
    diagnostic_list second;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &second));
    EXPECT_TRUE(invocations(&fixture) > after_first);

    diagnostic_list_free(&first);
    diagnostic_list_free(&second);
    fixture_teardown(&fixture);
}

/* --- the exit status of `molto lint` (docs/Style.md) --- */

/* What clang-tidy prints when a header it was pointed at cannot be found: it
   could not even parse the file, so nothing was analysed, and it says so with
   an error of its own and a non-zero status. */
#define TIDY_COULD_NOT_PROCESS \
    "Error while processing src/main.c.\n" \
    "src/main.c:1:10: error: 'stdbool.h' file not found [clang-diagnostic-error]"

/* Run the command itself in the fixture's workspace and return its status.

   The service tests above only see the diagnostics; the exit status is decided
   one layer up, and that is the number CI reads. A lint that reports errors and
   exits 0 is a CI job that passes a lint that never ran. JSON keeps the test
   output to one small document and no progress line. */
static int run_lint_command(const lint_fixture *fixture) {
    char previous[4096];
    if (getcwd(previous, sizeof previous) == NULL || chdir(fixture->root) != 0)
        return -1;
    int code = lint_command_run(NULL, false, false, false, "json", 0);
    (void)chdir(previous);
    return code;
}

/* Make the linter stub say `transcript` and exit with `code`. */
static bool linter_says(const lint_fixture *fixture, const char *transcript, int code) {
    return write_stub(fixture->linter, fixture->log, transcript, "1", code);
}

MOLTEST(lint_command_fails_when_an_error_is_reported) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "", 0, ""));
    ASSERT_TRUE(linter_says(&fixture,
        "src/main.c:1:5: error: an assignment within an 'if' condition is bug-prone "
        "[bugprone-assignment-in-if-condition]", 1));

    EXPECT_EQ(exit_build_failure, run_lint_command(&fixture));
    /* A replayed run has to fail the same way: replaying an error as silence
       would turn the second CI run green. */
    EXPECT_EQ(exit_build_failure, run_lint_command(&fixture));

    fixture_teardown(&fixture);
}

MOLTEST(lint_command_fails_when_the_linter_could_not_process_a_file) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "", 0, ""));
    ASSERT_TRUE(linter_says(&fixture, TIDY_COULD_NOT_PROCESS, 1));

    EXPECT_EQ(exit_build_failure, run_lint_command(&fixture));

    fixture_teardown(&fixture);
}

MOLTEST(lint_command_fails_when_the_linter_gives_up_without_a_diagnostic) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "", 0, ""));
    /* Only the banner and a failing status: no parsed error to count, and the
       file was still never analysed. */
    ASSERT_TRUE(linter_says(&fixture, "Error while processing src/main.c.", 1));

    EXPECT_EQ(exit_build_failure, run_lint_command(&fixture));

    fixture_teardown(&fixture);
}

MOLTEST(lint_command_succeeds_when_there_are_only_warnings) {
    lint_fixture fixture;
    /* Documented: a warning is reported and still succeeds. */
    ASSERT_TRUE(fixture_setup(&fixture, COMPILER_TRANSCRIPT, 0,
        "src/main.c:1:5: warning: variable 'x' is never read [clang-analyzer-deadcode]"));

    EXPECT_EQ(exit_ok, run_lint_command(&fixture));

    fixture_teardown(&fixture);
}

MOLTEST(lint_command_succeeds_on_a_clean_run) {
    lint_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "", 0, ""));

    EXPECT_EQ(exit_ok, run_lint_command(&fixture));

    fixture_teardown(&fixture);
}

/* --- the macOS SDK --- */

/* How many times `needle` occurs in `text`. */
static int occurrences(const char *text, const char *needle) {
    int count = 0;
    for (const char *at = strstr(text, needle); at != NULL; at = strstr(at + 1, needle))
        count++;
    return count;
}

/* A linter that reports nothing, run over two sources: the SDK is a question
   about the machine, so asking it once per file would be asking it N times. */
static bool sdk_fixture_setup(lint_fixture *fixture) {
    return fixture_setup(fixture, "", 0, "")
        && write_file(fixture->root, "src/other.c", "int other(void){return 1;}\n");
}

#ifdef __APPLE__
MOLTEST(lint_tells_the_linter_where_the_macos_sdk_is) {
    /* Upstream clang, which is what clang-tidy is, does not look for the SDK
       the way Apple's clang does, and macOS has no /usr/include: without being
       told, clang-tidy reports `'stdio.h' file not found` on every source. */
    lint_fixture fixture;
    ASSERT_TRUE(sdk_fixture_setup(&fixture));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_EQ(1, occurrences(log, "xcrun --show-sdk-path"));
    EXPECT_EQ(2, occurrences(log, "SDKROOT=/fake/sdk --config-file="));
    /* The compiler pass is the build's own compiler, which finds the SDK the
       way the build does; lint must not hand it anything the build does not. */
    EXPECT_NULL(strstr(log, "SDKROOT=/fake/sdk -fsyntax-only"));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_respects_an_sdk_the_environment_already_names) {
    /* SDKROOT is how a user, or Xcode, says which SDK they mean; lint asking
       xcrun over it would silently analyse against a different one. */
    lint_fixture fixture;
    ASSERT_TRUE(sdk_fixture_setup(&fixture));
    ASSERT_EQ(0, setenv("SDKROOT", "/user/sdk", 1));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_NULL(strstr(log, "xcrun"));
    EXPECT_EQ(2, occurrences(log, "SDKROOT=/user/sdk --config-file="));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_respects_an_sdk_the_project_env_names) {
    lint_fixture fixture;
    ASSERT_TRUE(sdk_fixture_setup(&fixture));
    ASSERT_TRUE(write_file(fixture.root, "Project.toml",
                           "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n"
                           "\n[target]\nstd = \"c17\"\n"
                           "\n[env]\nSDKROOT = \"/project/sdk\"\n"));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_NULL(strstr(log, "xcrun"));
    EXPECT_EQ(2, occurrences(log, "SDKROOT=/project/sdk --config-file="));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_does_not_give_a_second_sysroot_to_a_command_that_has_one) {
    /* A project that names its SDK on the compile line has decided; the linter
       sees that line after the separator and needs nothing else. */
    lint_fixture fixture;
    ASSERT_TRUE(sdk_fixture_setup(&fixture));
    ASSERT_TRUE(write_file(fixture.root, "Project.toml",
                           "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n"
                           "\n[target]\nstd = \"c17\"\n"
                           "flags = [\"-isysroot\", \"/chosen/sdk\"]\n"));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_NULL(strstr(log, "xcrun"));
    EXPECT_NULL(strstr(log, "SDKROOT="));
    /* Once per pass per source: two sources, a compiler and a linter each. */
    EXPECT_EQ(4, occurrences(log, "-isysroot"));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_still_runs_the_linter_when_the_sdk_cannot_be_found) {
    /* A Mac without the Command Line Tools. Lint says why the linter is about
       to miss the system headers, and runs it anyway: what it reports is still
       the honest answer, and the compiler pass is unaffected. */
    lint_fixture fixture;
    ASSERT_TRUE(sdk_fixture_setup(&fixture));
    ASSERT_TRUE(write_xcrun(&fixture, "", 1));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_EQ(1, occurrences(log, "xcrun --show-sdk-path"));
    EXPECT_NULL(strstr(log, "SDKROOT="));
    EXPECT_EQ(2, occurrences(log, "--config-file="));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}

MOLTEST(lint_analyses_again_once_the_sdk_changes) {
    /* The system headers live in the SDK, and the dependency lists the cache
       watches leave system headers out, so the SDK itself has to be part of
       what a recorded result answers for. */
    lint_fixture fixture;
    ASSERT_TRUE(sdk_fixture_setup(&fixture));

    diagnostic_list first;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &first));
    int after_first = invocations(&fixture);

    ASSERT_TRUE(write_xcrun(&fixture, "/other/sdk", 0));
    diagnostic_list second;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &second));
    EXPECT_TRUE(invocations(&fixture) > after_first);

    diagnostic_list_free(&first);
    diagnostic_list_free(&second);
    fixture_teardown(&fixture);
}
#else
MOLTEST(lint_gives_the_linter_no_sdk_off_macos) {
    /* Only macOS keeps its system headers in an SDK. Everywhere else the
       linter finds them where the compiler does, and nothing is asked. */
    lint_fixture fixture;
    ASSERT_TRUE(sdk_fixture_setup(&fixture));

    diagnostic_list found;
    ASSERT_EQ(exit_ok, run_lint(&fixture, &found));

    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_NULL(strstr(log, "xcrun"));
    EXPECT_NULL(strstr(log, "SDKROOT="));
    EXPECT_EQ(2, occurrences(log, "--config-file="));
    free(log);

    diagnostic_list_free(&found);
    fixture_teardown(&fixture);
}
#endif
