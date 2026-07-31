#ifndef MOLTO_FS_SERVICE_H
#define MOLTO_FS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

/* Return true if a filesystem entry exists at `path`. */
[[nodiscard]] bool fs_path_exists(const char *path);

/* Create a single directory. Succeeds if it already exists. */
[[nodiscard]] bool fs_make_dir(const char *path);

/* Write `content` to `path`, creating or truncating the file. */
[[nodiscard]] bool fs_write_file(const char *path, const char *content);

/* Read the whole file at `path` into a heap-allocated, NUL-terminated string.
   Returns NULL on error; the caller must free() the result. */
[[nodiscard]] char *fs_read_file(const char *path);

/* Return true if `path` exists and is a directory. */
[[nodiscard]] bool fs_is_dir(const char *path);

/* Create `path` and any missing parent directories. Succeeds if it exists. */
[[nodiscard]] bool fs_make_dirs(const char *path);

/* Modification time of `path` in nanoseconds since the epoch. Returns false if
   the file cannot be stat-ed. Filesystems without sub-second resolution simply
   report whole seconds. */
[[nodiscard]] bool fs_mtime_ns(const char *path, int64_t *out);

/* Return true if `target` must be rebuilt from `source`: true when `target`
   is missing or `source` has a newer modification time (nanosecond precision,
   so two edits within the same second are still told apart). */
[[nodiscard]] bool fs_source_newer(const char *source, const char *target);

#endif /* MOLTO_FS_SERVICE_H */
