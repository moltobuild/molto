#include <molto/services/object_cache.h>

#include <molto/services/fs_service.h>
#include <molto/services/source_service.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The directory inside the source cache that marks a dependency's tree. Only
   what lives under it is covered. */
#define SOURCES_DIR "/sources/"
#define OBJECTS_DIR "objects"

/* Arguments whose value names where output goes. They are what differs between
   two projects compiling the same thing, so the key is taken with them and
   their values removed. */
static const char *const OUTPUT_ARGS[] = {"-o", "-MF"};

/* FNV-1a 64, the same hash the workspace database uses on file contents. */
#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static void hash_text(const char *text, size_t length, uint64_t *hash) {
    for(size_t i = 0; i < length; i++) {
        *hash ^= (unsigned char)text[i];
        *hash *= FNV_PRIME;
    }
}

/* Whether the token starting at `token` is one of the output arguments. */
static const char *output_arg_at(const char *token, size_t length) {
    for(size_t i = 0; i < sizeof OUTPUT_ARGS / sizeof OUTPUT_ARGS[0]; i++) {
        if(strlen(OUTPUT_ARGS[i]) == length && strncmp(token, OUTPUT_ARGS[i], length) == 0)
            return OUTPUT_ARGS[i];
    }
    return NULL;
}

/* Hash `command`, skipping each output argument and the value after it, and
   then taking everything from OBJECT_CACHE_ENV_MARK onwards verbatim.

   The argv is a space-joined one and no path molto composes contains a space,
   so splitting on spaces recovers the tokens it was built from. That reasoning
   stops at the mark: what follows is an environment, whose values molto did not
   compose and may well contain spaces, so it is hashed as it stands.

   The mark is also a delimiter for the argv part, not only its terminator. It
   is written flush against the last token, and a scan that only knew about
   spaces would swallow the whole environment into that token. */
static uint64_t hash_command(const char *command) {
    uint64_t hash = FNV_OFFSET;
    bool skip_value = false;
    const char *token = command;

    while(*token != '\0' && *token != OBJECT_CACHE_ENV_MARK[0]) {
        const size_t length = strcspn(token, " " OBJECT_CACHE_ENV_MARK);
        if(skip_value) {
            skip_value = false;
        } else if(output_arg_at(token, length) != NULL) {
            skip_value = true;
        } else {
            hash_text(token, length, &hash);
            hash_text(" ", 1, &hash);
        }
        token += length;
        while(*token == ' ')
            token++;
    }
    if(*token != '\0')
        hash_text(token, strlen(token), &hash);
    return hash;
}

bool object_cache_covers(const char *source) {
    char sources_root[OBJECT_CACHE_PATH_MAX];
    /* Composed through the source cache itself, so the two cannot disagree
       about where a dependency's tree lives. */
    if(!source_cache_path("n", "v", "t", sources_root, sizeof sources_root))
        return false;
    char *marker = strstr(sources_root, SOURCES_DIR);
    if(marker == NULL)
        return false;
    marker[strlen(SOURCES_DIR)] = '\0';

    return strncmp(source, sources_root, strlen(sources_root)) == 0;
}

bool object_cache_path(const char *source, const char *command, char *out, size_t size) {
    if(!object_cache_covers(source))
        return false;

    char sources_root[OBJECT_CACHE_PATH_MAX];
    if(!source_cache_path("n", "v", "t", sources_root, sizeof sources_root))
        return false;
    char *marker = strstr(sources_root, SOURCES_DIR);
    if(marker == NULL)
        return false;
    *marker = '\0'; /* the cache root, with "/sources/..." cut off */

    /* The source's own place inside the cache — "<name>/<version>/<target>/…" —
       is what identifies the code, and the command hash is what identifies how
       it is being compiled. Both are needed: the same file at two optimisation
       levels is two objects. */
    const char *coordinate = source + strlen(sources_root) + strlen(SOURCES_DIR);
    return fs_format_path(out, size, "%s/" OBJECTS_DIR "/%s-%016llx.o", sources_root, coordinate,
                          (unsigned long long)hash_command(command));
}

/* A byte copy rather than a hard link: the compiler writes an object by
   opening that path, so a link would let the next build rewrite the cached
   copy in place — and a cache entry that can be rewritten is not one. */
static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if(in == NULL)
        return false;
    FILE *out = fopen(to, "wb");
    if(out == NULL) {
        (void)fclose(in);
        return false;
    }

    char buffer[65536];
    size_t read = 0;
    bool ok = true;
    while(ok && (read = fread(buffer, 1, sizeof buffer, in)) > 0)
        ok = fwrite(buffer, 1, read, out) == read;
    ok = ok && ferror(in) == 0;

    (void)fclose(in);
    return fclose(out) == 0 && ok;
}

static bool make_parent(const char *path) {
    char directory[OBJECT_CACHE_PATH_MAX];
    if(!fs_format_path(directory, sizeof directory, "%s", path))
        return false;
    char *slash = strrchr(directory, '/');
    if(slash == NULL)
        return false;
    *slash = '\0';
    return fs_make_dirs(directory);
}

bool object_cache_take(const char *cached, const char *object) {
    if(!fs_path_exists(cached))
        return false;
    return make_parent(object) && copy_file(cached, object);
}

void object_cache_put(const char *object, const char *cached) {
    if(!make_parent(cached))
        return;

    /* Written beside the entry and renamed over it, so a reader never opens an
       object that is still being written. */
    char staging[OBJECT_CACHE_PATH_MAX];
    if(!fs_format_path(staging, sizeof staging, "%s.%d", cached, (int)getpid()))
        return;
    if(!copy_file(object, staging)) {
        (void)remove(staging);
        return;
    }
    if(!fs_replace(staging, cached))
        (void)remove(staging);
}
