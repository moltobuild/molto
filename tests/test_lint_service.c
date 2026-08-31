#include <moltest.h>

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
    char saved_cc[4096];
    char saved_tidy[4096];
    char saved_pickup[4096];
    bool had_cc;
    bool had_tidy;
    bool had_pickup;
} lint_fixture;

/* A stub that logs its argv, echoes a canned transcript, and — like a real
   compiler — honours -MF by writing a dependency list. The cache reads that
   list to learn which headers a file included (RFC-0006); without it there is
   nothing to watch and nothing is ever recorded. */
static bool write_stub(const char *path, const char *log, const char *transcript,
                       const char *stream, int exit_code) {
    char script[2048];
    snprintf(script, sizeof script,
             "#!/bin/sh\n"
             "echo \"$@\" >> %s\n"
             "src=''; dep=''; prev=''\n"
             "for a in \"$@\"; do\n"
             "  case \"$a\" in *.c) src=\"$a\";; esac\n"
             "  [ \"$prev\" = '-MF' ] && dep=\"$a\"\n"
             "  prev=\"$a\"\n"
             "done\n"
             "[ -n \"$dep\" ] && [ -n \"$src\" ] && echo \"out.o: $src\" > \"$dep\"\n"
             "cat >&%s <<'TRANSCRIPT'\n%s\nTRANSCRIPT\n"
             "exit %d\n",
             log, stream, transcript, exit_code);
    return fs_write_file(path, script) && chmod(path, 0755) == 0;
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
static bool write_resolver(const char *path) {
    static const char *const script =
        "#!/bin/sh\n"
        "case \"$1\" in\n"
        "  --exists) [ \"$2\" = \"toykit\" ] && exit 0 || exit 1 ;;\n"
        "  --modversion) echo 1.2.3 ;;\n"
        "  --cflags) echo '-I/opt/toykit/include -pthread -D_REENTRANT' ;;\n"
        "esac\n"
        "exit 0\n";
    return fs_write_file(path, script) && chmod(path, 0755) == 0;
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
       so a file that was only touched is correctly left alone. */
    ASSERT_TRUE(write_file(fixture.root, "src/main.c", "int main(void){return 1;}\n"));

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
    char script[1024];
    snprintf(script, sizeof script,
             "#!/bin/sh\n"
             "echo \"$@\" >> %s\n"
             "src=''; dep=''; prev=''\n"
             "for a in \"$@\"; do\n"
             "  case \"$a\" in *.c) src=\"$a\";; esac\n"
             "  [ \"$prev\" = '-MF' ] && dep=\"$a\"\n"
             "  prev=\"$a\"\n"
             "done\n"
             "[ -n \"$dep\" ] && [ -n \"$src\" ] && echo \"out.o: $src\" > \"$dep\"\n"
             "echo 'int changed_while_running(void);' >> \"$src\"\n"
             "echo '%s' >&2\n",
             fixture.log, "src/main.c:1:16: warning: unused variable 'x' [-Wunused-variable]");
    ASSERT_TRUE(fs_write_file(fixture.compiler, script));
    ASSERT_TRUE(chmod(fixture.compiler, 0755) == 0);

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
