#include <moltest.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/tool_service.h>
#include <molto/workspace/wsdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A stand-in for pickup, so these exercise Molto's side of the contract without
   depending on which tools this machine happens to have. Every invocation
   appends a line to a log, which is how "did it ask again?" is answered. */
typedef struct {
    char root[64];
    char program[128];
    char log[128];
    char previous[4096];
    bool had_previous;
} pickup_stub;

/* The answer `pickup tools --format toml` gives. The paths name programs that
   certainly exist, since a resolved tool is tracked as a file. */
#define STUB_ANSWER \
    "[[tool]]\n" \
    "kind = \"formatter\"\n" \
    "name = \"clang-format\"\n" \
    "path = \"/bin/sh\"\n" \
    "version = \"clang-format version 22.1.8\"\n" \
    "source = \"pickup\"\n" \
    "\n" \
    "[[tool]]\n" \
    "kind = \"linter\"\n" \
    "name = \"clang-tidy\"\n" \
    "path = \"/bin/cat\"\n" \
    "version = \"LLVM version 22.1.8\"\n" \
    "source = \"pickup\"\n"

static bool stub_setup(pickup_stub *stub, const char *answer) {
    if (!moltest_temp_dir("molto_tools", stub->root, sizeof stub->root))
        return false;
    snprintf(stub->program, sizeof stub->program, "%s/pickup", stub->root);
    snprintf(stub->log, sizeof stub->log, "%s/calls", stub->root);

    const char *existing = getenv("MOLTO_PICKUP");
    stub->had_previous = existing != NULL;
    if (existing != NULL)
        snprintf(stub->previous, sizeof stub->previous, "%s", existing);

    char script[2048];
    snprintf(script, sizeof script,
             "#!/bin/sh\n"
             "echo called >> %s\n"
             "cat <<'ANSWER'\n%s\nANSWER\n",
             stub->log, answer);
    return fs_write_file(stub->program, script)
        && chmod(stub->program, 0755) == 0
        && setenv("MOLTO_PICKUP", stub->program, 1) == 0
        && unsetenv("MOLTO_CLANG_FORMAT") == 0
        && unsetenv("MOLTO_CLANG_TIDY") == 0;
}

static void stub_teardown(pickup_stub *stub) {
    if (stub->had_previous)
        (void)setenv("MOLTO_PICKUP", stub->previous, 1);
    else
        (void)unsetenv("MOLTO_PICKUP");
    char cmd[128];
    (void)fs_remove_tree(stub->root);
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

static bool workspace_setup(char *root, size_t root_size) {
    return moltest_temp_dir("molto_tools_ws", root, root_size);
}

static void workspace_teardown(const char *root) {
    char cmd[128];
    (void)fs_remove_tree(root);
}

MOLTEST(tool_resolve_reads_the_answer_of_pickup_tools) {
    pickup_stub stub;
    ASSERT_TRUE(stub_setup(&stub, STUB_ANSWER));

    /* Two tools in one answer: each kind has to find its own, not the first. */
    resolved_tool formatter;
    ASSERT_EQ(exit_ok, tool_resolve(tool_kind_formatter, NULL, false, &formatter));
    EXPECT_STREQ("clang-format", formatter.name);
    EXPECT_STREQ("/bin/sh", formatter.path);
    EXPECT_STREQ("clang-format version 22.1.8", formatter.version);

    resolved_tool linter;
    ASSERT_EQ(exit_ok, tool_resolve(tool_kind_linter, NULL, false, &linter));
    EXPECT_STREQ("clang-tidy", linter.name);
    EXPECT_STREQ("/bin/cat", linter.path);

    stub_teardown(&stub);
}

MOLTEST(tool_resolve_remembers_the_answer_in_the_workspace_database) {
    pickup_stub stub;
    char root[64];
    ASSERT_TRUE(stub_setup(&stub, STUB_ANSWER));
    ASSERT_TRUE(workspace_setup(root, sizeof root));

    wsdb *db = wsdb_open(root);
    ASSERT_NOT_NULL(db);

    resolved_tool first;
    ASSERT_EQ(exit_ok, tool_resolve(tool_kind_linter, db, false, &first));
    EXPECT_EQ(1, stub_calls(&stub));

    /* The second command reuses what the first learned: pickup costs a fork and
       two --version probes, and the answer does not change between them. */
    resolved_tool again;
    ASSERT_EQ(exit_ok, tool_resolve(tool_kind_linter, db, false, &again));
    EXPECT_EQ(1, stub_calls(&stub));
    EXPECT_STREQ(first.path, again.path);

    wsdb_close(db);
    workspace_teardown(root);
    stub_teardown(&stub);
}

MOLTEST(tool_resolve_asks_again_when_refresh_is_set) {
    pickup_stub stub;
    char root[64];
    ASSERT_TRUE(stub_setup(&stub, STUB_ANSWER));
    ASSERT_TRUE(workspace_setup(root, sizeof root));

    wsdb *db = wsdb_open(root);
    ASSERT_NOT_NULL(db);

    resolved_tool tool;
    ASSERT_EQ(exit_ok, tool_resolve(tool_kind_formatter, db, false, &tool));
    ASSERT_EQ(exit_ok, tool_resolve(tool_kind_formatter, db, true, &tool));
    EXPECT_EQ(2, stub_calls(&stub));

    wsdb_close(db);
    workspace_teardown(root);
    stub_teardown(&stub);
}

MOLTEST(tool_resolve_prefers_the_environment_override_without_caching_it) {
    pickup_stub stub;
    ASSERT_TRUE(stub_setup(&stub, STUB_ANSWER));
    ASSERT_TRUE(setenv("MOLTO_CLANG_TIDY", "/usr/local/bin/my-tidy", 1) == 0);

    resolved_tool tool;
    ASSERT_EQ(exit_ok, tool_resolve(tool_kind_linter, NULL, false, &tool));
    EXPECT_STREQ("/usr/local/bin/my-tidy", tool.path);
    /* Bypassing resolution means pickup is never asked at all. */
    EXPECT_EQ(0, stub_calls(&stub));

    /* The other kind still resolves normally. */
    ASSERT_EQ(exit_ok, tool_resolve(tool_kind_formatter, NULL, false, &tool));
    EXPECT_STREQ("/bin/sh", tool.path);

    (void)unsetenv("MOLTO_CLANG_TIDY");
    stub_teardown(&stub);
}

MOLTEST(tool_resolve_reports_a_machine_with_no_tool_of_that_kind) {
    pickup_stub stub;
    /* Pickup ran and answered; there is simply no formatter here. That is a
       fact to act on — lint still has the compiler — not a malfunction. */
    ASSERT_TRUE(stub_setup(&stub,
        "[[tool]]\n"
        "kind = \"linter\"\n"
        "name = \"clang-tidy\"\n"
        "path = \"/bin/cat\"\n"));

    resolved_tool tool;
    EXPECT_EQ(exit_dependency_failure,
              tool_resolve(tool_kind_formatter, NULL, false, &tool));

    stub_teardown(&stub);
}

MOLTEST(tool_resolve_reports_a_resolver_it_could_not_run) {
    const char *previous = getenv("MOLTO_PICKUP");
    char saved[4096] = "";
    if (previous != NULL)
        snprintf(saved, sizeof saved, "%s", previous);
    ASSERT_TRUE(setenv("MOLTO_PICKUP", "/nonexistent/molto_no_pickup_zzz", 1) == 0);
    (void)unsetenv("MOLTO_CLANG_TIDY");

    resolved_tool tool;
    EXPECT_EQ(exit_build_failure, tool_resolve(tool_kind_linter, NULL, false, &tool));

    if (previous != NULL)
        (void)setenv("MOLTO_PICKUP", saved, 1);
    else
        (void)unsetenv("MOLTO_PICKUP");
}

MOLTEST(tool_kind_names_are_what_pickup_reports) {
    /* These strings are the contract with pickup's `kind` field. */
    EXPECT_STREQ("formatter", tool_kind_name(tool_kind_formatter));
    EXPECT_STREQ("linter", tool_kind_name(tool_kind_linter));
}
