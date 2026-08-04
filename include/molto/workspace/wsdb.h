#ifndef MOLTO_WSDB_H
#define MOLTO_WSDB_H

#include <stdbool.h>

#include <molto/util/str_list.h>

/*
 * The Workspace Database (RFC-0004): the authoritative incremental state stored
 * in `<root>/.bin/wsdb`. It records, per output artifact, the command that
 * produced it and its prerequisites, and per input file a hybrid freshness
 * signature (nanosecond mtime + size, confirmed by a content hash). All paths in
 * the same root-prefixed paths the build composes.
 */
typedef struct wsdb wsdb;

/* Open (or create) the workspace database at `<root>/.bin/`, taking an exclusive
   lock. Returns NULL if another process holds the lock, or on failure. A missing,
   version-incompatible or corrupt database opens empty (fail-safe). */
[[nodiscard]] wsdb *wsdb_open(const char *root);

/* True if `object` is up to date: it exists, was produced by `command`, and none
   of its recorded prerequisites changed (hybrid mtime/size/hash). */
[[nodiscard]] bool wsdb_object_fresh(wsdb *db, const char *object, const char *command);

/* Record that `object` was produced by `command` from `prereqs`, refreshing the
   freshness signature of the source and every prerequisite. Returns false if the
   record could not be stored, in which case the object is rebuilt next time. */
bool wsdb_record_object(wsdb *db, const char *object, const char *command, const str_list *prereqs);

/* True if `binary` is up to date for `command` (staleness vs its objects is the
   caller's concern). */
[[nodiscard]] bool wsdb_binary_fresh(wsdb *db, const char *binary, const char *command);

/* Record that `binary` was linked by `command`. Returns false if the record
   could not be stored, in which case the binary is re-linked next time. */
bool wsdb_record_binary(wsdb *db, const char *binary, const char *command);

/* --- resolved toolchains ---
   Which compiler to invoke is the answer to a question asked of an external
   resolver. Recording it here is what spares that query on every build. The
   entry is keyed by language, fingerprinted by the request, and tied to the
   compiler binary itself, so replacing the compiler invalidates it. */

/* True if the toolchain recorded under `key` still answers `request` and the
   compiler it names is unchanged. */
[[nodiscard]] bool wsdb_toolchain_fresh(wsdb *db, const char *key, const char *request);

/* Record `values` (the resolved answer) as the result of `request`. The first
   value must be the compiler's path: it is registered as an input, so the
   existing freshness machinery notices when it is replaced. */
bool wsdb_record_toolchain(wsdb *db, const char *key, const char *request, const str_list *values);

/* Read back the values recorded under `key` into `out` (caller-initialised).
   Returns false if there is no such entry. */
[[nodiscard]] bool wsdb_toolchain_values(wsdb *db, const char *key, str_list *out);

/* --- analysis results (RFC-0006) ---
   What a tool said about one file, so an unchanged file is not analysed again.
   Unlike an object there is no artifact on disk to check: the entry is the
   whole of what was produced, and it must replay the diagnostics rather than
   the fact that there were none. */

/* True if the result recorded under `key` still answers `fingerprint` and none
   of the files it was derived from changed. */
[[nodiscard]] bool wsdb_result_fresh(wsdb *db, const char *key, const char *fingerprint);

/* Record `values` as what `fingerprint` produced, watching `prereqs` — the file
   analysed and the headers it included. `values` may be empty: a file with no
   diagnostics is a recorded answer, not a missing one. `prereqs` may not, since
   an entry watching nothing would never go stale. */
bool wsdb_record_result(wsdb *db, const char *key, const char *fingerprint, const str_list *prereqs,
                        const str_list *values);

/* Read back what was recorded under `key` into `out` (caller-initialised).
   False only if there is no such entry; true with nothing appended is the file
   that was analysed and had nothing to say. */
[[nodiscard]] bool wsdb_result_values(wsdb *db, const char *key, str_list *out);

/* Delete outputs (and their DB entries) whose path is under `prefix` but not in
   `live` — i.e. orphans left by a removed source. */
void wsdb_prune(wsdb *db, const str_list *live, const char *prefix);

/* Persist if changed, release the lock and free. Safe on NULL. Returns false if
   the state could not be saved — the build itself still stands, but the next one
   will not be incremental, so the caller is expected to say so out loud. */
bool wsdb_close(wsdb *db);

#endif /* MOLTO_WSDB_H */
