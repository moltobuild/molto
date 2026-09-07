#ifndef MOLTO_UTIL_SHA256_H
#define MOLTO_UTIL_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * SHA-256 (FIPS 180-4).
 *
 * Publishing declares a digest the registry verifies as the bytes land, so
 * molto has to compute one. It used to ask `sha256sum` for it, which cost a
 * dependency and, on Windows, correctness: GNU coreutils escapes a backslash
 * in a filename and prefixes the whole line with one, so `sha256sum --binary
 * C:\path\to.tar.gz` answers
 *
 *     \aa998685...  C:\\path\\to.tar.gz
 *
 * and the first field is 65 characters. Every native Windows path took that
 * branch, and the report was "sha256sum did not answer with a digest" — a
 * true statement about a digest that was right there.
 *
 * Incremental because an artifact is not small: a toolchain runs to hundreds
 * of megabytes and is never held in memory whole.
 *
 * Ported from pickup's `src/util/sha256.c`, which had already answered these
 * questions for the same ecosystem.
 */

/* Bytes in a digest, and in its hex form counting the terminator. */
#define SHA256_DIGEST_SIZE 32
#define SHA256_HEX_SIZE 65

/* How much of a file is read at a time when hashing it. */
#define SHA256_CHUNK_SIZE 65536

typedef struct {
    uint32_t state[8];
    uint64_t length; /* total bytes fed in */
    unsigned char block[64];
    size_t buffered;
} sha256_state;

/* Begin a digest. */
void sha256_init(sha256_state *state);

/* Feed in more data. Call as many times as needed. */
void sha256_update(sha256_state *state, const void *data, size_t length);

/* Finish, writing SHA256_HEX_SIZE bytes of lowercase hex into `hex_out`. The
   state must not be used again afterwards. */
void sha256_finish(sha256_state *state, char *hex_out);

/* Hash a whole file. False if it could not be read; `hex_out` needs room for
   SHA256_HEX_SIZE bytes. */
[[nodiscard]] bool sha256_file(const char *path, char *hex_out);

/* Called as a file is hashed, with how much of it has been read. `total` is 0
   when the size could not be told, which a caller reads as "no denominator". */
typedef void (*sha256_watcher)(long long done, long long total, void *context);

/* The same, reporting progress as it goes.

   Hashing 127 MB takes long enough that silence is indistinguishable from a
   hang, and the total is known here — it is the size of the file — so it can
   be reported as a real fraction rather than mere animation. `watcher` may be
   NULL. */
[[nodiscard]] bool sha256_file_watched(const char *path, char *hex_out, sha256_watcher watcher,
                                       void *context);

#endif /* MOLTO_UTIL_SHA256_H */
