#include <molto/services/source_discovery.h>

#include <molto/services/fs_service.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

static bool has_suffix(const char *text, const char *suffix) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if(suffix_len > text_len)
        return false;
    return strcmp(text + text_len - suffix_len, suffix) == 0;
}

bool source_is_cpp(const char *path) { return has_suffix(path, ".cpp") || has_suffix(path, ".cc"); }

static bool is_source(const char *path) { return has_suffix(path, ".c") || source_is_cpp(path); }

bool source_is_header(const char *path) {
    return has_suffix(path, ".h") || has_suffix(path, ".hpp") || has_suffix(path, ".hh");
}

/* What a style tool has an opinion about. Wider than what the build compiles:
   a header is not a translation unit, but it is code, and RFC-0005 is explicit
   that style applies to it just as much. */
static bool is_styleable(const char *path) { return is_source(path) || source_is_header(path); }

/* Size of the buffer used to compose the path of a discovered entry. */
#define DISCOVERY_PATH_SIZE 4096

/* Walk `root`, appending every file `wanted` accepts. Symlinked directories are
   not descended into: a cycle of links would recurse forever. Linked files are
   still collected — following one is bounded, walking one is not. */
static bool collect_recursive(const char *root, bool (*wanted)(const char *), str_list *out) {
    DIR *dir = opendir(root);
    if(dir == NULL)
        return false;
    bool ok = true;
    const struct dirent *entry;
    while(ok && (entry = readdir(dir)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[DISCOVERY_PATH_SIZE];
        if(!fs_format_path(path, sizeof path, "%s/%s", root, entry->d_name)) {
            ok = false;
            break;
        }
        if(fs_is_dir_no_follow(path))
            ok = collect_recursive(path, wanted, out);
        else if(!fs_is_dir(path) && wanted(entry->d_name))
            ok = str_list_push(out, path);
    }
    closedir(dir);
    return ok;
}

/* Walk and sort. Directory order varies between filesystems and would leak into
   the order of the objects on the link line, or of the diagnostics in a report;
   sorting keeps both reproducible. */
static bool collect_sorted(const char *root, bool (*wanted)(const char *), str_list *out) {
    if(!collect_recursive(root, wanted, out))
        return false;
    str_list_sort(out);
    return true;
}

bool source_discovery_collect(const char *root, str_list *out) {
    return collect_sorted(root, is_source, out);
}

bool source_discovery_collect_styleable(const char *root, str_list *out) {
    return collect_sorted(root, is_styleable, out);
}
