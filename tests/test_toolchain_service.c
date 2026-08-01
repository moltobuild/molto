#include <moltest.h>

#include <molto/exit_code.h>
#include <molto/project/project_ctx.h>
#include <molto/services/fs_service.h>
#include <molto/services/toolchain_service.h>
#include <molto/workspace/wsdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A stand-in for pickup, so the tests exercise Molto's side of the contract
   without depending on which compilers this machine happens to have. Every
   invocation appends a line to a log, which is how "did it ask again?" is
   answered. */
typedef struct {
    char root[64];
    char program[128];
    char log[128];
    char previous[4096];
    bool had_previous;
} pickup_stub;

static bool stub_write(const pickup_stub *stub, const char *answer, int exit_code) {
    char script[2048];
    snprintf(script, sizeof script,
             "#!/bin/sh\n"
             "echo called >> %s\n"
             "cat <<'ANSWER'\n%s\nANSWER\n"
             "exit %d\n",
             stub->log, answer, exit_code);
    if (!fs_write_file(stub->program, script))
        return false;
    return chmod(stub->program, 0755) == 0;
}

static bool stub_setup(pickup_stub *stub, const char *answer, int exit_code) {
    snprintf(stub->root, sizeof stub->root, "%s", "/tmp/molto_pickup_XXXXXX");
    if (mkdtemp(stub->root) == NULL)
        return false;
    snprintf(stub->program, sizeof stub->program, "%s/pickup", stub->root);
    snprintf(stub->log, sizeof stub->log, "%s/calls", stub->root);

    const char *existing = getenv("MOLTO_PICKUP");
    stub->had_previous = existing != NULL;
    if (existing != NULL)
        snprintf(stub->previous, sizeof stub->previous, "%s", existing);

    return stub_write(stub, answer, exit_code)
        && setenv("MOLTO_PICKUP", stub->program, 1) == 0
        && unsetenv("C_COMPILER") == 0;
}

static void stub_teardown(pickup_stub *stub) {
    if (stub->had_previous)
        (void)setenv("MOLTO_PICKUP", stub->previous, 1);
    else
        (void)unsetenv("MOLTO_PICKUP");
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", stub->root);
    (void)system(cmd);
}

static int stub_calls(const pickup_stub *stub) {
    char *log = fs_read_file(stub->log);
    if (log == NULL)
        return 0;
    int calls = 0;
    for (const char *c = log; *c != '\0'; c++)
        calls += *c == '\n' ? 1 : 0;
    free(log);
    return calls;
}

/* A workspace to hold the database the answer is cached in. */
static bool workspace_setup(char *root, size_t root_size) {
    snprintf(root, root_size, "%s", "/tmp/molto_tc_ws_XXXXXX");
    return mkdtemp(root) != NULL;
}

static void workspace_teardown(const char *root) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

/* An answer naming a program that certainly exists, since the resolved
   compiler is tracked as a file. */
#define STUB_ANSWER \
    "[compiler]\n" \
    "path = \"/bin/sh\"\n" \
    "c_path = \"/bin/sh\"\n" \
    "cxx_path = \"/bin/echo\"\n" \
    "vendor = \"gcc\"\n" \
    "version = \"12.3.0\"\n"

static project_target target_requiring(const char *feature) {
    project_target target;
    memset(&target, 0, sizeof target);
    snprintf(target.std, sizeof target.std, "%s", "c2x");
    if (feature != NULL) {
        snprintf(target.requires[0], PROJECT_OPT_LEN, "%s", feature);
        target.requires_count = 1;
    }
    return target;
}

MOLTEST(toolchain_reads_the_answer_from_the_resolver) {
    pickup_stub stub;
    ASSERT_TRUE(stub_setup(&stub, STUB_ANSWER, 0));
    char root[64];
    ASSERT_TRUE(workspace_setup(root, sizeof root));

    wsdb *db = wsdb_open(root);
    ASSERT_NOT_NULL(db);
    project_target target = target_requiring("attr_nodiscard");
    resolved_toolchain chain;
    ASSERT_EQ(exit_ok, toolchain_resolve(&target, false, db, false, &chain));

    EXPECT_STREQ("/bin/sh", chain.cc);
    EXPECT_STREQ("/bin/echo", chain.cxx);
    EXPECT_STREQ("gcc", chain.vendor);
    EXPECT_STREQ("12.3.0", chain.version);
    EXPECT_EQ(1, stub_calls(&stub));

    (void)wsdb_close(db);
    workspace_teardown(root);
    stub_teardown(&stub);
}

MOLTEST(toolchain_is_asked_once_and_then_remembered) {
    pickup_stub stub;
    ASSERT_TRUE(stub_setup(&stub, STUB_ANSWER, 0));
    char root[64];
    ASSERT_TRUE(workspace_setup(root, sizeof root));

    project_target target = target_requiring("attr_nodiscard");
    resolved_toolchain chain;

    wsdb *db = wsdb_open(root);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(exit_ok, toolchain_resolve(&target, false, db, false, &chain));
    ASSERT_EQ(1, stub_calls(&stub));

    /* Same request, same database: the recorded answer is used and no process
       is spawned. This is the whole point of caching it. */
    ASSERT_EQ(exit_ok, toolchain_resolve(&target, false, db, false, &chain));
    EXPECT_EQ(1, stub_calls(&stub));
    EXPECT_STREQ("/bin/sh", chain.cc);

    /* It survives closing and reopening the workspace. */
    (void)wsdb_close(db);
    db = wsdb_open(root);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(exit_ok, toolchain_resolve(&target, false, db, false, &chain));
    EXPECT_EQ(1, stub_calls(&stub));

    (void)wsdb_close(db);
    workspace_teardown(root);
    stub_teardown(&stub);
}

MOLTEST(toolchain_is_asked_again_when_the_request_changes) {
    pickup_stub stub;
    ASSERT_TRUE(stub_setup(&stub, STUB_ANSWER, 0));
    char root[64];
    ASSERT_TRUE(workspace_setup(root, sizeof root));

    wsdb *db = wsdb_open(root);
    ASSERT_NOT_NULL(db);
    resolved_toolchain chain;

    project_target target = target_requiring("attr_nodiscard");
    ASSERT_EQ(exit_ok, toolchain_resolve(&target, false, db, false, &chain));
    ASSERT_EQ(1, stub_calls(&stub));

    /* Requiring something else is a different question, so it gets asked. */
    project_target stricter = target_requiring("typeof");
    ASSERT_EQ(exit_ok, toolchain_resolve(&stricter, false, db, false, &chain));
    EXPECT_EQ(2, stub_calls(&stub));

    /* So is asking for C++ rather than C. */
    ASSERT_EQ(exit_ok, toolchain_resolve(&stricter, true, db, false, &chain));
    EXPECT_EQ(3, stub_calls(&stub));

    /* And an explicit refresh asks even when nothing changed. */
    ASSERT_EQ(exit_ok, toolchain_resolve(&stricter, true, db, true, &chain));
    EXPECT_EQ(4, stub_calls(&stub));

    (void)wsdb_close(db);
    workspace_teardown(root);
    stub_teardown(&stub);
}

MOLTEST(toolchain_reports_when_nothing_satisfies_the_request) {
    /* Exit code 3 is pickup's "nothing matches", which is an answer rather
       than a malfunction and must not be mistaken for one. */
    pickup_stub stub;
    ASSERT_TRUE(stub_setup(&stub, "", 3));
    char root[64];
    ASSERT_TRUE(workspace_setup(root, sizeof root));

    wsdb *db = wsdb_open(root);
    ASSERT_NOT_NULL(db);
    project_target target = target_requiring("attr_nodiscard");
    resolved_toolchain chain;
    EXPECT_EQ(exit_build_failure, toolchain_resolve(&target, false, db, false, &chain));

    (void)wsdb_close(db);
    workspace_teardown(root);
    stub_teardown(&stub);
}

MOLTEST(toolchain_reports_a_resolver_that_cannot_run) {
    pickup_stub stub;
    ASSERT_TRUE(stub_setup(&stub, STUB_ANSWER, 0));
    ASSERT_TRUE(setenv("MOLTO_PICKUP", "/nonexistent/pickup", 1) == 0);

    project_target target = target_requiring("attr_nodiscard");
    resolved_toolchain chain;
    EXPECT_EQ(exit_build_failure, toolchain_resolve(&target, false, NULL, false, &chain));

    stub_teardown(&stub);
}

MOLTEST(toolchain_reports_an_unreadable_answer) {
    pickup_stub stub;
    ASSERT_TRUE(stub_setup(&stub, "this is not toml", 0));

    project_target target = target_requiring("attr_nodiscard");
    resolved_toolchain chain;
    EXPECT_EQ(exit_build_failure, toolchain_resolve(&target, false, NULL, false, &chain));

    stub_teardown(&stub);
}

MOLTEST(toolchain_lets_the_environment_override_the_resolver) {
    pickup_stub stub;
    ASSERT_TRUE(stub_setup(&stub, STUB_ANSWER, 0));

    /* Setting the compiler by hand is a deliberate choice, so it wins and the
       resolver is not consulted at all. */
    ASSERT_TRUE(setenv("C_COMPILER", "/usr/bin/some-cc", 1) == 0);
    ASSERT_TRUE(setenv("CPP_COMPILER", "/usr/bin/some-c++", 1) == 0);

    project_target target = target_requiring("attr_nodiscard");
    resolved_toolchain chain;
    ASSERT_EQ(exit_ok, toolchain_resolve(&target, false, NULL, false, &chain));
    EXPECT_STREQ("/usr/bin/some-cc", chain.cc);
    EXPECT_STREQ("/usr/bin/some-c++", chain.cxx);
    EXPECT_EQ(0, stub_calls(&stub));

    (void)unsetenv("C_COMPILER");
    (void)unsetenv("CPP_COMPILER");
    stub_teardown(&stub);
}
