#include <moltest.h>

#include <molto/services/host_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The resolver is a process, so these drive a stub through MOLTO_PKG_CONFIG
   rather than requiring pkg-config and a particular library on the machine
   running them. What is under test is how molto reads an answer, not whether
   this host has GTK. */

typedef struct {
    char dir[64];
    char tool[128];
} stub;

static bool stub_open(stub *at, const char *script) {
    snprintf(at->dir, sizeof at->dir, "%s", "/tmp/molto_host_XXXXXX");
    if (mkdtemp(at->dir) == NULL)
        return false;
    snprintf(at->tool, sizeof at->tool, "%s/pkg-config", at->dir);

    FILE *file = fopen(at->tool, "w");
    if (file == NULL)
        return false;
    fputs(script, file);
    if (fclose(file) != 0)
        return false;
    return chmod(at->tool, 0755) == 0 && setenv("MOLTO_PKG_CONFIG", at->tool, 1) == 0;
}

static void stub_close(const stub *at) {
    (void)unsetenv("MOLTO_PKG_CONFIG");
    (void)remove(at->tool);
    (void)rmdir(at->dir);
}

/* Answers like pkg-config does for a toolkit: many include directories, a
   handful of libraries, and options that are neither. */
static const char *const ANSWERS =
    "#!/bin/sh\n"
    "case \"$1\" in\n"
    "  --exists) [ \"$2\" = \"toykit\" ] && exit 0 || exit 1 ;;\n"
    "  --modversion) echo 3.24.33 ;;\n"
    "  --cflags) echo '-I/opt/toykit/include -I/opt/toykit/lib/include -pthread -D_REENTRANT'\n"
    "            echo '-L/opt/toykit/lib -Wl,-rpath,/opt/toykit/lib -Wl,--enable-new-dtags'\n"
    "            echo '-ltoykit -lm' ;;\n"
    "esac\n"
    "exit 0\n";

MOLTEST(a_host_capability_answers_with_its_includes_and_links) {
    stub at;
    ASSERT_TRUE(stub_open(&at, ANSWERS));

    host_answer answer;
    char err[512] = "";
    ASSERT_TRUE(host_resolve("toykit", &answer, err, sizeof err));

    ASSERT_EQ(2u, answer.include_count);
    EXPECT_STREQ("/opt/toykit/include", answer.includes[0]);
    EXPECT_STREQ("/opt/toykit/lib/include", answer.includes[1]);

    /* As they reach the link line, and `-L` kept beside `-l`: a library outside
       the linker's default path needs both. */
    ASSERT_EQ(4u, answer.link_count);
    EXPECT_STREQ("-L/opt/toykit/lib", answer.links[0]);
    EXPECT_STREQ("-Wl,-rpath,/opt/toykit/lib", answer.links[1]);
    EXPECT_STREQ("-ltoykit", answer.links[2]);

    EXPECT_STREQ("3.24.33", answer.version);
    stub_close(&at);
}

/* `-pthread` and `-D_REENTRANT` are in that answer and must not be in the
   build: a resolver contributing a define would decide a consumer's ABI from
   outside the manifest anyone reviewed. */
MOLTEST(a_host_answer_contributes_nothing_but_includes_and_links) {
    stub at;
    ASSERT_TRUE(stub_open(&at, ANSWERS));

    host_answer answer;
    char err[512] = "";
    ASSERT_TRUE(host_resolve("toykit", &answer, err, sizeof err));

    for (size_t i = 0; i < answer.link_count; i++) {
        EXPECT_NULL(strstr(answer.links[i], "-pthread"));
        EXPECT_NULL(strstr(answer.links[i], "-D"));
    }
    stub_close(&at);
}

/* "Not installed" and "no resolver" send a reader to different places, so they
   are different messages. */
MOLTEST(a_capability_the_resolver_does_not_know_says_so) {
    stub at;
    ASSERT_TRUE(stub_open(&at, ANSWERS));

    host_answer answer;
    char err[512] = "";
    EXPECT_FALSE(host_resolve("absent", &answer, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "absent"));
    EXPECT_NOT_NULL(strstr(err, "not installed"));
    stub_close(&at);
}

MOLTEST(a_missing_resolver_is_reported_as_the_machine_and_not_the_package) {
    ASSERT_EQ(0, setenv("MOLTO_PKG_CONFIG", "/nonexistent/pkg-config", 1));

    host_answer answer;
    char err[512] = "";
    EXPECT_FALSE(host_resolve("toykit", &answer, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "could not be run"));
    EXPECT_NOT_NULL(strstr(err, "MOLTO_PKG_CONFIG"));

    (void)unsetenv("MOLTO_PKG_CONFIG");
}

MOLTEST(a_capability_with_no_name_is_refused) {
    host_answer answer;
    char err[512] = "";
    EXPECT_FALSE(host_resolve("", &answer, err, sizeof err));
}

/* A library outside the default path is found at link time by `-L` and at run
   time by `-rpath`, and they are one fact spelled twice. Keeping only the first
   produces a binary that links and then cannot start — which is what SDL3 in a
   custom prefix did before this. */
MOLTEST(a_host_answer_keeps_the_rpath_that_makes_its_library_runnable) {
    stub at;
    ASSERT_TRUE(stub_open(&at, ANSWERS));

    host_answer answer;
    char err[512] = "";
    ASSERT_TRUE(host_resolve("toykit", &answer, err, sizeof err));

    bool found = false;
    for (size_t i = 0; i < answer.link_count; i++) {
        if (strcmp(answer.links[i], "-Wl,-rpath,/opt/toykit/lib") == 0)
            found = true;
        /* And nothing else that rides in on -Wl,: it is otherwise an escape
           hatch onto the whole linker, and a resolver is not one. */
        EXPECT_NULL(strstr(answer.links[i], "--enable-new-dtags"));
    }
    EXPECT_TRUE(found);
    stub_close(&at);
}
