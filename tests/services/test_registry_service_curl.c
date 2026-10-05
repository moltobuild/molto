#include <moltest.h>
#include <moltest_mock.h>

#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/registry_service.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * What registry_service makes of curl, with curl faked.
 *
 * An isolated test (RFC-0021): it replaces src/services/process_service.c, so
 * process_capture_all() below is the one this binary has. test_registry_service.c
 * talks to a real HTTP server; these are the answers it cannot make on demand
 * (no curl, a dropped connection, an answer too large) and the promises about
 * the credential that only the command line shows: never an argument, never
 * left behind, never sent to object storage.
 */

MOCK_VALUE_FUNC(int, process_capture_all, const char *const *, const process_env_var *, size_t,
                char *, size_t, bool *);

/* How registry_service marks the status after a body (its STATUS_MARKER). */
#define STATUS(code) "\n@molto-status:" #code

/* What curl prints, the command it was run with, and the credential file it
   was pointed at as it was during the call. argv and the file live only for
   the call, so both are copied then rather than read after. */
static const char *answer;
static int exit_code;
static bool answer_truncated;
static char ran[2048];
static char config_path[512];
static char config_text[512];
static bool config_private;

static int curl_answers(const char *const *argv, const process_env_var *env, size_t env_count,
                        char *out, size_t out_size, bool *truncated) {
    (void)env, (void)env_count;
    ran[0] = config_path[0] = config_text[0] = '\0';
    config_private = false;
    for(size_t i = 0; argv[i] != NULL; i++) {
        const size_t used = strlen(ran);
        snprintf(ran + used, sizeof ran - used, "%s%s", i > 0 ? " " : "", argv[i]);
        if(strcmp(argv[i], "--config") == 0 && argv[i + 1] != NULL) {
            snprintf(config_path, sizeof config_path, "%s", argv[i + 1]);
            char *text = fs_read_file(argv[i + 1]);
            if(text != NULL)
                snprintf(config_text, sizeof config_text, "%s", text);
            free(text);
#ifndef _WIN32
            struct stat info;
            config_private =
                stat(argv[i + 1], &info) == 0 && (info.st_mode & 0777) == (S_IRUSR | S_IWUSR);
#else
            config_private = true;
#endif
        }
    }
    snprintf(out, out_size, "%s", answer != NULL ? answer : "");
    if(truncated != NULL)
        *truncated = answer_truncated;
    return exit_code;
}

BEFORE_EACH() {
    answer = NULL;
    exit_code = 0;
    answer_truncated = false;
    process_capture_all_mock.custom_fake = curl_answers;
}

static registry_response response;
static char err[512];

static bool get(void) {
    err[0] = '\0';
    return registry_get("https://registry.example", "/v1/packages/zlib", &response, err,
                        sizeof err);
}

/* --- curl itself --- */

DESCRIBE(a_missing_curl_says_curl_is_not_installed) {
    exit_code = 127;
    EXPECT_FALSE(get());
    EXPECT_NOT_NULL(strstr(err, "curl is not installed"));
}

DESCRIBE(a_dropped_connection_quotes_curl) {
    exit_code = 6;
    answer = "curl: (6) Could not resolve host: registry.example";
    EXPECT_FALSE(get());
    EXPECT_NOT_NULL(strstr(err, "could not reach the registry"));
    EXPECT_NOT_NULL(strstr(err, "Could not resolve host"));
}

DESCRIBE(an_answer_too_large_to_read_is_refused) {
    answer = "{}" STATUS(200);
    answer_truncated = true;
    EXPECT_FALSE(get());
    EXPECT_NOT_NULL(strstr(err, "too large"));
}

DESCRIBE(an_answer_without_a_status_is_refused) {
    answer = "<html>proxy error</html>";
    EXPECT_FALSE(get());
    EXPECT_NOT_NULL(strstr(err, "did not answer with a status"));
}

DESCRIBE(a_read_is_public_and_sends_no_credential) {
    answer = "{\"name\":\"zlib\"}" STATUS(200);
    ASSERT_TRUE(get());
    EXPECT_EQ(200, (int)response.status);
    EXPECT_STREQ("{\"name\":\"zlib\"}", response.body);
    EXPECT_STREQ("curl --silent --show-error --request GET --write-out "
                 "\n@molto-status:%{http_code} https://registry.example/v1/packages/zlib",
                 ran);
}

/* --- the credential --- */

DESCRIBE(a_token_is_never_an_argument_and_never_left_behind) {
    /* Every process on the machine can read another's argv. The token goes in
       a file only its owner can read, for the length of the call. */
    answer = "{}" STATUS(201);
    err[0] = '\0';
    ASSERT_TRUE(registry_upload_blob("https://registry.example", "s3cr3t-token", "/v1/blob",
                                     "/tmp/pkg.tar.gz", "sha256:abc", &response, err,
                                     sizeof err));
    EXPECT_NULL(strstr(ran, "s3cr3t-token"));
    EXPECT_NOT_NULL(strstr(ran, "--config"));
    EXPECT_STREQ("header = \"authorization: Bearer s3cr3t-token\"\n", config_text);
    EXPECT_TRUE(config_private);
    EXPECT_FALSE(fs_path_exists(config_path));
    EXPECT_NOT_NULL(strstr(ran, "--request PUT"));
    EXPECT_NOT_NULL(strstr(ran, "x-molto-checksum: sha256:abc"));
    EXPECT_NOT_NULL(strstr(ran, "--data-binary @/tmp/pkg.tar.gz"));
}

DESCRIBE(a_failed_request_leaves_no_credential_behind_either) {
    exit_code = 7;
    answer = "curl: (7) Failed to connect";
    registry_response out;
    EXPECT_FALSE(registry_publish_recipe("https://registry.example", "s3cr3t-token", "/v1/r",
                                         "/tmp/recipe.toml", &out, err, sizeof err));
    EXPECT_STRNE("", config_path);
    EXPECT_FALSE(fs_path_exists(config_path));
}

DESCRIBE(a_signed_upload_never_carries_the_registrys_credential) {
    /* Object storage is a third party: the signature is its credential, and
       molto's token has no business there. */
    registry_signed_upload upload = {.header_count = 1};
    snprintf(upload.url, sizeof upload.url, "https://storage.example/blob?sig=abc");
    snprintf(upload.headers[0], sizeof upload.headers[0], "x-amz-checksum: abc");
    answer = "" STATUS(200);
    ASSERT_TRUE(registry_put_signed(&upload, "/tmp/pkg.tar.gz", &response, err, sizeof err));
    EXPECT_NULL(strstr(ran, "--config"));
    EXPECT_NULL(strstr(ran, "Bearer"));
    EXPECT_STREQ("curl --silent --show-error --request PUT --header x-amz-checksum: abc "
                 "--upload-file /tmp/pkg.tar.gz --write-out \n@molto-status:%{http_code} "
                 "https://storage.example/blob?sig=abc",
                 ran);
}

/* --- signing in --- */

static char token[128];

static bool sign_in(void) {
    token[0] = err[0] = '\0';
    return registry_create_token("https://registry.example", "me@example.com", "pw", "laptop",
                                 token, sizeof token, err, sizeof err);
}

DESCRIBE(signing_in_reads_the_issued_token) {
    answer = "{\"token\":\"issued-123\"}" STATUS(201);
    ASSERT_TRUE(sign_in());
    EXPECT_STREQ("issued-123", token);
    EXPECT_NOT_NULL(strstr(ran, "--request POST"));
    EXPECT_NOT_NULL(strstr(ran, "https://registry.example/v1/auth/token"));
}

DESCRIBE(a_refused_sign_in_says_why) {
    answer = "{\"message\":\"wrong email or password\"}" STATUS(401);
    EXPECT_FALSE(sign_in());
    EXPECT_NOT_NULL(strstr(err, "(401)"));
    EXPECT_NOT_NULL(strstr(err, "wrong email or password"));
}

DESCRIBE(a_sign_in_answer_without_a_token_is_refused) {
    answer = "not json" STATUS(201);
    EXPECT_FALSE(sign_in());
    EXPECT_NOT_NULL(strstr(err, "not JSON"));

    answer = "{\"token\":\"\"}" STATUS(201);
    EXPECT_FALSE(sign_in());
    EXPECT_NOT_NULL(strstr(err, "issued no token"));
}

DESCRIBE(credentials_too_long_to_send_are_refused_before_curl) {
    char long_password[1100];
    memset(long_password, 'x', sizeof long_password - 1);
    long_password[sizeof long_password - 1] = '\0';
    EXPECT_FALSE(registry_create_token("https://registry.example", "me@example.com",
                                       long_password, "laptop", token, sizeof token, err,
                                       sizeof err));
    EXPECT_NOT_NULL(strstr(err, "too long"));
    EXPECT_EQ(0, (int)process_capture_all_mock.call_count);
}

/* --- publishing --- */

DESCRIBE(a_recipe_is_published_as_toml) {
    answer = "{}" STATUS(201);
    ASSERT_TRUE(registry_publish_recipe("https://registry.example", "t", "/v1/recipes",
                                        "/tmp/recipe.toml", &response, err, sizeof err));
    EXPECT_NOT_NULL(strstr(ran, "--request POST"));
    EXPECT_NOT_NULL(strstr(ran, "content-type: application/toml"));
    EXPECT_NOT_NULL(strstr(ran, "--data-binary @/tmp/recipe.toml"));
}

DESCRIBE(a_refused_presign_explains_itself) {
    answer = "{\"message\":\"version already published\"}" STATUS(409);
    registry_signed_upload upload;
    bool supported = false;
    EXPECT_FALSE(registry_presign_blob("https://registry.example", "t", "/v1/presign", "abc",
                                       &upload, &supported, err, sizeof err));
    EXPECT_TRUE(supported);
    EXPECT_NOT_NULL(strstr(err, "(409)"));
    EXPECT_NOT_NULL(strstr(err, "version already published"));
}

DESCRIBE(a_presign_answer_that_is_not_json_is_refused) {
    answer = "<html/>" STATUS(201);
    registry_signed_upload upload;
    bool supported = false;
    EXPECT_FALSE(registry_presign_blob("https://registry.example", "t", "/v1/presign", "abc",
                                       &upload, &supported, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "not JSON"));
}

/* --- explaining an answer --- */

DESCRIBE(an_answer_is_explained_by_its_message_or_its_body) {
    char detail[128];
    registry_response json = {.status = 400};
    snprintf(json.body, sizeof json.body, "{\"message\":\"bad name\"}");
    registry_explain(&json, detail, sizeof detail);
    EXPECT_STREQ("bad name", detail);

    registry_response plain = {.status = 502};
    snprintf(plain.body, sizeof plain.body, "Bad Gateway");
    registry_explain(&plain, detail, sizeof detail);
    EXPECT_STREQ("Bad Gateway", detail);
}
