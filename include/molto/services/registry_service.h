#ifndef MOLTO_REGISTRY_SERVICE_H
#define MOLTO_REGISTRY_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The registry's REST API, over curl.
 *
 * Molto shells out rather than linking an HTTP client, exactly as it shells
 * out to a compiler: curl is present wherever a build is, and a TLS stack
 * inside molto would be a second thing to keep patched.
 *
 * The token never appears in argv. `ps` is readable by every process on the
 * machine, so the Authorization header is passed through a curl config file
 * created 0600 and deleted afterwards.
 */

/* Largest answer read back. The registry's JSON replies are a few hundred
   bytes; a recipe echoed in `metadata` is the only thing that grows. */
#define REGISTRY_BODY_MAX 16384

typedef struct {
    long status; /* HTTP status, or 0 when curl never got one */
    char body[REGISTRY_BODY_MAX];
} registry_response;

/* True if the request reached the registry and it answered `status`. A false
   here is a transport failure, never an HTTP error: a 404 is a true with a
   status of 404. */

/* POST /v1/auth/token — exchange an email and password for a bearer token.
   Writes the token into `token`. */
[[nodiscard]] bool registry_create_token(const char *base_url, const char *email,
                                         const char *password, const char *token_name, char *token,
                                         size_t token_size, char *err, size_t err_size);

/* PUT /v1/{kind}s/{name}/{version}/{target}/blob — stream the archive up.
   `checksum` is the lowercase hex sha256 the registry verifies as it lands. */
[[nodiscard]] bool registry_upload_blob(const char *base_url, const char *token, const char *path,
                                        const char *file, const char *checksum,
                                        registry_response *out, char *err, size_t err_size);

/* POST /v1/{kind}s — record the artifact, with the recipe.toml as the body. */
[[nodiscard]] bool registry_publish_recipe(const char *base_url, const char *token,
                                           const char *path, const char *recipe_file,
                                           registry_response *out, char *err, size_t err_size);

/* The `message` of the registry's error body, for reporting a refusal in the
   terms it used. Falls back to the whole body when it is not that shape. */
void registry_explain(const registry_response *response, char *out, size_t out_size);

#endif /* MOLTO_REGISTRY_SERVICE_H */
