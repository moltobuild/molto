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

/* Copy the `line`-th line of `path` into `out`, 1-based and without its line
   ending. Returns false when the file cannot be opened or has no such line —
   which is the ordinary answer, not an exceptional one, for the pseudo-files a
   compiler names (`<command line>`, `<built-in>`) and for anything generated
   and since removed.

   Streamed rather than read whole, because the caller wants one line out of a
   file that may be a machine-written one of a hundred megabytes. A line longer
   than `out` holds is truncated rather than refused: an excerpt is for reading,
   and part of one still reads. */
[[nodiscard]] bool fs_read_line(const char *path, long line, char *out, size_t out_size);

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

/* Modification time and size of `path`, in one call. Returns false if the file
   cannot be stat-ed. Either output may be NULL.

   It exists so that a caller wanting both does not have to stat the file
   itself: doing that means holding a `struct stat`, and the two fields this
   returns are exactly the two whose spelling differs between platforms. */
[[nodiscard]] bool fs_stamp(const char *path, int64_t *mtime_ns, uint64_t *size);

/* The single-writer lock on a workspace, held for as long as the process holds
 * this handle and released when it lets go — including when the process dies,
 * because the operating system closes it.
 *
 * What identifies the lock is the one thing in this header a platform decides:
 * a file descriptor on POSIX, an object handle on Windows. No caller reads the
 * field — they pass the handle back to `fs_lock_release` — so the `#ifdef`
 * stays here and never reaches the code that takes the lock (RFC-0017). */
typedef struct {
#ifdef _WIN32
    void *file; /* HANDLE, opaque so callers need no windows.h */
#else
    int fd;
#endif
    bool held;
} fs_lock;

/* Take the lock at `path`, creating the file if it is not there. Returns false
   without waiting when someone else holds it: a second molto in the same
   workspace is told so, never queued behind the first. */
/* The directory this process is in, in Molto's separator. False if it does not
   fit or cannot be read.

   A service rather than a `getcwd` at each caller, and not only for the
   spelling: what Windows hands back is separated by backslashes, and Molto has
   one separator everywhere else (RFC-0017 refuses a second path model). The
   conversion belongs at the boundary the path comes in through, which is
   here — a caller that received one already converted can compare it against
   anything else Molto composed. */
[[nodiscard]] bool fs_current_dir(char *out, size_t size);

/* Rewrite `path` in place to use Molto's separator.
 *
 * For a path that came from outside — a compiler wrote it into a depfile, a
 * person typed it — rather than one Molto composed, which already uses it. A
 * no-op on POSIX, and it must be: a backslash is a legal character in a
 * filename there, and converting one would rename the file being talked about.
 */
void fs_to_one_separator(char *path);

/* Whether `path` names a place without reference to where the process is.
 *
 * Not `path[0] == '/'`, which is the same question asked in a way that is only
 * right on one platform: an absolute path on Windows opens with a drive and a
 * colon. Nine callers were asking it that way, and every one of them read a
 * Windows path as relative and joined it onto a root. */
[[nodiscard]] bool fs_path_is_absolute(const char *path);

/* Resolve `path` to an absolute one with no `.`, `..` or symlink left in it,
   and false when it cannot be resolved — which for an existing file means only
   that the buffer was too small. The result uses Molto's separator, for the
   reason `fs_current_dir` gives.

   POSIX resolves symlinks and requires the file to exist; Windows resolves
   neither, because it has no `realpath` and the closest call it does have
   answers lexically. Callers use it to compare two paths or to record one, and
   both survive the difference; a caller that needed the symlink followed would
   need a different function and a note saying why. */
[[nodiscard]] bool fs_real_path(const char *path, char *out, size_t size);

/* A second name for an existing file. False when the system will not make one.
 *
 * A symlink on POSIX and a hard link on Windows, and the difference is not
 * carelessness: a Windows symlink needs a privilege or Developer Mode, and a
 * build tool that works only for an administrator is not a build tool. A hard
 * link needs neither and gives what the one caller wants — a second name for
 * the same bytes.
 *
 * The one caller places the two links beside a shared library, and treats a
 * failure as a warning rather than a failed build. */
/* Copy `from` to `to`, byte for byte, creating or truncating the destination.
 *
 * Byte for byte and not through `fs_read_file`, which NUL-terminates what it
 * returns and so cannot describe an archive: the first zero in a tarball would
 * end the copy. */
[[nodiscard]] bool fs_copy_file(const char *from, const char *to);

[[nodiscard]] bool fs_link(const char *target, const char *path);

/* What `path` points at, when it is the kind of link that points at something.
 *
 * False on a Windows link, and that is the whole of the difference: `fs_link`
 * makes a hard link there, which is a second name for the bytes rather than a
 * note saying where they are. Nothing is being read back because there is
 * nothing written down.
 *
 * The answer is verbatim — a relative target comes back relative, which is the
 * property worth checking about the links beside a shared library: an absolute
 * one would write this machine's build directory into an artifact meant to be
 * copied elsewhere. */
[[nodiscard]] bool fs_link_target(const char *path, char *out, size_t size);

[[nodiscard]] bool fs_lock_take(const char *path, fs_lock *out);

/* Let go of a lock. Safe on one that was never taken. */
void fs_lock_release(fs_lock *lock);

/* Return true if `target` must be rebuilt from `source`: true when `target`
   is missing or `source` has a newer modification time (nanosecond precision,
   so two edits within the same second are still told apart). */
[[nodiscard]] bool fs_source_newer(const char *source, const char *target);

/* The name a file would be run by, with whatever the platform appends to an
   executable taken off.

   On POSIX that is the name itself: any file can carry the execute bit, and
   nothing is added to its name to say so. On Windows the suffix *is* the
   permission — a file called `molto-meson` cannot be run and
   `molto-meson.exe` can — so `.exe` is both required here and stripped, which
   is the filter `access(X_OK)` provides on POSIX and cannot provide there,
   since Windows has no execute bit for it to read.

   False when the file cannot be run by name on this platform, or when the
   answer does not fit.

   Copied from pickup's fs_service rather than invented a second time, the way
   RFC-0017 asks: the same question was answered there first. */
[[nodiscard]] bool fs_executable_name(const char *file, char *out, size_t size);

/* The filename a program of this name is stored in: the name itself on POSIX,
   the name plus `.exe` on Windows.

   The inverse of `fs_executable_name`, and it exists because a plugin is found
   by composing a name and then looking for the file — `meson` gives
   `molto-meson`, and looking for a file called `molto-meson` on Windows finds
   nothing at all. */
[[nodiscard]] bool fs_executable_file(const char *name, char *out, size_t size);

#endif /* MOLTO_FS_SERVICE_H */
