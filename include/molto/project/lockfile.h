#ifndef MOLTO_LOCKFILE_H
#define MOLTO_LOCKFILE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/services/dep_graph.h>
#include <molto/util/str_list.h>

/*
 * `Molto.lock`: the resolved graph, written down (RFC-0008).
 *
 * Versions in a manifest are exact, so the lock is not what makes the *version
 * numbers* reproducible — they already are. What it adds is the transitive
 * graph and the checksums: reproducibility of the bytes rather than of the
 * names, and a record of what a build actually contained.
 *
 * It is generated and committed. Generated because a hand-edited lock file is
 * a lock file that lies; committed because a resolution that does not travel
 * to the next machine is not a resolution but a fresh guess. That is also why
 * it is not kept in the WSDB, which is local and disposable by design
 * (RFC-0004).
 *
 * Packages are written sorted by name and every list is ordered, so the diff
 * of this file is worth reading — which is the whole reason to commit it.
 */

/* The format this writer produces and the highest it can read. */
#define LOCKFILE_VERSION 1

#define LOCKFILE_NAME "Molto.lock"

typedef struct {
    char name[DEP_NAME_MAX];
    /* "" for a source that carries no version of its own. */
    char version[DEP_VERSION_MAX];
    char source[DEP_GRAPH_SOURCE_MAX];
    /* "" when the origin cannot be digested, which is what a path dependency
       is. */
    char checksum[SOURCE_DIGEST_MAX];
    str_list scopes;
    str_list dependencies;
} lock_package;

typedef struct {
    long version;
    char root[DEP_NAME_MAX]; /* the package this lock belongs to */
    lock_package *packages;
    size_t count;
} lockfile;

/* Render a graph as the text of a lock file. Heap, caller frees; NULL on
   allocation failure.

   Separate from writing it because everything worth getting wrong is here —
   the ordering, the escaping, the keys — and none of it needs a filesystem to
   exercise. */
[[nodiscard]] char *lockfile_render(const char *package, const dep_graph *graph);

/* Write `Molto.lock` at the workspace root, replacing any previous one. */
[[nodiscard]] bool lockfile_write(const char *root, const char *package, const dep_graph *graph,
                                  char *err, size_t err_size);

/* Read `Molto.lock` from the workspace root.

   False, with a reason, when it is absent, unreadable, not valid TOML, or
   declares a format this reader does not understand. A caller treats every one
   of those the same way: resolve again. Never trusting a file you cannot fully
   read is the same rule the WSDB follows, and for the same reason. */
[[nodiscard]] bool lockfile_read(const char *root, lockfile *out, char *err, size_t err_size);

/* True when `lock` still describes `ctx`: same root package, and the same set
   of direct dependencies at the same versions and origins. A manifest edited
   since the lock was written makes it stale, and a stale lock is re-resolved
   rather than patched. */
[[nodiscard]] bool lockfile_matches(const lockfile *lock, const project_ctx *ctx);

/* Refuse a resolution that does not match what was locked.
 *
 * This is what a lock file is *for*. Exact versions already make the numbers
 * reproducible (RFC-0008); what they cannot do is notice that the bytes behind
 * one of those numbers changed. A coordinate is supposed to be immutable
 * (RFC-0010), so a package that resolves to a different checksum, a different
 * origin, or that has grown a dependency it did not have, is either a registry
 * that rewrote history or an attack — and both are worth stopping a build for.
 *
 * Only meaningful when `lockfile_matches` already said the manifest is
 * unchanged: a user editing `[deps]` is *expected* to change the resolution.
 */
[[nodiscard]] bool lockfile_verify(const lockfile *lock, const dep_graph *graph, char *err,
                                   size_t err_size);

void lockfile_free(lockfile *lock);

#endif /* MOLTO_LOCKFILE_H */
