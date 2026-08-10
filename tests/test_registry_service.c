#include <moltest.h>

#include <molto/services/process_service.h>
#include <molto/services/registry_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The read half of the registry client.
 *
 * A body-less GET is a shape the request path had never built before — it
 * always pushed a body flag and a content type — so what is worth proving here
 * is that curl is handed a valid invocation and that the answer comes back
 * whole. The decisions made *about* an answer live in resolve_service, and are
 * tested there against canned bodies with no network at all. */

#define PORT "8731"
#define BASE_URL "http://127.0.0.1:" PORT

static const char *const RELEASE_JSON =
    "{\"kind\":\"package\",\"name\":\"sqlite\",\"version\":\"3.53.4\",\"targets\":[]}";

MOLTEST(registry_get_reports_a_registry_it_cannot_reach) {
    /* Port 1 refuses immediately, so this exercises the transport-failure
       branch without waiting for a timeout. A malformed argv would fail here
       too, but differently: curl would complain about its own options rather
       than about the connection. */
    registry_response response;
    char err[256] = "";

    EXPECT_FALSE(registry_get("http://127.0.0.1:1", "/v1/packages", &response, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "registry"));
}

/* A real HTTP server, because file:// gives curl an http_code of 000 and the
   client correctly refuses that as "the registry did not answer with a
   status". Returns the child's pid, or -1 when python3 is not installed. */
static long serve_fixture(const char *directory) {
    const char *check[] = { "python3", "--version", NULL };
    char version[64] = "";
    if (process_capture(check, version, sizeof version) != 0)
        return -1;

    const long pid = (long)fork();
    if (pid != 0)
        return pid;

    /* The child: serve quietly and never return. */
    (void)freopen("/dev/null", "w", stdout);
    (void)freopen("/dev/null", "w", stderr);
    execlp("python3", "python3", "-m", "http.server", PORT, "--directory", directory,
           "--bind", "127.0.0.1", (char *)NULL);
    _exit(127);
}

/* Waits for the server to answer, so the test does not race its startup. */
static bool wait_for_server(void) {
    for (int attempt = 0; attempt < 50; attempt++) {
        registry_response response;
        char err[256] = "";
        if (registry_get(BASE_URL, "/", &response, err, sizeof err))
            return true;
        usleep(100000);
    }
    return false;
}

MOLTEST(registry_get_reads_a_body_and_its_status) {
    char directory[] = "/tmp/molto_registry_XXXXXX";
    ASSERT_NOT_NULL(mkdtemp(directory));

    /* The path is a real directory tree, so the coordinate in the URL is the
       coordinate on disk. */
    char nested[256];
    char fixture[320];
    snprintf(nested, sizeof nested, "%s/v1/packages/sqlite", directory);
    char command[512];
    snprintf(command, sizeof command, "mkdir -p '%s'", nested);
    const char *mkdir_argv[] = { "sh", "-c", command, NULL };
    ASSERT_EQ(0, process_run(mkdir_argv));

    snprintf(fixture, sizeof fixture, "%s/3.53.4", nested);
    FILE *file = fopen(fixture, "w");
    ASSERT_NOT_NULL(file);
    fputs(RELEASE_JSON, file);
    ASSERT_EQ(0, fclose(file));

    const long pid = serve_fixture(directory);
    if (pid < 0) {
        SKIP("python3 is not installed, so there is no server to read from");
        return;
    }

    if (wait_for_server()) {
        registry_response response;
        char err[256] = "";
        EXPECT_TRUE(registry_get(BASE_URL, "/v1/packages/sqlite/3.53.4", &response, err,
                                 sizeof err));
        EXPECT_EQ(200L, response.status);
        EXPECT_NOT_NULL(strstr(response.body, "\"version\":\"3.53.4\""));

        /* A 404 is an answer and not a failure: the caller decides what an
           absent coordinate means. */
        EXPECT_TRUE(registry_get(BASE_URL, "/v1/packages/nothing", &response, err, sizeof err));
        EXPECT_EQ(404L, response.status);
    }

    char kill_command[64];
    snprintf(kill_command, sizeof kill_command, "kill %ld", pid);
    const char *kill_argv[] = { "sh", "-c", kill_command, NULL };
    (void)process_run(kill_argv);

    char clean[512];
    snprintf(clean, sizeof clean, "rm -rf '%s'", directory);
    const char *clean_argv[] = { "sh", "-c", clean, NULL };
    (void)process_run(clean_argv);
}
