#include <molto/services/fs_service.h>

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <unistd.h>
#endif

/*
 * The platform, in one block, because every function below this line is one
 * question asked of the filesystem and none of them should have to know which
 * system is answering (RFC-0017).
 */

/* Windows has no mode argument: a directory's permissions come from its parent
   rather than from the call that creates it. */
#ifdef _WIN32
#define make_one_dir(path) _mkdir(path)
#else
#define make_one_dir(path) mkdir((path), 0755)
#endif

/* Nanoseconds in one second, for composing a timestamp. */
#define NANOS_PER_SECOND 1000000000LL

#ifdef _WIN32
/* 1601 to 1970 in the 100-nanosecond units a FILETIME counts. */
#define FILETIME_EPOCH_DELTA 116444736000000000LL

static int64_t filetime_to_unix_ns(const FILETIME *time) {
    const ULARGE_INTEGER since_1601 = {
        .LowPart = time->dwLowDateTime,
        .HighPart = time->dwHighDateTime,
    };
    return ((int64_t)since_1601.QuadPart - FILETIME_EPOCH_DELTA) * 100;
}
#endif

/*
 * When a file was last written, in nanoseconds, and how big it is.
 *
 * Deliberately not `stat` on Windows. `struct stat` there keeps whole seconds
 * in `st_mtime` and has no `st_mtim` at all, which made the resolution of
 * every freshness check one second -- so two writes inside the same second
 * were simultaneous, and a rebuild that nanoseconds would have triggered did
 * not happen. That is not a rounding anyone chose; it is what `stat` can say.
 *
 * It is also not what the platform can say. `GetFileAttributesEx` answers with
 * a FILETIME, which counts 100-nanosecond units, and hands back the size in
 * the same call. So the ceiling was the interface, and this is a different
 * interface.
 *
 * What that cost while it stood: `molto build` on Windows did not notice a
 * source edited within a second of the last build. Not a slow rebuild -- a
 * skipped one, on a tool whose whole job is deciding what changed.
 */
[[nodiscard]] static bool file_written_at(const char *path, int64_t *mtime_ns, uint64_t *size) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA facts;
    if(!GetFileAttributesExA(path, GetFileExInfoStandard, &facts))
        return false;
    if(mtime_ns != NULL)
        *mtime_ns = filetime_to_unix_ns(&facts.ftLastWriteTime);
    if(size != NULL)
        *size = ((uint64_t)facts.nFileSizeHigh << 32) | facts.nFileSizeLow;
    return true;
#else
    struct stat info;
    if(stat(path, &info) != 0)
        return false;
    if(mtime_ns != NULL)
        *mtime_ns = (int64_t)info.st_mtim.tv_sec * NANOS_PER_SECOND + (int64_t)info.st_mtim.tv_nsec;
    if(size != NULL)
        *size = (uint64_t)info.st_size;
    return true;
#endif
}

bool fs_path_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

bool fs_make_dir(const char *path) {
    if(make_one_dir(path) == 0)
        return true;
    /* Treat an already existing directory as success. */
    struct stat info;
    if(stat(path, &info) == 0 && S_ISDIR(info.st_mode))
        return true;
    return false;
}

bool fs_write_file(const char *path, const char *content) {
    /* Binary, because text mode on Windows turns every `\n` into `\r\n` on the
       way out while `fs_read_file` opens "rb" and reads them back verbatim.
       Write, read, write, and a file grows a `\r` per line per round trip.

       Nothing molto writes wants a platform's idea of a line. `Molto.lock` is
       committed and diffed, and RFC-0017 asks for it to be byte-identical
       across platforms — a manifest that differed by line ending would make
       every lock differ by operating system. */
    FILE *file = fopen(path, "wb");
    if(file == NULL)
        return false;
    size_t length = strlen(content);
    size_t written = fwrite(content, 1, length, file);
    int closed = fclose(file);
    return written == length && closed == 0;
}

char *fs_read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return NULL;
    if(fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if(size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1);
    if(buffer == NULL) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[read] = '\0';
    return buffer;
}

/* Advance to the start of line `line`, reading and discarding. Returns false
   when the file ends first, which is how a line past the end is reported. */
static bool skip_to_line(FILE *file, long line) {
    for(long at = 1; at < line;) {
        int c = getc(file);
        if(c == EOF)
            return false;
        if(c == '\n')
            at++;
    }
    return true;
}

bool fs_read_line(const char *path, long line, char *out, size_t out_size) {
    if(out == NULL || out_size == 0)
        return false;
    out[0] = '\0';
    if(line < 1)
        return false;

    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return false;
    if(!skip_to_line(file, line)) {
        fclose(file);
        return false;
    }

    int c = getc(file);
    if(c == EOF) { /* the file ended on the line before this one */
        fclose(file);
        return false;
    }

    /* Read past the end of the buffer rather than stopping at it, so the line
       ending is still found and a truncated excerpt is a shorter line and not
       a run-on into the next one. */
    size_t used = 0;
    for(; c != EOF && c != '\n'; c = getc(file)) {
        if(used + 1 < out_size)
            out[used++] = (char)c;
    }
    fclose(file);

    /* A file written on another platform ends its lines with two characters,
       and showing the first of them would put a stray glyph at the end of
       every excerpt taken from it. */
    if(used > 0 && out[used - 1] == '\r')
        used--;
    out[used] = '\0';
    return true;
}

bool fs_is_dir(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/*
 * `stat` that does not follow a symlink.
 *
 * Windows has no `lstat`, and what it does have does not answer the same
 * question — a reparse point is not a symlink and the only caller here is
 * asking whether to walk into a directory. Following is the safe answer there,
 * because nothing Molto makes on Windows is a symlink: `fs_link` makes hard
 * links, which a stat cannot tell from the file itself. The day something does
 * create one, this is where the answer has to change.
 */
static int stat_no_follow(const char *path, struct stat *out) {
#ifdef _WIN32
    return stat(path, out);
#else
    return lstat(path, out);
#endif
}

bool fs_is_dir_no_follow(const char *path) {
    struct stat info;
    return stat_no_follow(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool fs_format_path(char *out, size_t size, const char *format, ...) {
    if(size == 0)
        return false;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out, size, format, args);
    va_end(args);
    return written >= 0 && (size_t)written < size;
}

/* A prefix there is nothing to create for, because it is where the filesystem
   starts. On POSIX that is `/`, which the loop below never produces; on Windows
   it is `D:`, which it produces for every absolute path and which `mkdir`
   refuses and `stat` will not reliably confirm — "D:" names the current
   directory on that drive, not the drive. */
static bool is_a_root(const char *prefix) {
    const size_t length = strlen(prefix);
#ifdef _WIN32
    if(length == 2 && prefix[1] == ':')
        return true;
#endif
    return length == 1 && prefix[0] == '/';
}

bool fs_make_dirs(const char *path) {
    char buffer[4096];
    size_t length = strlen(path);
    if(length == 0 || length >= sizeof buffer)
        return false;
    memcpy(buffer, path, length + 1);
    /* Create each intermediate component in turn. */
    for(size_t i = 1; i < length; i++) {
        if(buffer[i] != '/')
            continue;
        buffer[i] = '\0';
        if(!is_a_root(buffer) && !fs_make_dir(buffer))
            return false;
        buffer[i] = '/';
    }
    return fs_make_dir(buffer);
}

/* Size of the buffer used to compose the path of an entry being deleted. */
#define REMOVE_PATH_SIZE 4096

bool fs_remove_tree(const char *path) {
    if(!fs_is_dir_no_follow(path)) {
        /* A file, a symlink, or nothing at all. */
        return remove(path) == 0 || !fs_path_exists(path);
    }
    DIR *dir = opendir(path);
    if(dir == NULL)
        return false;
    bool ok = true;
    const struct dirent *entry;
    while((entry = readdir(dir)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[REMOVE_PATH_SIZE];
        if(!fs_format_path(child, sizeof child, "%s/%s", path, entry->d_name)) {
            ok = false;
            continue;
        }
        if(!fs_remove_tree(child))
            ok = false;
    }
    closedir(dir);
    return rmdir(path) == 0 && ok;
}

bool fs_mtime_ns(const char *path, int64_t *out) { return file_written_at(path, out, NULL); }

bool fs_stamp(const char *path, int64_t *mtime_ns, uint64_t *size) {
    return file_written_at(path, mtime_ns, size);
}

uint64_t fs_signature(const char *path) {
    int64_t mtime_ns = 0;
    uint64_t size = 0;
    if(!fs_stamp(path, &mtime_ns, &size))
        return 0;
    return (uint64_t)mtime_ns + size;
}

bool fs_source_newer(const char *source, const char *target) {
    int64_t target_ns;
    if(!fs_mtime_ns(target, &target_ns))
        return true; /* missing target: rebuild */
    int64_t source_ns;
    if(!fs_mtime_ns(source, &source_ns))
        return true; /* missing source: fail safe and rebuild */
    return source_ns > target_ns;
}

bool fs_report_long_path(const char *what) {
    fprintf(stderr, "molto: path too long to compose (%s)\n", what);
    return false;
}

const char *fs_relative_to(const char *path, const char *root) {
    if(root == NULL || root[0] == '\0')
        return path;
    size_t root_length = strlen(root);
    if(strncmp(path, root, root_length) == 0 && path[root_length] == '/')
        return path + root_length + 1;
    return path;
}

/*
 * The workspace lock.
 *
 * Two ways of saying the same thing. POSIX asks for the lock separately from
 * opening the file, so the two calls are two steps. Windows has no advisory
 * lock of this shape, but it does not need one: a file opened with a sharing
 * mode of zero is exclusive by the act of opening it, and a second opener is
 * refused. Both release on close, and both therefore release when the process
 * dies however it dies — which is the property that matters, because a lock a
 * crash leaves behind is a workspace nobody can build in.
 */

bool fs_lock_take(const char *path, fs_lock *out) {
    out->held = false;
#ifdef _WIN32
    out->file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if(out->file == INVALID_HANDLE_VALUE) {
        out->file = NULL;
        return false;
    }
#else
    out->fd = open(path, O_CREAT | O_RDWR, 0644);
    if(out->fd < 0)
        return false;
    if(flock(out->fd, LOCK_EX | LOCK_NB) != 0) {
        close(out->fd);
        out->fd = -1;
        return false;
    }
#endif
    out->held = true;
    return true;
}

void fs_lock_release(fs_lock *lock) {
    if(lock == NULL || !lock->held)
        return;
#ifdef _WIN32
    (void)CloseHandle(lock->file);
    lock->file = NULL;
#else
    (void)close(lock->fd); /* releases the flock */
    lock->fd = -1;
#endif
    lock->held = false;
}

/*
 * Windows answers with backslashes; Molto composes with forward slashes.
 *
 * Converting here rather than at every comparison is the whole of RFC-0017's
 * rule about not growing a second path model. It is not cosmetic: the
 * workspace root arrives from the system and a source path is composed by
 * `fs_format_path`, and a build that mixes the two produces
 * `D:\ws\project/src/main.c` — which every "is this inside the workspace"
 * check reads as outside, because the character after the root is not the
 * separator it was looking for. Windows accepts a forward slash everywhere it
 * accepts a backslash, so nothing is lost by choosing one.
 *
 * POSIX does not convert anything, and must not: a backslash is a perfectly
 * legal character in a filename there.
 */
void fs_to_one_separator(char *path) {
#ifdef _WIN32
    for(char *c = path; *c != '\0'; c++) {
        if(*c == '\\')
            *c = '/';
    }
#else
    (void)path;
#endif
}

bool fs_current_dir(char *out, size_t size) {
#ifdef _WIN32
    if(_getcwd(out, (int)size) == NULL)
        return false;
#else
    if(getcwd(out, size) == NULL)
        return false;
#endif
    fs_to_one_separator(out);
    return true;
}

bool fs_real_path(const char *path, char *out, size_t size) {
#ifdef _WIN32
    const DWORD written = GetFullPathNameA(path, (DWORD)size, out, NULL);
    if(written == 0 || written >= size)
        return false;
    fs_to_one_separator(out);
    return true;
#else
    char resolved[PATH_MAX];
    if(realpath(path, resolved) == NULL)
        return false;
    return fs_format_path(out, size, "%s", resolved);
#endif
}

bool fs_link_target(const char *path, char *out, size_t size) {
#ifdef _WIN32
    (void)path;
    (void)out;
    (void)size;
    return false;
#else
    const ssize_t length = readlink(path, out, size - 1);
    if(length <= 0)
        return false;
    out[length] = '\0';
    return true;
#endif
}

bool fs_link(const char *target, const char *path) {
#ifdef _WIN32
    return CreateHardLinkA(path, target, NULL) != 0;
#else
    return symlink(target, path) == 0;
#endif
}

bool fs_path_is_absolute(const char *path) {
    if(path == NULL || path[0] == '\0')
        return false;
#ifdef _WIN32
    /* `D:/x` names a place; `D:x` does not — it is relative to whatever
       directory that drive is currently on, which is per-process state and not
       a location. A leading slash is absolute on the current drive, which is
       enough for every caller here: they all compare against or join onto a
       root from the same machine. */
    if(((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')
        return path[2] == '/';
#endif
    return path[0] == '/';
}

bool fs_path_without_root(const char *path, char *out, size_t size) {
    if(path == NULL || out == NULL || size == 0)
        return false;

    /* Written into the result before `rest`, so a drive survives as a
       directory of its own. Empty whenever there is no drive to keep, which is
       every path on POSIX and most of them on Windows. */
    char drive[3] = "";
    const char *rest = path;
    if(fs_path_is_absolute(path)) {
#ifdef _WIN32
        if(path[1] == ':') {
            drive[0] = path[0];
            drive[1] = '/';
            rest = path + 2;
        }
#endif
        /* Every leading slash, not one: `//server/share` is a path a Windows
           machine really hands out, and two empty components at the front of
           the result would be a directory called nothing. */
        while(rest[0] == '/')
            rest++;
    }

    /* `>= 0`, not `> 0`: the root with its root off is the empty string, and
       that is an answer written successfully rather than a failure to write
       one. */
    const int written = snprintf(out, size, "%s%s", drive, rest);
    return written >= 0 && (size_t)written < size;
}

bool fs_copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if(in == NULL)
        return false;
    FILE *out = fopen(to, "wb");
    if(out == NULL) {
        (void)fclose(in);
        return false;
    }

    char chunk[8192];
    bool ok = true;
    for(;;) {
        const size_t got = fread(chunk, 1, sizeof chunk, in);
        if(got == 0)
            break;
        if(fwrite(chunk, 1, got, out) != got) {
            ok = false;
            break;
        }
    }
    if(ferror(in))
        ok = false;
    (void)fclose(in);
    return fclose(out) == 0 && ok;
}

/* Case-insensitively, because a filesystem that does not distinguish `GCC.EXE`
   from `gcc.exe` will hand back either. */
static bool ends_with_exe(const char *file, size_t length) {
    static const char suffix[] = ".exe";
    const size_t width = sizeof suffix - 1;
    if(length <= width)
        return false;
    const char *tail = file + length - width;
    for(size_t i = 0; i < width; i++) {
        if(tolower((unsigned char)tail[i]) != suffix[i])
            return false;
    }
    return true;
}

bool fs_executable_name(const char *file, char *out, size_t size) {
    const size_t length = strlen(file);
#ifdef _WIN32
    if(!ends_with_exe(file, length))
        return false;
    const size_t bare = length - (sizeof FS_EXECUTABLE_SUFFIX - 1);
#else
    (void)ends_with_exe;
    const size_t bare = length;
#endif
    if(bare >= size)
        return false;
    memcpy(out, file, bare);
    out[bare] = '\0';
    return true;
}

bool fs_executable_file(const char *name, char *out, size_t size) {
    const int written = snprintf(out, size, "%s" FS_EXECUTABLE_SUFFIX, name);
    return written > 0 && (size_t)written < size;
}

bool fs_replace(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(from, to) == 0;
#endif
}
