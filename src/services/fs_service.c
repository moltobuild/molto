#include <molto/services/fs_service.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

bool fs_path_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

bool fs_make_dir(const char *path) {
    if (mkdir(path, 0755) == 0)
        return true;
    /* Treat an already existing directory as success. */
    struct stat info;
    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode))
        return true;
    return false;
}

bool fs_write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");
    if (file == NULL)
        return false;
    size_t length = strlen(content);
    size_t written = fwrite(content, 1, length, file);
    int closed = fclose(file);
    return written == length && closed == 0;
}

char *fs_read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
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

bool fs_make_dirs(const char *path) {
    char buffer[4096];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof buffer)
        return false;
    memcpy(buffer, path, length + 1);
    /* Create each intermediate component in turn. */
    for (size_t i = 1; i < length; i++) {
        if (buffer[i] != '/')
            continue;
        buffer[i] = '\0';
        if (!fs_make_dir(buffer))
            return false;
        buffer[i] = '/';
    }
    return fs_make_dir(buffer);
}

bool fs_source_newer(const char *source, const char *target) {
    struct stat target_info;
    if (stat(target, &target_info) != 0)
        return true;
    struct stat source_info;
    if (stat(source, &source_info) != 0)
        return true;
    return source_info.st_mtime > target_info.st_mtime;
}
