#include <molto/services/source_discovery.h>

#include <molto/services/fs_service.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

static bool has_suffix(const char *text, const char *suffix) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > text_len)
        return false;
    return strcmp(text + text_len - suffix_len, suffix) == 0;
}

bool source_is_cpp(const char *path) {
    return has_suffix(path, ".cpp") || has_suffix(path, ".cc");
}

static bool is_source(const char *path) {
    return has_suffix(path, ".c") || source_is_cpp(path);
}

/* Size of the buffer used to compose the path of a discovered entry. */
#define DISCOVERY_PATH_SIZE 4096

bool source_discovery_collect(const char *root, str_list *out) {
    DIR *dir = opendir(root);
    if (dir == NULL)
        return false;
    bool ok = true;
    const struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[DISCOVERY_PATH_SIZE];
        if (!fs_format_path(path, sizeof path, "%s/%s", root, entry->d_name)) {
            ok = false;
            break;
        }
        if (fs_is_dir(path))
            ok = source_discovery_collect(path, out);
        else if (is_source(entry->d_name))
            ok = str_list_push(out, path);
    }
    closedir(dir);
    return ok;
}
