#include <molto/services/platform_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/host_service.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/services/source_service.h>
#include <molto/util/sha256.h>

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

/* Where a fetched platform lives under the cache root, and where every file it
   was assembled from is kept, by digest, so two recipes pinning the same
   package download it once. */
#define PLATFORMS_AREA "platforms"
#define DOWNLOADS_AREA "downloads"
#define STAMP_FILE ".molto-fetched"
#define WORK_SUFFIX ".fetching"
/* The empty root a dependency answered by the host still names. */
#define HOST_ROOT "host"

#define PLATFORM_ARRAY "platform"
#define FILE_ARRAY "file"
#define HOST_SECTION "host"

#define OS_RELEASE_PATH "/etc/os-release"

/* What ghcr.io accepts for an anonymous pull of a public Homebrew bottle: a
   token anyone may send, and the only header this form ever adds. */
#define GHCR_ANONYMOUS "Authorization: Bearer QQ=="

static bool fail(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

/* --- the recipe --- */

static void entry_init(platform_entry *entry) {
    memset(entry, 0, sizeof *entry);
    str_list_init(&entry->include);
    str_list_init(&entry->link);
    str_list_init(&entry->lib);
    str_list_init(&entry->bin);
    str_list_init(&entry->urls);
    str_list_init(&entry->digests);
}

static void entry_free(platform_entry *entry) {
    str_list_free(&entry->include);
    str_list_free(&entry->link);
    str_list_free(&entry->lib);
    str_list_free(&entry->bin);
    str_list_free(&entry->urls);
    str_list_free(&entry->digests);
}

void platform_recipe_init(platform_recipe *recipe) { memset(recipe, 0, sizeof *recipe); }

void platform_recipe_free(platform_recipe *recipe) {
    for(size_t i = 0; i < recipe->count; i++)
        entry_free(&recipe->items[i]);
    free(recipe->items);
    platform_recipe_init(recipe);
}

static const char *const OSES[] = {"linux", "macos", "windows"};
static const char *const ARCHES[] = {"x86_64", "aarch64"};

static const struct {
    const char *name;
    platform_format format;
} FORMATS[] = {
    {"deb", platform_format_deb},
    {"rpm", platform_format_rpm},
    {"pacman", platform_format_pacman},
    {"bottle", platform_format_bottle},
};

static const struct {
    const char *name;
    platform_runtime runtime;
} RUNTIMES[] = {
    {"host", platform_runtime_host},
    {"bundled", platform_runtime_bundled},
};

/* Tables a platform recipe may not carry beside its platforms. Each is the
   other forms' answer to "where do the bytes come from" or "what is compiled",
   and a recipe stating both answers is contradicting itself. */
static const char *const FOREIGN_TABLES[] = {"source", "build", "provide", "artifacts", "deps"};

static bool is_one_of(const char *value, const char *const *list, size_t count) {
    for(size_t i = 0; i < count; i++) {
        if(strcmp(value, list[i]) == 0)
            return true;
    }
    return false;
}

/* A platform name becomes a directory, so it is held to the shape of one
   segment: no separator, no `..`, nothing a shell would trip over. */
static bool is_segment(const char *name) {
    if(name[0] == '\0' || name[0] == '.')
        return false;
    for(const char *at = name; *at != '\0'; at++) {
        if(!islower((unsigned char)*at) && !isdigit((unsigned char)*at) && *at != '-' &&
           *at != '_' && *at != '.')
            return false;
    }
    return true;
}

/* Relative, and staying under the root: no leading separator, no drive, no
   `..` segment. Every path a platform names is joined onto its unpacked root,
   and one that left it would put a directory the recipe's author chose on a
   consumer's command line. */
static bool stays_inside(const char *path) {
    if(path[0] == '\0' || path[0] == '/' || path[0] == '\\' || strchr(path, ':') != NULL)
        return false;
    for(const char *at = path; *at != '\0';) {
        const size_t length = strcspn(at, "/\\");
        if(length == 2 && at[0] == '.' && at[1] == '.')
            return false;
        at += length;
        if(*at != '\0')
            at++;
    }
    return true;
}

static bool read_required(doc_view item, const char *key, char *out, size_t size, size_t index,
                          char *err, size_t err_size) {
    if(!doc_get_string(item, "", key, out, size) || out[0] == '\0')
        return fail(err, err_size, "[[platform]] #%zu has no '%s'", index + 1, key);
    return true;
}

static bool read_optional(doc_view item, const char *key, char *out, size_t size, const char *where,
                          char *err, size_t err_size) {
    if(!doc_get_string(item, "", key, out, size) && doc_has_key(item, "", key))
        return fail(err, err_size, "platform '%s': '%s' must be a string", where, key);
    return true;
}

static bool read_paths(doc_view item, const char *key, str_list *out, const char *where,
                       bool must_stay_inside, char *err, size_t err_size) {
    if(!doc_has_key(item, "", key))
        return true;
    if(!doc_get_array(item, "", key, out))
        return fail(err, err_size, "platform '%s': '%s' must be a list of strings", where, key);
    for(size_t i = 0; must_stay_inside && i < str_list_count(out); i++) {
        const char *path = str_list_get(out, i);
        if(!stays_inside(path))
            return fail(err, err_size,
                        "platform '%s': '%s' names '%s', and every path a platform names stays "
                        "inside what it unpacks",
                        where, key, path);
    }
    return true;
}

static bool is_digest(const char *text) {
    if(strlen(text) != 64)
        return false;
    for(const char *at = text; *at != '\0'; at++) {
        if(!isdigit((unsigned char)*at) && !(*at >= 'a' && *at <= 'f'))
            return false;
    }
    return true;
}

static bool read_files(doc_view item, platform_entry *entry, char *err, size_t err_size) {
    const size_t count = doc_array_len(item, FILE_ARRAY);
    if(count == 0)
        return fail(err, err_size, "platform '%s' pins no [[platform.file]]", entry->name);
    if(count > PLATFORM_MAX_FILES)
        return fail(err, err_size, "platform '%s' pins %zu files and at most %d may be pinned",
                    entry->name, count, PLATFORM_MAX_FILES);

    for(size_t i = 0; i < count; i++) {
        doc_view file;
        char url[SOURCE_URL_MAX] = "";
        char digest[SOURCE_DIGEST_MAX] = "";
        if(!doc_array_at(item, FILE_ARRAY, i, &file))
            return fail(err, err_size, "platform '%s': file #%zu is not a table", entry->name,
                        i + 1);
        if(!doc_get_string(file, "", "url", url, sizeof url) || url[0] == '\0')
            return fail(err, err_size, "platform '%s': file #%zu has no 'url'", entry->name, i + 1);
        if(strncmp(url, "https://", 8) != 0)
            return fail(err, err_size, "platform '%s': file #%zu is not fetched over https: %s",
                        entry->name, i + 1, url);
        /* A URL is a promise about a location and not about content: without
           a digest, a mirror that changed is a build that changed. */
        if(!doc_get_string(file, "", "sha256", digest, sizeof digest) || !is_digest(digest))
            return fail(err, err_size,
                        "platform '%s': file #%zu has no 'sha256', or it is not a lowercase hex "
                        "sha256 digest",
                        entry->name, i + 1);
        if(!str_list_push(&entry->urls, url) || !str_list_push(&entry->digests, digest))
            return fail(err, err_size, "out of memory reading a platform recipe");
    }
    return true;
}

static bool read_entry(doc_view item, size_t index, platform_entry *entry, char *err,
                       size_t err_size) {
    char format[PLATFORM_FIELD_MAX] = "";
    char runtime[PLATFORM_FIELD_MAX] = "";
    if(!read_required(item, "name", entry->name, sizeof entry->name, index, err, err_size))
        return false;
    if(!is_segment(entry->name))
        return fail(err, err_size,
                    "platform '%s': a name is lowercase letters, digits, '.', '-' and '_'",
                    entry->name);

    const char *where = entry->name;
    if(!read_required(item, "os", entry->os, sizeof entry->os, index, err, err_size) ||
       !read_required(item, "arch", entry->arch, sizeof entry->arch, index, err, err_size) ||
       !read_required(item, "format", format, sizeof format, index, err, err_size) ||
       !read_required(item, "runtime", runtime, sizeof runtime, index, err, err_size) ||
       !read_optional(item, "distro", entry->distro, sizeof entry->distro, where, err, err_size) ||
       !read_optional(item, "distro_version", entry->distro_version, sizeof entry->distro_version,
                      where, err, err_size) ||
       !read_optional(item, "os_version_min", entry->os_version_min, sizeof entry->os_version_min,
                      where, err, err_size) ||
       !read_optional(item, "upstream", entry->upstream, sizeof entry->upstream, where, err,
                      err_size))
        return false;

    if(!is_one_of(entry->os, OSES, sizeof OSES / sizeof OSES[0]))
        return fail(err, err_size, "platform '%s': 'os' must be linux, macos or windows, got %s",
                    where, entry->os);
    if(!is_one_of(entry->arch, ARCHES, sizeof ARCHES / sizeof ARCHES[0]))
        return fail(err, err_size, "platform '%s': 'arch' must be x86_64 or aarch64, got %s", where,
                    entry->arch);
    if(entry->distro_version[0] != '\0' && entry->distro[0] == '\0')
        return fail(err, err_size, "platform '%s': 'distro_version' without a 'distro'", where);
    if(entry->distro[0] != '\0' && strcmp(entry->os, "linux") != 0)
        return fail(err, err_size, "platform '%s': only a linux platform has a 'distro'", where);

    bool known = false;
    for(size_t i = 0; i < sizeof FORMATS / sizeof FORMATS[0]; i++) {
        if(strcmp(format, FORMATS[i].name) == 0) {
            entry->format = FORMATS[i].format;
            known = true;
        }
    }
    if(!known)
        return fail(err, err_size,
                    "platform '%s': 'format' must be deb, rpm, pacman or bottle, got %s", where,
                    format);
    known = false;
    for(size_t i = 0; i < sizeof RUNTIMES / sizeof RUNTIMES[0]; i++) {
        if(strcmp(runtime, RUNTIMES[i].name) == 0) {
            entry->runtime = RUNTIMES[i].runtime;
            known = true;
        }
    }
    if(!known)
        return fail(err, err_size, "platform '%s': 'runtime' must be host or bundled, got %s",
                    where, runtime);

    return read_paths(item, "include", &entry->include, where, true, err, err_size) &&
           read_paths(item, "link", &entry->link, where, false, err, err_size) &&
           read_paths(item, "lib", &entry->lib, where, true, err, err_size) &&
           read_paths(item, "bin", &entry->bin, where, true, err, err_size) &&
           read_files(item, entry, err, err_size);
}

/* A library name reaches a link line as `-l<name>`, so one that is not a plain
   name is a second flag smuggled in through the first. */
static bool links_are_names(const platform_entry *entry, char *err, size_t err_size) {
    for(size_t i = 0; i < str_list_count(&entry->link); i++) {
        const char *name = str_list_get(&entry->link, i);
        const char *at = name[0] == ':' ? name + 1 : name;
        bool ok = *at != '\0' && *at != '-';
        for(; ok && *at != '\0'; at++)
            ok = isalnum((unsigned char)*at) || strchr("._-+", *at) != NULL;
        if(!ok)
            return fail(err, err_size, "platform '%s': '%s' is not a library name", entry->name,
                        name);
    }
    return true;
}

bool platform_recipe_read(doc_view doc, platform_recipe *out, char *err, size_t err_size) {
    platform_recipe_init(out);

    for(size_t i = 0; i < sizeof FOREIGN_TABLES / sizeof FOREIGN_TABLES[0]; i++) {
        if(doc_has_table(doc, FOREIGN_TABLES[i]) || doc_array_len(doc, FOREIGN_TABLES[i]) > 0)
            return fail(err, err_size,
                        "a platform recipe pins its files under [[platform]] and carries no "
                        "[%s]",
                        FOREIGN_TABLES[i]);
    }

    if(!doc_get_string(doc, HOST_SECTION, "pkgconfig", out->pkgconfig, sizeof out->pkgconfig) &&
       doc_has_key(doc, HOST_SECTION, "pkgconfig"))
        return fail(err, err_size, "[host] pkgconfig must be a string");
    if(strchr(out->pkgconfig, '/') != NULL || out->pkgconfig[0] == '-')
        return fail(err, err_size, "[host] pkgconfig names a module, not a path or a flag: %s",
                    out->pkgconfig);

    const size_t count = doc_array_len(doc, PLATFORM_ARRAY);
    if(count == 0)
        return fail(err, err_size, "a platform recipe declares no [[platform]]");
    if(count > PLATFORM_MAX)
        return fail(err, err_size, "the recipe declares %zu platforms and at most %d may be", count,
                    PLATFORM_MAX);

    out->items = calloc(count, sizeof *out->items);
    if(out->items == NULL)
        return fail(err, err_size, "out of memory reading a platform recipe");

    for(size_t i = 0; i < count; i++) {
        platform_entry *entry = &out->items[i];
        entry_init(entry);
        out->count = i + 1;

        doc_view item;
        if(!doc_array_at(doc, PLATFORM_ARRAY, i, &item)) {
            platform_recipe_free(out);
            return fail(err, err_size, "[[platform]] #%zu is not a table", i + 1);
        }
        if(!read_entry(item, i, entry, err, err_size) || !links_are_names(entry, err, err_size)) {
            platform_recipe_free(out);
            return false;
        }
        for(size_t j = 0; j < i; j++) {
            if(strcmp(out->items[j].name, entry->name) == 0) {
                /* Reported before the free: the name lives in what is freed. */
                (void)fail(err, err_size, "two platforms are named '%s'", entry->name);
                platform_recipe_free(out);
                return false;
            }
        }
    }
    return true;
}

void platform_recipe_digest(const platform_recipe *recipe, char hex_out[65]) {
    sha256_state state;
    sha256_init(&state);
    for(size_t i = 0; i < recipe->count; i++) {
        const platform_entry *entry = &recipe->items[i];
        sha256_update(&state, entry->name, strlen(entry->name) + 1);
        for(size_t j = 0; j < str_list_count(&entry->digests); j++)
            sha256_update(&state, str_list_get(&entry->digests, j), 65);
    }
    sha256_finish(&state, hex_out);
}

/* --- this machine --- */

static void set_field(char *out, const char *value) {
    snprintf(out, PLATFORM_FIELD_MAX, "%s", value);
}

void platform_host_read_os_release(const char *text, platform_host *out) {
    for(const char *line = text; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        const size_t length = end == NULL ? strlen(line) : (size_t)(end - line);
        char *field = NULL;
        size_t skip = 0;
        if(strncmp(line, "ID=", 3) == 0) {
            field = out->distro;
            skip = 3;
        } else if(strncmp(line, "VERSION_ID=", 11) == 0) {
            field = out->distro_version;
            skip = 11;
        }
        if(field != NULL) {
            const char *value = line + skip;
            size_t value_length = length - skip;
            while(value_length > 0 && (value[value_length - 1] == '\r' ||
                                       isspace((unsigned char)value[value_length - 1])))
                value_length--;
            if(value_length >= 2 && (value[0] == '"' || value[0] == '\'') &&
               value[value_length - 1] == value[0]) {
                value++;
                value_length -= 2;
            }
            if(value_length >= PLATFORM_FIELD_MAX)
                value_length = PLATFORM_FIELD_MAX - 1;
            memcpy(field, value, value_length);
            field[value_length] = '\0';
        }
        line = end == NULL ? NULL : end + 1;
    }
}

void platform_host_detect(platform_host *out) {
    memset(out, 0, sizeof *out);
    /* What molto was compiled for, which is what it builds for: a platform
       recipe serves the machine it runs on, the way RFC-0016's resolver does. */
#if defined(_WIN32)
    set_field(out->os, "windows");
#elif defined(__APPLE__)
    set_field(out->os, "macos");
#else
    set_field(out->os, "linux");
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    set_field(out->arch, "aarch64");
#else
    set_field(out->arch, "x86_64");
#endif

#if defined(__APPLE__)
    char version[PLATFORM_FIELD_MAX] = "";
    size_t size = sizeof version;
    if(sysctlbyname("kern.osproductversion", version, &size, NULL, 0) == 0)
        set_field(out->os_version, version);
#elif !defined(_WIN32)
    char *text = fs_read_file(OS_RELEASE_PATH);
    if(text != NULL) {
        platform_host_read_os_release(text, out);
        free(text);
    }
#endif
}

int platform_version_compare(const char *a, const char *b) {
    while(*a != '\0' || *b != '\0') {
        char *end_a = NULL;
        char *end_b = NULL;
        const long x = *a != '\0' ? strtol(a, &end_a, 10) : 0;
        const long y = *b != '\0' ? strtol(b, &end_b, 10) : 0;
        if(x != y)
            return x < y ? -1 : 1;
        a = end_a != NULL && end_a != a ? end_a : a + strlen(a);
        b = end_b != NULL && end_b != b ? end_b : b + strlen(b);
        if(*a == '.')
            a++;
        if(*b == '.')
            b++;
    }
    return 0;
}

static bool matches(const platform_entry *entry, const platform_host *host) {
    if(strcmp(entry->os, host->os) != 0 || strcmp(entry->arch, host->arch) != 0)
        return false;
    if(entry->distro[0] != '\0' && strcmp(entry->distro, host->distro) != 0)
        return false;
    if(entry->distro_version[0] != '\0' && strcmp(entry->distro_version, host->distro_version) != 0)
        return false;
    if(entry->os_version_min[0] != '\0' && host->os_version[0] != '\0' &&
       platform_version_compare(host->os_version, entry->os_version_min) < 0)
        return false;
    return true;
}

static void describe_host(const platform_host *host, char *out, size_t size) {
    if(host->distro[0] != '\0')
        snprintf(out, size, "%s %s %s (%s)", host->distro, host->distro_version, host->arch,
                 host->os);
    else if(host->os_version[0] != '\0')
        snprintf(out, size, "%s %s %s", host->os, host->os_version, host->arch);
    else
        snprintf(out, size, "%s %s", host->os, host->arch);
}

bool platform_choose(const platform_recipe *recipe, const platform_host *host, size_t *index,
                     char *err, size_t err_size) {
    for(size_t i = 0; i < recipe->count; i++) {
        if(matches(&recipe->items[i], host)) {
            *index = i;
            return true;
        }
    }

    char served[1024] = "";
    size_t used = 0;
    for(size_t i = 0; i < recipe->count && used < sizeof served; i++) {
        const int written = snprintf(served + used, sizeof served - used, "%s%s", i > 0 ? ", " : "",
                                     recipe->items[i].name);
        if(written < 0)
            break;
        used += (size_t)written;
    }
    /* Four fields and their separators: sized so the description is never cut. */
    char here[4 * PLATFORM_FIELD_MAX + 8];
    describe_host(host, here, sizeof here);
    return fail(err, err_size, "no platform it pins matches this machine, %s; it serves %s", here,
                served);
}

/* --- the answer --- */

void platform_answer_init(platform_answer *answer) {
    memset(answer, 0, sizeof *answer);
    str_list_init(&answer->includes);
    str_list_init(&answer->links);
    str_list_init(&answer->runtime_dirs);
    str_list_init(&answer->bounds);
}

void platform_answer_free(platform_answer *answer) {
    str_list_free(&answer->includes);
    str_list_free(&answer->links);
    str_list_free(&answer->runtime_dirs);
    str_list_free(&answer->bounds);
}

static bool push(str_list *list, const char *value, char *err, size_t err_size) {
    if(!str_list_push(list, value))
        return fail(err, err_size, "out of memory describing a platform package");
    return true;
}

static bool push_format(str_list *list, char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static bool push_format(str_list *list, char *err, size_t err_size, const char *format, ...) {
    char value[PLATFORM_PATH_MAX + 32];
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(value, sizeof value, format, args);
    va_end(args);
    if(written < 0 || (size_t)written >= sizeof value)
        return fail(err, err_size, "a path in a platform package is too long");
    return push(list, value, err, err_size);
}

/* The coordinate's directory, one segment at a time, each checked: a name or a
   version reaches here from a registry, and one spelled `..` would put a fetch
   wherever it liked. */
static bool platform_dir(const char *name, const char *version, const char *platform, char *out,
                         size_t size, char *err, size_t err_size) {
    char cache[PLATFORM_PATH_MAX];
    if(!source_cache_root(cache, sizeof cache))
        return fail(err, err_size,
                    "this machine has no home directory, so there is nowhere to cache a "
                    "platform package; set MOLTO_HOME or MOLTO_CACHE");
    if(!is_segment(name) || !is_segment(platform) || version[0] == '\0' ||
       strchr(version, '/') != NULL || strstr(version, "..") != NULL)
        return fail(err, err_size, "'%s %s' cannot name a directory in the cache", name, version);
    if(!fs_format_path(out, size, "%s/" PLATFORMS_AREA "/%s/%s/%s", cache, name, version, platform))
        return fail(err, err_size, "the cache path for %s is too long", name);
    return true;
}

/* Whatever pkg-config says about the module on this machine, as the answer.
   `*found` is false, and the call succeeds, when the host simply does not have
   it: that is the case the platforms below are for, and not an error. */
static bool answer_from_host(const char *module, const char *name, const char *version,
                             platform_answer *out, bool *found, char *err, size_t err_size) {
    host_answer *host = calloc(1, sizeof *host);
    if(host == NULL)
        return fail(err, err_size, "out of memory asking the host");
    char ignored[256] = "";
    *found = host_resolve(module, host, ignored, sizeof ignored);
    bool ok = true;
    if(*found) {
        set_field(out->answered, HOST_RESOLVER_NAME);
        set_field(out->upstream, host->version);
        ok = platform_dir(name, version, HOST_ROOT, out->root, sizeof out->root, err, err_size) &&
             (fs_make_dirs(out->root) || fail(err, err_size, "could not create %s", out->root));
        for(size_t i = 0; ok && i < host->include_count; i++)
            ok = push(&out->includes, host->includes[i], err, err_size) &&
                 push(&out->bounds, host->includes[i], err, err_size);
        for(size_t i = 0; ok && i < host->link_count; i++)
            ok = push(&out->links, host->links[i], err, err_size);
    }
    free(host);
    return ok;
}

/* --- fetching --- */

static bool run_quietly(const char *const argv[], char *err, size_t err_size, const char *what) {
    char output[2048] = "";
    process_spec spec = {0};
    spec.stdout_to = process_stream_capture;
    spec.stderr_to = process_stream_capture;
    spec.capture = output;
    spec.capture_size = sizeof output;
    const int code = process_execute(argv, &spec);
    if(code == 127)
        return fail(err, err_size, "%s is not installed, and molto needs it to %s", argv[0], what);
    if(code != 0) {
        output[strcspn(output, "\n")] = '\0';
        return fail(err, err_size, "%s failed to %s%s%s", argv[0], what,
                    output[0] != '\0' ? ": " : "", output);
    }
    return true;
}

static bool download(const char *url, const char *file, bool bottle, char *err, size_t err_size) {
    const char *plain[] = {"curl", "--fail", "--silent", "--show-error", "--location", "--output",
                           file,   url,      NULL};
    const char *ghcr[] = {"curl",     "--fail",       "--silent", "--show-error", "--location",
                          "--header", GHCR_ANONYMOUS, "--output", file,           url,
                          NULL};
    return run_quietly(bottle ? ghcr : plain, err, err_size, "download a platform package");
}

/* One pinned file, fetched once into the shared downloads area and verified
   every time it is used: a file that is in the cache under its digest but does
   not hash to it is fetched again rather than trusted. */
static bool obtain(const char *url, const char *digest, bool bottle, char *out, size_t size,
                   char *err, size_t err_size) {
    char cache[PLATFORM_PATH_MAX];
    char area[PLATFORM_PATH_MAX];
    if(!source_cache_root(cache, sizeof cache) ||
       !fs_format_path(area, sizeof area, "%s/" DOWNLOADS_AREA, cache) ||
       !fs_format_path(out, size, "%s/%s", area, digest))
        return fail(err, err_size, "there is no cache to download into");
    if(!fs_make_dirs(area))
        return fail(err, err_size, "could not create %s", area);

    char actual[65] = "";
    if(fs_path_exists(out) && sha256_file(out, actual) && strcmp(actual, digest) == 0)
        return true;

    char partial[PLATFORM_PATH_MAX];
    if(!fs_format_path(partial, sizeof partial, "%s.part", out))
        return fail(err, err_size, "the download path is too long");
    (void)remove(partial);
    if(!download(url, partial, bottle, err, err_size))
        return false;
    if(!sha256_file(partial, actual) || strcmp(actual, digest) != 0) {
        (void)remove(partial);
        return fail(err, err_size, "%s hashes to %s, and the recipe expects %s", url, actual,
                    digest);
    }
    if(!fs_replace(partial, out))
        return fail(err, err_size, "could not keep the download of %s", url);
    return true;
}

/* A whole file, for the formats molto reads itself. */
static unsigned char *read_binary(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return NULL;
    unsigned char *data = NULL;
    const long length = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    if(length >= 0 && fseek(file, 0, SEEK_SET) == 0) {
        data = malloc((size_t)length + 1);
        if(data != NULL && fread(data, 1, (size_t)length, file) != (size_t)length) {
            free(data);
            data = NULL;
        }
    }
    fclose(file);
    if(data != NULL)
        *size = (size_t)length;
    return data;
}

static bool write_binary(const char *path, const unsigned char *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if(file == NULL)
        return false;
    const bool ok = fwrite(data, 1, size, file) == size;
    return fclose(file) == 0 && ok;
}

static bool untar(const char *archive, const char *into, bool strip_keg, char *err,
                  size_t err_size) {
    const char *plain[] = {"tar", "-xf", archive, "-C", into, NULL};
    /* A bottle wraps its keg in <formula>/<version>/; dropping both merges
       every keg into one prefix, which is what `brew link` would have done. */
    const char *keg[] = {"tar", "-xf", archive, "-C", into, "--strip-components=2", NULL};
    return run_quietly(strip_keg ? keg : plain, err, err_size, "unpack a platform package");
}

/* --- zstd --- */

/*
 * Ubuntu's .debs, Fedora's .rpms, and every Arch and MSYS2 package are
 * compressed with zstd, and which `tar` can read it is a property of how that
 * tar was built: macOS's and Windows' bsdtar hand it to a `zstd` program, GNU
 * tar always does. So molto decompresses zstd itself, with that one program,
 * and gives tar a plain archive.
 *
 * Every Linux this form serves has `zstd` in its base install — dpkg, rpm and
 * pacman all need it — and on Windows it is downloaded once, from the zstd
 * project's own release, into molto's cache: pinned by digest like every other
 * file molto fetches, and shared by every project on the machine.
 */
#define ZSTD_ENV "MOLTO_ZSTD"
#define ZSTD_TOOL_DIR "tools/zstd-1.5.7"
#define ZSTD_WINDOWS_URL                                                                           \
    "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-v1.5.7-win64.zip"
#define ZSTD_WINDOWS_SHA256 "acb4e8111511749dc7a3ebedca9b04190e37a17afeb73f55d4425dbf0b90fad9"

static const unsigned char ZSTD_MAGIC[] = {0x28, 0xb5, 0x2f, 0xfd};

static bool answers(const char *program) {
    const char *argv[] = {program, "--version", NULL};
    char ignored[256] = "";
    return run_quietly(argv, ignored, sizeof ignored, "report its version");
}

static bool cached_zstd(char *out, size_t size, char *err, size_t err_size) {
    char cache[PLATFORM_PATH_MAX];
    char directory[PLATFORM_PATH_MAX];
    if(!source_cache_root(cache, sizeof cache) ||
       !fs_format_path(directory, sizeof directory, "%s/" ZSTD_TOOL_DIR, cache))
        return fail(err, err_size, "there is no cache to keep zstd in");
#ifdef _WIN32
    if(!fs_format_path(out, size, "%s/zstd.exe", directory))
        return fail(err, err_size, "the zstd path is too long");
    if(fs_path_exists(out))
        return true;
    char archive[PLATFORM_PATH_MAX];
    if(!obtain(ZSTD_WINDOWS_URL, ZSTD_WINDOWS_SHA256, false, archive, sizeof archive, err,
               err_size))
        return false;
    if(!fs_make_dirs(directory))
        return fail(err, err_size, "could not create %s", directory);
    /* Windows' own tar reads a zip; the release wraps zstd.exe in a folder. */
    const char *argv[] = {"tar", "-xf", archive, "-C", directory, "--strip-components=1", NULL};
    if(!run_quietly(argv, err, err_size, "unpack zstd"))
        return false;
    return fs_path_exists(out) || fail(err, err_size, "the zstd release holds no zstd.exe");
#else
    (void)out;
    (void)size;
    return fail(err, err_size,
                "zstd is not installed, and the packages this platform pins are compressed "
                "with it; it is part of every distribution's base install — install zstd, or "
                "name one with " ZSTD_ENV);
#endif
}

/* The zstd molto runs: $MOLTO_ZSTD, then the one on PATH, then its own. */
static bool zstd_program(char *out, size_t size, char *err, size_t err_size) {
    static char found[PLATFORM_PATH_MAX];
    if(found[0] == '\0') {
        const char *chosen = getenv(ZSTD_ENV);
        if(chosen != NULL && chosen[0] != '\0')
            snprintf(found, sizeof found, "%s", chosen);
        else if(answers("zstd"))
            snprintf(found, sizeof found, "zstd");
        else if(!cached_zstd(found, sizeof found, err, err_size)) {
            found[0] = '\0';
            return false;
        }
    }
    snprintf(out, size, "%s", found);
    return true;
}

static bool starts_with_zstd(const char *path) {
    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return false;
    unsigned char magic[sizeof ZSTD_MAGIC] = {0};
    const bool read = fread(magic, 1, sizeof magic, file) == sizeof magic;
    fclose(file);
    return read && memcmp(magic, ZSTD_MAGIC, sizeof ZSTD_MAGIC) == 0;
}

static bool unzstd(const char *packed, const char *out, char *err, size_t err_size) {
    char zstd[PLATFORM_PATH_MAX];
    if(!zstd_program(zstd, sizeof zstd, err, err_size))
        return false;
    const char *argv[] = {zstd, "-d", "-q", "-f", "-o", out, packed, NULL};
    return run_quietly(argv, err, err_size, "decompress a platform package");
}

/* A tarball in whatever compression its platform chose. */
static bool unpack_tarball(const char *archive, const char *work, const char *into, bool strip_keg,
                           char *err, size_t err_size) {
    if(!starts_with_zstd(archive))
        return untar(archive, into, strip_keg, err, err_size);
    char plain[PLATFORM_PATH_MAX];
    if(!fs_format_path(plain, sizeof plain, "%s/plain.tar", work))
        return fail(err, err_size, "the work path is too long");
    const bool ok =
        unzstd(archive, plain, err, err_size) && untar(plain, into, strip_keg, err, err_size);
    (void)remove(plain);
    return ok;
}

static bool unpack_deb(const char *file, const char *work, const char *root, char *err,
                       size_t err_size) {
    size_t size = 0;
    unsigned char *data = read_binary(file, &size);
    if(data == NULL)
        return fail(err, err_size, "could not read %s", file);
    size_t offset = 0;
    size_t length = 0;
    char member[PLATFORM_PATH_MAX];
    bool ok = platform_ar_member(data, size, "data.tar", &offset, &length) ||
              fail(err, err_size, "%s is not a .deb: it holds no data.tar", file);
    ok = ok && (fs_format_path(member, sizeof member, "%s/data.tar.member", work) ||
                fail(err, err_size, "the work path is too long"));
    ok = ok && (write_binary(member, data + offset, length) ||
                fail(err, err_size, "could not write %s", member));
    free(data);
    ok = ok && unpack_tarball(member, work, root, false, err, err_size);
    (void)remove(member);
    return ok;
}

/* An rpm's payload is a cpio in zstd, xz or gzip — zstd since Fedora 31. The
   other two are left to the programs every system that ships them has. */
static bool decompress_payload(const char *work, const unsigned char *payload, size_t size,
                               char *out, size_t out_size, char *err, size_t err_size) {
    static const unsigned char XZ[] = {0xfd, '7', 'z', 'X', 'Z', 0x00};
    static const unsigned char GZIP[] = {0x1f, 0x8b};

    const char *suffix = NULL;
    if(size >= sizeof ZSTD_MAGIC && memcmp(payload, ZSTD_MAGIC, sizeof ZSTD_MAGIC) == 0)
        suffix = ".zst";
    else if(size >= sizeof XZ && memcmp(payload, XZ, sizeof XZ) == 0)
        suffix = ".xz";
    else if(size >= sizeof GZIP && memcmp(payload, GZIP, sizeof GZIP) == 0)
        suffix = ".gz";
    else
        return fail(err, err_size, "an rpm payload compressed with something molto cannot read");

    char packed[PLATFORM_PATH_MAX];
    if(!fs_format_path(out, out_size, "%s/payload.cpio", work) ||
       !fs_format_path(packed, sizeof packed, "%s%s", out, suffix))
        return fail(err, err_size, "the work path is too long");
    if(!write_binary(packed, payload, size))
        return fail(err, err_size, "could not write %s", packed);

    if(strcmp(suffix, ".zst") == 0) {
        const bool ok = unzstd(packed, out, err, err_size);
        (void)remove(packed);
        return ok;
    }
    const char *xz[] = {"xz", "-d", "-f", "-q", packed, NULL};
    const char *gzip[] = {"gzip", "-d", "-f", "-q", packed, NULL};
    return run_quietly(strcmp(suffix, ".xz") == 0 ? xz : gzip, err, err_size, "decompress an rpm");
}

static bool unpack_rpm(const char *file, const char *work, const char *root, char *err,
                       size_t err_size) {
    size_t size = 0;
    unsigned char *data = read_binary(file, &size);
    if(data == NULL)
        return fail(err, err_size, "could not read %s", file);
    size_t offset = 0;
    char cpio[PLATFORM_PATH_MAX];
    bool ok = platform_rpm_payload(data, size, &offset) ||
              fail(err, err_size, "%s is not an rpm molto can read", file);
    ok = ok &&
         decompress_payload(work, data + offset, size - offset, cpio, sizeof cpio, err, err_size);
    free(data);
    if(!ok)
        return false;

    unsigned char *archive = read_binary(cpio, &size);
    if(archive == NULL)
        return fail(err, err_size, "could not read %s", cpio);
    ok = platform_cpio_unpack(archive, size, root, err, err_size);
    free(archive);
    (void)remove(cpio);
    return ok;
}

/* --- relocating a bottle --- */

static bool is_macho(const char *path) {
    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return false;
    unsigned char magic[4] = {0};
    const bool read = fread(magic, 1, sizeof magic, file) == sizeof magic;
    fclose(file);
    /* MH_MAGIC_64, as a little-endian machine writes it. */
    return read && magic[0] == 0xcf && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe;
}

static bool relocate_file(const char *path, char *err, size_t err_size) {
    if(!is_macho(path))
        return true;
    size_t size = 0;
    unsigned char *data = read_binary(path, &size);
    if(data == NULL)
        return fail(err, err_size, "could not read %s", path);
    bool changed = false;
    bool ok = platform_macho_relocate(data, size, &changed) ||
              fail(err, err_size, "%s is a Mach-O molto cannot relocate", path);
    if(ok && changed) {
        /* Bottles ship read-only; the copy in the cache is molto's to edit. */
        (void)chmod(path, 0755);
        ok = write_binary(path, data, size) || fail(err, err_size, "could not write %s", path);
        /* A changed load command no longer matches the signature, and Apple
           silicon refuses to load an unsigned or mis-signed library. */
        const char *argv[] = {"/usr/bin/codesign", "--force", "--sign", "-", path, NULL};
        ok = ok && run_quietly(argv, err, err_size, "sign a relocated library");
    }
    free(data);
    return ok;
}

static bool relocate_tree(const char *directory, char *err, size_t err_size) {
    DIR *handle = opendir(directory);
    if(handle == NULL)
        return true;
    bool ok = true;
    const struct dirent *entry;
    while(ok && (entry = readdir(handle)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[PLATFORM_PATH_MAX];
        if(!fs_format_path(path, sizeof path, "%s/%s", directory, entry->d_name)) {
            ok = fail(err, err_size, "a path under %s is too long", directory);
            break;
        }
        if(fs_is_dir_no_follow(path))
            ok = relocate_tree(path, err, err_size);
        else if(!fs_is_dir(path))
            ok = relocate_file(path, err, err_size);
    }
    closedir(handle);
    return ok;
}

/* --- assembling one platform --- */

static void announce(const char *name, const platform_entry *entry) {
    fprintf(stderr, "molto: fetching %s for %s (%zu files)\n", name, entry->name,
            str_list_count(&entry->urls));
}

static bool assemble(const platform_entry *entry, const char *work, const char *root, char *err,
                     size_t err_size) {
    if(!fs_make_dirs(root))
        return fail(err, err_size, "could not create %s", root);
    const bool bottle = entry->format == platform_format_bottle;
    for(size_t i = 0; i < str_list_count(&entry->urls); i++) {
        char file[PLATFORM_PATH_MAX];
        if(!obtain(str_list_get(&entry->urls, i), str_list_get(&entry->digests, i), bottle, file,
                   sizeof file, err, err_size))
            return false;
        bool ok = true;
        switch(entry->format) {
        case platform_format_deb:
            ok = unpack_deb(file, work, root, err, err_size);
            break;
        case platform_format_rpm:
            ok = unpack_rpm(file, work, root, err, err_size);
            break;
        case platform_format_pacman:
            ok = unpack_tarball(file, work, root, false, err, err_size);
            break;
        case platform_format_bottle:
            ok = untar(file, root, true, err, err_size);
            break;
        }
        if(!ok)
            return false;
    }
    if(!bottle)
        return true;
    for(size_t i = 0; i < str_list_count(&entry->lib); i++) {
        char lib[PLATFORM_PATH_MAX];
        if(!fs_format_path(lib, sizeof lib, "%s/%s", root, str_list_get(&entry->lib, i)))
            return fail(err, err_size, "the lib path is too long");
        if(!relocate_tree(lib, err, err_size))
            return false;
    }
    return true;
}

static bool fetch_platform(const platform_entry *entry, const char *name, const char *destination,
                           char *err, size_t err_size) {
    char stamp[PLATFORM_PATH_MAX];
    if(!fs_format_path(stamp, sizeof stamp, "%s/" STAMP_FILE, destination))
        return fail(err, err_size, "the cache path is too long");
    if(fs_path_exists(stamp))
        return true;

    char work[PLATFORM_PATH_MAX];
    char root[PLATFORM_PATH_MAX];
    if(!fs_format_path(work, sizeof work, "%s" WORK_SUFFIX, destination) ||
       !fs_format_path(root, sizeof root, "%s/root", work))
        return fail(err, err_size, "the cache path is too long");
    if(!fs_remove_tree(work))
        return fail(err, err_size, "could not clear %s", work);

    announce(name, entry);
    bool ok = assemble(entry, work, root, err, err_size);
    /* Moved into place in one rename and stamped after, so a reader never
       sees a platform half unpacked as a complete one. */
    if(ok && (!fs_remove_tree(destination) || rename(root, destination) != 0))
        ok = fail(err, err_size, "could not move the unpacked files into %s", destination);
    if(ok && !fs_write_file(stamp, "ok\n"))
        ok = fail(err, err_size, "could not record that %s was fetched", destination);
    (void)fs_remove_tree(work);
    return ok;
}

/* --- a runtime the host has to have --- */

static const char *const HOST_LIBRARY_DIRS[] = {
    "/usr/lib/x86_64-linux-gnu",
    "/lib/x86_64-linux-gnu",
    "/usr/lib/aarch64-linux-gnu",
    "/lib/aarch64-linux-gnu",
    "/usr/lib64",
    "/lib64",
    "/usr/lib",
    "/lib",
};

/* Where the host keeps `file`, or false. */
static bool host_library_path(const char *file, char *out, size_t size) {
    for(size_t i = 0; i < sizeof HOST_LIBRARY_DIRS / sizeof HOST_LIBRARY_DIRS[0]; i++) {
        if(fs_format_path(out, size, "%s/%s", HOST_LIBRARY_DIRS[i], file) && fs_path_exists(out))
            return true;
    }
    return false;
}

static bool host_has_library(const char *file) {
    char path[PLATFORM_PATH_MAX];
    return host_library_path(file, path, sizeof path);
}

/*
 * A development package's `libssl.so` is a relative link to `libssl.so.3`,
 * which the runtime package installs beside it — and on a `host` platform
 * molto pins the first and never the second, so the link dangles. A linker
 * given `-l:libssl.so.3` does not care; a configuration does: CMake's
 * FindOpenSSL picks the dangling `libssl.so`, every function check links
 * against nothing, and libwebsockets is configured for an OpenSSL from 2008.
 *
 * So each link the package pins to one of the sonames it links by is pointed
 * at the host's own file, which is exactly what installing the runtime package
 * would have done. Done on every resolve, and nothing when it already points
 * there.
 */
static bool point_links_at_host(const platform_entry *entry, const char *root, char *err,
                                size_t err_size) {
    str_list files;
    str_list_init(&files);
    bool listed = false;
    for(size_t i = 0; i < str_list_count(&entry->link); i++) {
        const char *library = str_list_get(&entry->link, i);
        char host[PLATFORM_PATH_MAX];
        if(library[0] != ':' || !host_library_path(library + 1, host, sizeof host))
            continue;
        if(!listed) {
            listed = true;
            if(!source_discovery_collect_all(root, &files)) {
                str_list_free(&files);
                return fail(err, err_size, "could not read %s", root);
            }
        }
        /* `libssl.so.3` is linked to by `libssl.so`: the name up to `.so`. */
        const char *so = strstr(library + 1, ".so");
        char dev[PLATFORM_FIELD_MAX];
        snprintf(dev, sizeof dev, "%.*s",
                 so != NULL ? (int)(so - library - 1) + 3 : (int)strlen(library + 1), library + 1);
        for(size_t j = 0; j < str_list_count(&files); j++) {
            const char *path = str_list_get(&files, j);
            const char *slash = strrchr(path, '/');
            const char *base = slash != NULL ? slash + 1 : path;
            char target[PLATFORM_PATH_MAX];
            if(strcmp(base, dev) != 0 || !fs_link_target(path, target, sizeof target) ||
               strcmp(target, host) == 0)
                continue;
            if(remove(path) != 0 || !fs_link(host, path)) {
                str_list_free(&files);
                return fail(err, err_size, "could not point %s at %s", path, host);
            }
        }
    }
    str_list_free(&files);
    return true;
}

/* A `host` platform links against the host's libraries by soname, so each one
   has to be there — checked now, naming the library, rather than left to a
   linker that would name a file and nothing that explains it. */
static bool host_runtime_present(const platform_entry *entry, const char *name, char *err,
                                 size_t err_size) {
    for(size_t i = 0; i < str_list_count(&entry->link); i++) {
        const char *library = str_list_get(&entry->link, i);
        if(library[0] != ':' || host_has_library(library + 1))
            continue;
        return fail(err, err_size,
                    "%s links against this machine's own %s, and it is not installed; molto "
                    "downloads %s's headers but never its runtime on %s — install it with the "
                    "system's package manager",
                    name, library + 1, name, entry->name);
    }
    return true;
}

static bool describe(const platform_entry *entry, const char *root, platform_answer *out, char *err,
                     size_t err_size) {
    set_field(out->answered, entry->name);
    set_field(out->upstream, entry->upstream);
    snprintf(out->root, sizeof out->root, "%s", root);

    for(size_t i = 0; i < str_list_count(&entry->include); i++) {
        if(!push_format(&out->includes, err, err_size, "%s/%s", root,
                        str_list_get(&entry->include, i)))
            return false;
    }
    const bool bundled = entry->runtime == platform_runtime_bundled;
    for(size_t i = 0; bundled && i < str_list_count(&entry->lib); i++) {
        const char *lib = str_list_get(&entry->lib, i);
        if(!push_format(&out->links, err, err_size, "-L%s/%s", root, lib))
            return false;
        /* Windows has no rpath: it looks beside the executable, which is what
           `bin` is copied for. */
        if(strcmp(entry->os, "windows") != 0 &&
           !push_format(&out->links, err, err_size, "-Wl,-rpath,%s/%s", root, lib))
            return false;
    }
    for(size_t i = 0; i < str_list_count(&entry->link); i++) {
        if(!push_format(&out->links, err, err_size, "-l%s", str_list_get(&entry->link, i)))
            return false;
    }
    for(size_t i = 0; i < str_list_count(&entry->bin); i++) {
        if(!push_format(&out->runtime_dirs, err, err_size, "%s/%s", root,
                        str_list_get(&entry->bin, i)))
            return false;
    }
    return true;
}

bool platform_resolve(const platform_recipe *recipe, const char *name, const char *version,
                      platform_answer *out, char *err, size_t err_size) {
    /* The host first: a machine that has the library with its headers builds
       against its own, which is what its owner expects and costs no download. */
    if(recipe->pkgconfig[0] != '\0') {
        bool found = false;
        if(!answer_from_host(recipe->pkgconfig, name, version, out, &found, err, err_size))
            return false;
        if(found)
            return true;
    }

    platform_host host;
    platform_host_detect(&host);
    size_t index = 0;
    if(!platform_choose(recipe, &host, &index, err, err_size))
        return false;
    const platform_entry *entry = &recipe->items[index];

    if(entry->runtime == platform_runtime_host && !host_runtime_present(entry, name, err, err_size))
        return false;

    char destination[PLATFORM_PATH_MAX];
    return platform_dir(name, version, entry->name, destination, sizeof destination, err,
                        err_size) &&
           fetch_platform(entry, name, destination, err, err_size) &&
           (entry->runtime != platform_runtime_host ||
            point_links_at_host(entry, destination, err, err_size)) &&
           describe(entry, destination, out, err, err_size);
}

/* --- runtime beside an executable --- */

static bool ends_with_dll(const char *name) {
    const size_t length = strlen(name);
    if(length < 4)
        return false;
    const char *suffix = name + length - 4;
    return suffix[0] == '.' && tolower((unsigned char)suffix[1]) == 'd' &&
           tolower((unsigned char)suffix[2]) == 'l' && tolower((unsigned char)suffix[3]) == 'l';
}

bool platform_copy_runtime(const str_list *dirs, const char *destination, char *err,
                           size_t err_size) {
    for(size_t i = 0; i < str_list_count(dirs); i++) {
        const char *directory = str_list_get(dirs, i);
        DIR *handle = opendir(directory);
        if(handle == NULL)
            continue;
        bool ok = true;
        const struct dirent *entry;
        while(ok && (entry = readdir(handle)) != NULL) {
            if(!ends_with_dll(entry->d_name))
                continue;
            char from[PLATFORM_PATH_MAX];
            char to[PLATFORM_PATH_MAX];
            ok = fs_format_path(from, sizeof from, "%s/%s", directory, entry->d_name) &&
                 fs_format_path(to, sizeof to, "%s/%s", destination, entry->d_name);
            if(!ok)
                ok = fail(err, err_size, "a runtime path under %s is too long", directory);
            else if(fs_path_exists(to) && !fs_source_newer(from, to))
                continue;
            else if(!fs_copy_file(from, to))
                ok = fail(err, err_size, "could not copy %s beside the executable", from);
        }
        closedir(handle);
        if(!ok)
            return false;
    }
    return true;
}
