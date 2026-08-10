#include <molto/services/registry_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/util/json.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Appended to every response so the status can be read back from stdout: curl
   has no other way to report it that does not also need a second invocation. */
#define STATUS_MARKER "\n@molto-status:"

#define URL_MAX 1024

static bool fail(char *err, size_t err_size, const char *message) {
    if(err != NULL && err_size > 0)
        snprintf(err, err_size, "%s", message);
    return false;
}

/* --- the header file curl reads the credential from --- */

/* Created 0600 in $TMPDIR so the token is never an argument: every process on
   the machine can read another's argv, and none can read this. */
static bool write_auth_config(const char *token, char *path, size_t size) {
    const char *dir = getenv("TMPDIR");
    if(dir == NULL || dir[0] == '\0')
        dir = "/tmp";
    if(!fs_format_path(path, size, "%s/molto-auth-%d", dir, (int)getpid()))
        return false;

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, S_IRUSR | S_IWUSR);
    if(fd < 0)
        return false;

    char line[512];
    const int n = snprintf(line, sizeof line, "header = \"authorization: Bearer %s\"\n", token);
    const bool ok = n > 0 && (size_t)n < sizeof line && write(fd, line, (size_t)n) == n;
    return close(fd) == 0 && ok;
}

/* --- running curl --- */

/* Splits curl's output into the body and the status the marker carries. */
static void split_response(char *text, registry_response *out) {
    out->status = 0;
    char *marker = strstr(text, STATUS_MARKER);
    if(marker == NULL) {
        snprintf(out->body, sizeof out->body, "%s", text);
        return;
    }
    *marker = '\0';
    out->status = strtol(marker + strlen(STATUS_MARKER), NULL, 10);
    snprintf(out->body, sizeof out->body, "%s", text);
}

static bool run_curl(const char *const argv[], registry_response *out, char *err, size_t err_size) {
    static char captured[REGISTRY_BODY_MAX + 512];
    bool truncated = false;
    const int code = process_capture_all(argv, NULL, 0, captured, sizeof captured, &truncated);

    if(code == 127)
        return fail(err, err_size, "curl is not installed, and molto needs it to reach a registry");
    if(code != 0) {
        char message[256];
        /* curl's own complaint, clipped: it is one line, and the buffer it
           shares with a body is far larger than any of them. */
        snprintf(message, sizeof message, "curl could not reach the registry: %.180s", captured);
        return fail(err, err_size, message);
    }
    if(truncated)
        return fail(err, err_size, "the registry's answer was too large to read");

    split_response(captured, out);
    if(out->status == 0)
        return fail(err, err_size, "the registry did not answer with a status");
    return true;
}

/* --- requests --- */

/* One request. `body_flag`/`body_value` are curl's own, so a file upload and a
   JSON payload go through the same path. */
static bool request(const char *base_url, const char *token, const char *method, const char *path,
                    const char *content_type, const char *extra_header, const char *body_flag,
                    const char *body_value, registry_response *out, char *err, size_t err_size) {
    char url[URL_MAX];
    if(!fs_format_path(url, sizeof url, "%s%s", base_url, path))
        return fail(err, err_size, "the registry URL is too long");

    char type_header[128] = "";
    if(content_type != NULL)
        snprintf(type_header, sizeof type_header, "content-type: %s", content_type);

    char config[512] = "";
    if(token != NULL && !write_auth_config(token, config, sizeof config))
        return fail(err, err_size, "could not store the credential for curl to read");

    const char *argv[24];
    size_t n = 0;
    argv[n++] = "curl";
    argv[n++] = "--silent";
    argv[n++] = "--show-error";
    argv[n++] = "--request";
    argv[n++] = method;
    /* Both optional so that one request path serves an upload, a JSON payload
       and a plain read: a GET has no body and no content type, and a second
       copy of this invocation existing only to omit two flags would be a
       second place for the credential handling to drift. */
    if(content_type != NULL) {
        argv[n++] = "--header";
        argv[n++] = type_header;
    }
    if(extra_header != NULL) {
        argv[n++] = "--header";
        argv[n++] = extra_header;
    }
    if(token != NULL) {
        argv[n++] = "--config";
        argv[n++] = config;
    }
    if(body_flag != NULL) {
        argv[n++] = body_flag;
        argv[n++] = body_value;
    }
    argv[n++] = "--write-out";
    argv[n++] = STATUS_MARKER "%{http_code}";
    argv[n++] = url;
    argv[n] = NULL;

    const bool ok = run_curl(argv, out, err, err_size);
    if(config[0] != '\0')
        (void)unlink(config);
    return ok;
}

/* --- the three calls molto makes --- */

static bool read_token(const registry_response *response, char *token, size_t token_size, char *err,
                       size_t err_size) {
    json_document *doc = json_parse(response->body);
    if(doc == NULL)
        return fail(err, err_size, "the registry's answer was not JSON");

    const char *value = json_string(json_get(json_root(doc), "token"));
    const bool ok = value != NULL && value[0] != '\0';
    if(ok)
        snprintf(token, token_size, "%s", value);
    json_free(doc);

    return ok ? true : fail(err, err_size, "the registry issued no token");
}

bool registry_get(const char *base_url, const char *path, registry_response *out, char *err,
                  size_t err_size) {
    return request(base_url, NULL, "GET", path, NULL, NULL, NULL, NULL, out, err, err_size);
}

bool registry_create_token(const char *base_url, const char *email, const char *password,
                           const char *token_name, char *token, size_t token_size, char *err,
                           size_t err_size) {
    /* The password is written as JSON on the command line only for the length
       of this call; nothing durable holds it, and what comes back is a token
       that can be revoked. */
    char payload[1024];
    const int n =
        snprintf(payload, sizeof payload, "{\"email\":\"%s\",\"password\":\"%s\",\"name\":\"%s\"}",
                 email, password, token_name);
    if(n < 0 || (size_t)n >= sizeof payload)
        return fail(err, err_size, "those credentials are too long");

    registry_response response;
    if(!request(base_url, NULL, "POST", "/v1/auth/token", "application/json", NULL, "--data",
                payload, &response, err, err_size))
        return false;

    if(response.status != 201) {
        char message[512];
        char detail[256];
        registry_explain(&response, detail, sizeof detail);
        snprintf(message, sizeof message, "the registry refused the sign-in (%ld): %s",
                 response.status, detail);
        return fail(err, err_size, message);
    }
    return read_token(&response, token, token_size, err, err_size);
}

bool registry_upload_blob(const char *base_url, const char *token, const char *path,
                          const char *file, const char *checksum, registry_response *out, char *err,
                          size_t err_size) {
    char body[1088];
    if(!fs_format_path(body, sizeof body, "@%s", file))
        return fail(err, err_size, "the archive path is too long");

    char checksum_header[128];
    snprintf(checksum_header, sizeof checksum_header, "x-molto-checksum: %s", checksum);

    return request(base_url, token, "PUT", path, "application/octet-stream", checksum_header,
                   "--data-binary", body, out, err, err_size);
}

bool registry_publish_recipe(const char *base_url, const char *token, const char *path,
                             const char *recipe_file, registry_response *out, char *err,
                             size_t err_size) {
    char body[1088];
    if(!fs_format_path(body, sizeof body, "@%s", recipe_file))
        return fail(err, err_size, "the recipe path is too long");

    return request(base_url, token, "POST", path, "application/toml", NULL, "--data-binary", body,
                   out, err, err_size);
}

void registry_explain(const registry_response *response, char *out, size_t out_size) {
    json_document *doc = json_parse(response->body);
    if(doc != NULL) {
        const char *message = json_string(json_get(json_root(doc), "message"));
        if(message != NULL) {
            snprintf(out, out_size, "%s", message);
            json_free(doc);
            return;
        }
        json_free(doc);
    }
    snprintf(out, out_size, "%s", response->body);
}
