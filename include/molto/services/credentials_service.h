#ifndef MOLTO_CREDENTIALS_SERVICE_H
#define MOLTO_CREDENTIALS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * What `molto login` wrote and `molto publish` reads.
 *
 * The file is ~/.molto/credentials.toml, created with mode 0600. It holds a
 * bearer token, not a password: the registry stores only the token's digest,
 * so a copy of this file is revocable and a password would not be.
 *
 * One registry for now. Several would mean a table per URL, and nothing yet
 * publishes to two.
 */

#define CREDENTIALS_URL_MAX 256
#define CREDENTIALS_EMAIL_MAX 256
#define CREDENTIALS_TOKEN_MAX 160

typedef struct {
    char registry[CREDENTIALS_URL_MAX];
    char email[CREDENTIALS_EMAIL_MAX];
    char token[CREDENTIALS_TOKEN_MAX];
} credentials;

/* Absolute path of the credentials file. False if $HOME is unset or the path
   would not fit. */
[[nodiscard]] bool credentials_path(char *out, size_t size);

/* Read the stored credentials. Returns false when there are none or the file
   cannot be read, writing a reason into `err` when it is not NULL. */
[[nodiscard]] bool credentials_load(credentials *out, char *err, size_t err_size);

/* Write `creds`, replacing whatever was there, with the file readable only by
   its owner. Creates ~/.molto if it does not exist. */
[[nodiscard]] bool credentials_save(const credentials *creds, char *err, size_t err_size);

#endif /* MOLTO_CREDENTIALS_SERVICE_H */
