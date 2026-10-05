#include <moltest.h>

#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/registry_service.h>
#include <molto/util/thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
   status". False when python3 is not installed, which is a skip and not a
   failure.

   Through the process service rather than fork and exec directly: a child that
   outlives the call starting it is what `process_start` is for, and it takes
   the streams to the platform's null device, which is what the two `freopen`
   calls here used to do by naming a POSIX device. */
static bool serve_fixture(const char *directory, process_handle *out) {
    const char *check[] = { "python3", "--version", NULL };
    char version[64] = "";
    if (process_capture(check, version, sizeof version) != 0)
        return false;

    const char *argv[] = { "python3", "-m",       "http.server", PORT, "--directory",
                           directory, "--bind",   "127.0.0.1",   NULL };
    return process_start(argv, out);
}

/* Waits for the server to answer, so the test does not race its startup. */
static bool wait_for_server(void) {
    for (int attempt = 0; attempt < 50; attempt++) {
        registry_response response;
        char err[256] = "";
        if (registry_get(BASE_URL, "/", &response, err, sizeof err))
            return true;
        thread_sleep_ms(100);
    }
    return false;
}

MOLTEST(registry_get_reads_a_body_and_its_status) {
    char directory[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_registry", directory, sizeof directory));

    /* The path is a real directory tree, so the coordinate in the URL is the
       coordinate on disk. */
    char nested[256];
    char fixture[320];
    snprintf(nested, sizeof nested, "%s/v1/packages/sqlite", directory);
    ASSERT_TRUE(fs_make_dirs(nested));

    snprintf(fixture, sizeof fixture, "%s/3.53.4", nested);
    FILE *file = fopen(fixture, "w");
    ASSERT_NOT_NULL(file);
    fputs(RELEASE_JSON, file);
    ASSERT_EQ(0, fclose(file));

    process_handle server;
    if (!serve_fixture(directory, &server))
        SKIP("python3 is not installed, so there is no server to read from");

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

    process_kill(&server);
    (void)fs_remove_tree(directory);
}

/*
 * Asking a registry to sign an upload, and the two ways it can say it will not.
 *
 * The fallback to the upload every registry has always had exists because the
 * signing endpoint is newer than the registries: a deployment that predates it
 * has no route and answers 404, and one that has the route but no credentials
 * answers 501. Both mean "use the other road", and reading only the second
 * turned the first into `the registry refused to sign the upload (404)` — a
 * refusal molto invented, since nothing was refused and the question was never
 * understood.
 */

#define PRESIGN_PORT "8732"
#define PRESIGN_URL "http://127.0.0.1:" PRESIGN_PORT
#define PRESIGN_PATH "/v1/toolchains/llvm-mingw/23.1.0/windows-x86_64/blob/presign"
#define A_DIGEST "aa998685edd4af210ced01c41a19cab9fab4543ab0bcd0f598efcac734e3efbc"

/* A server that answers every request with one status, so a test can be about
   what the client does with it. `http.server` cannot: it answers POST with 501
   of its own, which is one of the two cases and silently the wrong one for the
   other. */
static bool serve_status(const char *directory, const char *status, process_handle *out) {
    const char *check[] = {"python3", "--version", NULL};
    char version[64] = "";
    if(process_capture(check, version, sizeof version) != 0)
        return false;

    char script[MOLTEST_PATH + 32];
    snprintf(script, sizeof script, "%s/server.py", directory);

    FILE *file = fopen(script, "w");
    if(file == NULL)
        return false;
    fputs("import sys\n"
          "from http.server import BaseHTTPRequestHandler, HTTPServer\n"
          "code = int(sys.argv[1])\n"
          "class H(BaseHTTPRequestHandler):\n"
          "    def respond(self):\n"
          "        self.send_response(code)\n"
          "        self.send_header('content-length', '0')\n"
          "        self.end_headers()\n"
          "    do_GET = do_POST = do_PUT = respond\n"
          "    def log_message(self, *a):\n"
          "        pass\n"
          "HTTPServer(('127.0.0.1', int(sys.argv[2])), H).serve_forever()\n",
          file);
    if(fclose(file) != 0)
        return false;

    const char *argv[] = {"python3", script, status, PRESIGN_PORT, NULL};
    return process_start(argv, out);
}

static bool wait_for_presign_server(void) {
    for(int attempt = 0; attempt < 50; attempt++) {
        registry_response response;
        char err[256] = "";
        if(registry_get(PRESIGN_URL, "/", &response, err, sizeof err))
            return true;
        thread_sleep_ms(100);
    }
    return false;
}

/* Both statuses through the same test, because what matters is that they are
   the same answer: not a failure, and not a signed upload either.

   Answers false when there was no server to ask, having recorded the skip
   itself -- a helper cannot use SKIP, which returns from the function it is
   written in, and a test that quietly returns instead is a test that reports
   success for having done nothing. */
static bool presign_against(const char *status) {
    char directory[MOLTEST_PATH];
    if(!moltest_temp_dir("molto_presign", directory, sizeof directory)) {
        moltest_record_skip("no temporary directory to serve from", __FILE__, __LINE__);
        return false;
    }

    process_handle server;
    if(!serve_status(directory, status, &server)) {
        (void)fs_remove_tree(directory);
        moltest_record_skip("python3 is not installed, so there is no registry to ask", __FILE__,
                            __LINE__);
        return false;
    }

    if(wait_for_presign_server()) {
        registry_signed_upload upload;
        bool supported = true;
        char err[256] = "";

        /* True: the question was asked and answered. */
        EXPECT_TRUE(registry_presign_blob(PRESIGN_URL, "a-token", PRESIGN_PATH, A_DIGEST, &upload,
                                          &supported, err, sizeof err));
        /* False: and the answer was no, so the caller takes the other road. */
        EXPECT_FALSE(supported);
        /* Nothing to report, because nothing went wrong. */
        EXPECT_STREQ("", err);
    }

    process_kill(&server);
    (void)fs_remove_tree(directory);
    return true;
}

MOLTEST(presign_treats_a_registry_that_predates_the_endpoint_as_unsigned) {
    (void)presign_against("404");
}

MOLTEST(presign_treats_a_registry_that_cannot_sign_as_unsigned) { (void)presign_against("501"); }

/* Anything else is a real refusal and is reported as one: a registry that says
   the coordinate is taken is not a registry to go around. */
MOLTEST(presign_reports_a_refusal_that_is_about_the_artifact) {
    char directory[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_presign", directory, sizeof directory));

    process_handle server;
    if(!serve_status(directory, "409", &server)) {
        (void)fs_remove_tree(directory);
        SKIP("python3 is not installed, so there is no registry to ask");
    }

    if(wait_for_presign_server()) {
        registry_signed_upload upload;
        bool supported = true;
        char err[256] = "";

        EXPECT_FALSE(registry_presign_blob(PRESIGN_URL, "a-token", PRESIGN_PATH, A_DIGEST, &upload,
                                           &supported, err, sizeof err));
        EXPECT_NOT_NULL(strstr(err, "409"));
    }

    process_kill(&server);
    (void)fs_remove_tree(directory);
}

/*
 * The URL a registry signs, and what molto does with one it cannot use.
 *
 * A registry composes the host of a signed URL out of its own configuration,
 * and a setting carrying a trailing newline signs a host with the newline in
 * it. curl refuses that URL before it opens a socket and says "(3) URL using
 * bad/illegal format", naming neither the URL nor which end configured it,
 * which is a long way to go for one invisible character.
 */
static bool serve_signed_url(const char *directory, const char *url, process_handle *out) {
    const char *check[] = {"python3", "--version", NULL};
    char version[64] = "";
    if(process_capture(check, version, sizeof version) != 0)
        return false;

    char script[MOLTEST_PATH + 32];
    snprintf(script, sizeof script, "%s/signed.py", directory);

    FILE *file = fopen(script, "w");
    if(file == NULL)
        return false;
    /* The URL arrives as an argument and is encoded here, so a test may name
       one holding a newline without writing an escape into this script. */
    fputs("import json, sys\n"
          "from http.server import BaseHTTPRequestHandler, HTTPServer\n"
          "url = sys.argv[1]\n"
          "class H(BaseHTTPRequestHandler):\n"
          "    def respond(self):\n"
          "        body = json.dumps(\n"
          "            {'url': url, 'headers': {'x-amz-checksum-sha256': 'aGk='}}\n"
          "        ).encode()\n"
          "        self.send_response(201)\n"
          "        self.send_header('content-length', str(len(body)))\n"
          "        self.end_headers()\n"
          "        self.wfile.write(body)\n"
          "    do_GET = do_POST = do_PUT = respond\n"
          "    def log_message(self, *a):\n"
          "        pass\n"
          "HTTPServer(('127.0.0.1', int(sys.argv[2])), H).serve_forever()\n",
          file);
    if(fclose(file) != 0)
        return false;

    const char *argv[] = {"python3", script, url, PRESIGN_PORT, NULL};
    return process_start(argv, out);
}

/* Answers false when there was no server to ask, having recorded the skip, for
   the same reason `presign_against` does. */
static bool presign_signing(const char *url, bool usable, const char *expected) {
    char directory[MOLTEST_PATH];
    if(!moltest_temp_dir("molto_signed", directory, sizeof directory)) {
        moltest_record_skip("no temporary directory to serve from", __FILE__, __LINE__);
        return false;
    }

    process_handle server;
    if(!serve_signed_url(directory, url, &server)) {
        (void)fs_remove_tree(directory);
        moltest_record_skip("python3 is not installed, so there is no registry to ask", __FILE__,
                            __LINE__);
        return false;
    }

    if(wait_for_presign_server()) {
        registry_signed_upload upload;
        bool supported = false;
        /* As wide as the buffer `publish` passes, so a message is judged at
           the length it is actually reported at. */
        char err[512] = "";

        const bool read = registry_presign_blob(PRESIGN_URL, "a-token", PRESIGN_PATH, A_DIGEST,
                                                &upload, &supported, err, sizeof err);
        if(usable) {
            EXPECT_TRUE(read);
            EXPECT_TRUE(supported);
            EXPECT_STREQ(url, upload.url);
            EXPECT_STREQ("", err);
        } else {
            EXPECT_FALSE(read);
            EXPECT_NOT_NULL(strstr(err, expected));
            /* The host, quoted back, because it is the evidence and molto has
               nothing else to offer: which of a registry's settings composed
               it is that registry's business and not in this protocol. */
            EXPECT_NOT_NULL(strstr(err, "host"));
        }
    }

    process_kill(&server);
    (void)fs_remove_tree(directory);
    return true;
}

/* The one this was written for: a setting stored with a trailing newline, and
   the newline coming back in the host. */
MOLTEST(presign_refuses_a_signed_url_with_a_newline_in_its_host) {
    (void)presign_signing("https://storage\n.example.com/artifacts/a.tar.gz?"
                          "X-Amz-Signature=deadbeef",
                          false, "control character");
}

/* The other way a host is composed wrong: a whole endpoint pasted in where a
   shorter part of one belonged, which reads as a URL and names nothing. */
MOLTEST(presign_refuses_a_signed_url_whose_host_is_not_a_host) {
    (void)presign_signing("https://https://storage.example.com/artifacts/"
                          "a.tar.gz?X-Amz-Signature=deadbeef",
                          false, "not a host");
}

/* And the shape of a real one, so the check is known to refuse something
   rather than everything. */
MOLTEST(presign_accepts_a_signed_url_it_can_use) {
    (void)presign_signing("https://storage.example.com/"
                          "artifacts/toolchains/llvm-mingw/23.1.0/windows-x86_64.tar.gz?"
                          "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=deadbeef",
                          true, NULL);
}

/* A registry chooses its own storage and RFC-0010 names none of it, so a host
   reached on a port is a host like any other -- and the port is the one branch
   of the check that nothing above walks. */
MOLTEST(presign_accepts_a_signed_url_whose_host_carries_a_port) {
    (void)presign_signing("https://storage.example.com:9000/"
                          "artifacts/toolchains/llvm-mingw/23.1.0/windows-x86_64.tar.gz?"
                          "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=deadbeef",
                          true, NULL);
}
