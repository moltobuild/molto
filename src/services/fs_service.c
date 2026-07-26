#include <molto/services/fs_service.h>

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
