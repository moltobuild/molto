#include <molto/services/fs_service.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

bool fs_path_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

bool fs_make_dir(const char *path) {
    if(mkdir(path, 0755) == 0)
        return true;
    /* Treat an already existing directory as success. */
    struct stat info;
    if(stat(path, &info) == 0 && S_ISDIR(info.st_mode))
        return true;
    return false;
}

bool fs_write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");
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

bool fs_is_dir(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool fs_is_dir_no_follow(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0 && S_ISDIR(info.st_mode);
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
        if(!fs_make_dir(buffer))
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

/* Nanoseconds in one second, for composing a timestamp. */
#define NANOS_PER_SECOND 1000000000LL

bool fs_mtime_ns(const char *path, int64_t *out) {
    struct stat info;
    if(stat(path, &info) != 0)
        return false;
    *out = (int64_t)info.st_mtim.tv_sec * NANOS_PER_SECOND + (int64_t)info.st_mtim.tv_nsec;
    return true;
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
