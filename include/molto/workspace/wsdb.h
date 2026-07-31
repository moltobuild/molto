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
bool wsdb_record_object(wsdb *db, const char *object, const char *command,
                        const str_list *prereqs);

/* True if `binary` is up to date for `command` (staleness vs its objects is the
   caller's concern). */
[[nodiscard]] bool wsdb_binary_fresh(wsdb *db, const char *binary, const char *command);

/* Record that `binary` was linked by `command`. Returns false if the record
   could not be stored, in which case the binary is re-linked next time. */
bool wsdb_record_binary(wsdb *db, const char *binary, const char *command);

/* Delete outputs (and their DB entries) whose path is under `prefix` but not in
   `live` — i.e. orphans left by a removed source. */
void wsdb_prune(wsdb *db, const str_list *live, const char *prefix);

/* Persist if changed, release the lock and free. Safe on NULL. Returns false if
   the state could not be saved — the build itself still stands, but the next one
   will not be incremental, so the caller is expected to say so out loud. */
bool wsdb_close(wsdb *db);

#endif /* MOLTO_WSDB_H */
