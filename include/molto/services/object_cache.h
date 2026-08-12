#ifndef MOLTO_OBJECT_CACHE_H
#define MOLTO_OBJECT_CACHE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Compiled objects, shared between projects.
 *
 * Fetching sqlite once and then compiling it once per project is half a win:
 * the download is shared and the twenty seconds are not. This is the other
 * half — an object compiled from a dependency is kept under
 * ~/.molto/cache/objects and reused by every project that would compile it the
 * same way.
 *
 * It covers dependencies and nothing else, and the reason is soundness rather
 * than scope. Knowing that two compilations produce the same object means
 * knowing that the source and every header it includes are identical, and the
 * headers are not known until after the compiler has run once. For a
 * dependency the question does not arise: its tree came from an archive
 * verified against a sha256, or from a commit id, and it is addressed by that
 * coordinate — the whole tree is immutable, so the coordinate answers for its
 * contents. A project's own sources are mutable and their headers unknown;
 * that is the problem ccache exists for, and molto does not try to be one.
 *
 * A `path` dependency is deliberately not covered either: its bytes are
 * whatever is on disk right now, which is the point of it.
 */

#define OBJECT_CACHE_KEY_MAX 64
#define OBJECT_CACHE_PATH_MAX 1024

/* True when `source` belongs to a dependency the shared cache may hold: it
   lives inside the source cache, so its content is fixed by its coordinate. */
[[nodiscard]] bool object_cache_covers(const char *source);

/* Where the shared object for compiling `source` with `command` lives.

   `command` is the full compile command. The parts of it that name where the
   output goes are excluded from the key, because they are what differs between
   two projects compiling the very same thing; everything else — the compiler,
   the standard, the optimisation level, every define and include — is what
   makes two objects interchangeable, and stays in.

   False when `source` is not covered, or when the path would not fit. */
[[nodiscard]] bool object_cache_path(const char *source, const char *command, char *out,
                                     size_t size);

/* Put `cached` in place at `object`. False when the cache does not hold it. */
[[nodiscard]] bool object_cache_take(const char *cached, const char *object);

/* Keep a freshly compiled `object` under `cached`. Best effort: a cache that
   cannot be written costs a rebuild somewhere else and nothing more. */
void object_cache_put(const char *object, const char *cached);

#endif /* MOLTO_OBJECT_CACHE_H */
