#include <molto/services/credentials_service.h>

#include <molto/services/fs_service.h>
#include <molto/util/toml.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The section every key lives under, so the file has room to grow a second
   table without the first one's keys becoming ambiguous. */
#define SECTION "registry"

static bool fail(char *err, size_t err_size, const char *message) {
    if(err != NULL && err_size > 0)
        snprintf(err, err_size, "%s", message);
    return false;
}

static bool molto_home(char *out, size_t size) {
    const char *home = getenv("HOME");
    if(home == NULL || home[0] == '\0')
        return false;
    return fs_format_path(out, size, "%s/.molto", home);
}

bool credentials_path(char *out, size_t size) {
    char home[512];
    if(!molto_home(home, sizeof home))
        return false;
    return fs_format_path(out, size, "%s/credentials.toml", home);
}

static bool read_field(const toml_document *doc, const char *key, char *out, size_t size, char *err,
                       size_t err_size) {
    if(!toml_get_string(doc, SECTION, key, out, size)) {
        char message[128];
        snprintf(message, sizeof message, "credentials.toml has no '%s'", key);
        return fail(err, err_size, message);
    }
    return true;
}

bool credentials_load(credentials *out, char *err, size_t err_size) {
    char path[768];
    if(!credentials_path(path, sizeof path))
        return fail(err, err_size, "HOME is not set, so there is nowhere to read credentials from");

    char *text = fs_read_file(path);
    if(text == NULL)
        return fail(err, err_size, "not logged in; run `molto login`");

    char parse_error[256] = "";
    toml_document *doc = toml_parse(text, parse_error, sizeof parse_error);
    free(text);
    if(doc == NULL)
        return fail(err, err_size, parse_error);

    const bool ok = read_field(doc, "url", out->registry, sizeof out->registry, err, err_size) &&
                    read_field(doc, "token", out->token, sizeof out->token, err, err_size);
    /* The address is what the person sees in `molto login`'s confirmation; a
       token without one still works. */
    if(ok && !toml_get_string(doc, SECTION, "email", out->email, sizeof out->email))
        out->email[0] = '\0';

    toml_free(doc);
    return ok;
}

/* Written through a private temporary file and renamed, so a reader never sees
   a half-written token, and created 0600 from the start rather than chmod'ed
   afterwards: between creat and chmod the token would be world-readable. */
static bool write_private(const char *path, const char *content, char *err, size_t err_size) {
    char temporary[832];
    if(!fs_format_path(temporary, sizeof temporary, "%s.new", path))
        return fail(err, err_size, "the credentials path is too long");

    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if(fd < 0)
        return fail(err, err_size, "could not create the credentials file");

    const size_t length = strlen(content);
    const ssize_t written = write(fd, content, length);
    if(close(fd) != 0 || written < 0 || (size_t)written != length) {
        (void)unlink(temporary);
        return fail(err, err_size, "could not write the credentials file");
    }
    if(rename(temporary, path) != 0) {
        (void)unlink(temporary);
        return fail(err, err_size, "could not replace the credentials file");
    }
    return true;
}

bool credentials_save(const credentials *creds, char *err, size_t err_size) {
    char home[512];
    char path[768];
    if(!molto_home(home, sizeof home) || !credentials_path(path, sizeof path))
        return fail(err, err_size, "HOME is not set, so there is nowhere to store credentials");
    if(!fs_make_dir(home))
        return fail(err, err_size, "could not create ~/.molto");

    char content[1536];
    const int n = snprintf(content, sizeof content,
                           "# Written by `molto login`. This file is a credential: it is\n"
                           "# readable only by you, and the token can be revoked from the\n"
                           "# registry's account page.\n"
                           "[" SECTION "]\n"
                           "url = \"%s\"\n"
                           "email = \"%s\"\n"
                           "token = \"%s\"\n",
                           creds->registry, creds->email, creds->token);
    if(n < 0 || (size_t)n >= sizeof content)
        return fail(err, err_size, "the credentials did not fit");

    return write_private(path, content, err, err_size);
}
