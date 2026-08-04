#ifndef MOLTO_FS_SERVICE_H
#define MOLTO_FS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
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

/* Return true if `path` is a directory, without following a symlink to one.
   Walking into linked directories can loop forever when the links form a
   cycle, so a tree walk asks this instead of fs_is_dir. */
[[nodiscard]] bool fs_is_dir_no_follow(const char *path);

/* A cheap signature of a file's current state: modification time and size,
   combined. Zero when the file cannot be read.

   It exists so a caller can notice that an input changed *while* it was being
   used to produce something. Recording a result then would store the output of
   the old content under the signature of the new one, and nothing would ever
   invalidate it: by every later measure the entry is current. Comparing this
   before and after is what turns that into one extra run instead of a wrong
   answer that persists. */
[[nodiscard]] uint64_t fs_signature(const char *path);

/* Compose a path into `out`. Returns false if the result did not fit, so a
   truncated path is reported instead of being used: two long source paths that
   truncate to the same object path would otherwise overwrite each other. */
[[nodiscard]] bool fs_format_path(char *out, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

/* Report on stderr that `what` did not fit the buffer it was composed into, and
   return false, so a caller can write `... || fs_report_long_path(path)`. Lives
   beside fs_format_path because it is the one failure that function has. */
bool fs_report_long_path(const char *what);

/* The path as the user thinks of it: relative to `root` when it is under it,
   and unchanged otherwise. Points into `path`; nothing is copied. Molto works
   in absolute paths because the tools run wherever the user invoked it from,
   and shows relative ones because that is what a project looks like. */
[[nodiscard]] const char *fs_relative_to(const char *path, const char *root);

/* Create `path` and any missing parent directories. Succeeds if it exists. */
[[nodiscard]] bool fs_make_dirs(const char *path);

/* Recursively delete `path` and everything below it. Succeeds if `path` does
   not exist. Symlinks are removed, never followed, so a link inside the tree
   cannot lead the deletion outside of it. */
[[nodiscard]] bool fs_remove_tree(const char *path);

/* Modification time of `path` in nanoseconds since the epoch. Returns false if
   the file cannot be stat-ed. Filesystems without sub-second resolution simply
   report whole seconds. */
[[nodiscard]] bool fs_mtime_ns(const char *path, int64_t *out);

/* Return true if `target` must be rebuilt from `source`: true when `target`
   is missing or `source` has a newer modification time (nanosecond precision,
   so two edits within the same second are still told apart). */
[[nodiscard]] bool fs_source_newer(const char *source, const char *target);

#endif /* MOLTO_FS_SERVICE_H */
