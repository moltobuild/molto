#include <molto/workspace/wsdb.h>

#include <molto/services/fs_service.h>
#include <molto/util/str_list.h>
#include <molto/util/str_map.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define WSDB_MAGIC   "MOLTOWSDB"
#define WSDB_MAGIC_LEN 9
/* Version 2 stores input timestamps in nanoseconds (version 1 used seconds);
   an older database is discarded and rebuilt. */
#define WSDB_VERSION 2u
#define WSDB_ROOT_MAX 4096
#define WSDB_PATH    (WSDB_ROOT_MAX + 64) /* room for a root plus a "/.bin/..." tail */
#define WSDB_STRING_MAX (16u * 1024u * 1024u) /* reject absurd lengths on load */

typedef enum {
    wsdb_input_kind = 0,
    wsdb_object_kind = 1,
    wsdb_binary_kind = 2,
} wsdb_kind;

typedef struct {
    wsdb_kind kind;
    int64_t mtime_ns; /* inputs: last-seen modification time (nanoseconds) */
    uint64_t size;    /* inputs: last-seen size */
    uint64_t hash;    /* inputs: FNV-1a 64 of last-seen content */
    char *command;    /* artifacts: command fingerprint (heap) */
    str_list prereqs; /* objects: prerequisite keys */
} wsdb_entry;

struct wsdb {
    char root[WSDB_ROOT_MAX];
    uint32_t version;
    str_map *entries;
    int lock_fd;
    bool dirty;
};

/* --- entry lifecycle --- */

static wsdb_entry *entry_new(wsdb_kind kind) {
    wsdb_entry *e = calloc(1, sizeof *e);
    if (e != NULL) {
        e->kind = kind;
        str_list_init(&e->prereqs);
    }
    return e;
}

static void entry_free(void *value) {
    wsdb_entry *e = value;
    free(e->command);
    str_list_free(&e->prereqs);
    free(e);
}

static void entry_set_command(wsdb_entry *e, const char *command) {
    free(e->command);
    e->command = command != NULL ? strdup(command) : NULL;
}

/* --- path helpers (keys are stored relative to the workspace root) --- */

static void relativize(const char *root, const char *path, char *out, size_t out_size) {
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) == 0 && path[root_len] == '/')
        snprintf(out, out_size, "%s", path + root_len + 1);
    else
        snprintf(out, out_size, "%s", path);
}

static void make_full(const char *root, const char *rel, char *out, size_t out_size) {
    if (rel[0] == '/')
        snprintf(out, out_size, "%s", rel);
    else
        snprintf(out, out_size, "%s/%s", root, rel);
}

/* Nanoseconds in one second, for composing a timestamp. */
#define NANOS_PER_SECOND 1000000000LL

/* Modification time of an already stat-ed file, in nanoseconds. */
static int64_t stat_mtime_ns(const struct stat *info) {
    return (int64_t)info->st_mtim.tv_sec * NANOS_PER_SECOND
         + (int64_t)info->st_mtim.tv_nsec;
}

/* FNV-1a 64-bit hash of a file's content; 0 if it cannot be read. */
static uint64_t hash_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return 0;
    uint64_t hash = 1469598103934665603ULL;
    unsigned char buffer[4096];
    size_t read;
    while ((read = fread(buffer, 1, sizeof buffer, file)) > 0) {
        for (size_t i = 0; i < read; i++) {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
    }
    fclose(file);
    return hash;
}

/* --- freshness --- */

/* Record/refresh the input signature for `rel` (a key). */
static void record_input(wsdb *db, const char *rel) {
    char full[WSDB_PATH];
    make_full(db->root, rel, full, sizeof full);
    struct stat info;
    if (stat(full, &info) != 0)
        return;
    wsdb_entry *e = str_map_get(db->entries, rel);
    if (e == NULL || e->kind != wsdb_input_kind) {
        e = entry_new(wsdb_input_kind);
        if (e == NULL || !str_map_put(db->entries, rel, e)) {
            entry_free(e);
            return;
        }
    }
    e->mtime_ns = stat_mtime_ns(&info);
    e->size = (uint64_t)info.st_size;
    e->hash = hash_file(full);
}

/* Has the input `rel` changed since it was recorded? Hybrid: trust mtime+size,
   confirm with a content hash only when they differ (and refresh on a match).
   Timestamps are nanosecond-precise, so edits within the same second count. */
static bool input_unchanged(wsdb *db, const char *rel) {
    wsdb_entry *e = str_map_get(db->entries, rel);
    if (e == NULL || e->kind != wsdb_input_kind)
        return false; /* no baseline -> treat as changed */
    char full[WSDB_PATH];
    make_full(db->root, rel, full, sizeof full);
    struct stat info;
    if (stat(full, &info) != 0)
        return false; /* deleted */
    if (stat_mtime_ns(&info) == e->mtime_ns && (uint64_t)info.st_size == e->size)
        return true; /* fast path */
    if (hash_file(full) == e->hash) {
        e->mtime_ns = stat_mtime_ns(&info); /* touched, same content: refresh */
        e->size = (uint64_t)info.st_size;
        db->dirty = true;
        return true;
    }
    return false;
}

bool wsdb_object_fresh(wsdb *db, const char *object, const char *command) {
    char rel[WSDB_PATH];
    relativize(db->root, object, rel, sizeof rel);
    wsdb_entry *e = str_map_get(db->entries, rel);
    if (e == NULL || e->kind != wsdb_object_kind)
        return false;
    if (e->command == NULL || strcmp(e->command, command) != 0)
        return false;
    struct stat info;
    if (stat(object, &info) != 0)
        return false;
    for (size_t i = 0; i < str_list_count(&e->prereqs); i++) {
        if (!input_unchanged(db, str_list_get(&e->prereqs, i)))
            return false;
    }
    return true;
}

void wsdb_record_object(wsdb *db, const char *object, const char *command,
                        const str_list *prereqs) {
    char rel[WSDB_PATH];
    relativize(db->root, object, rel, sizeof rel);
    wsdb_entry *e = str_map_get(db->entries, rel);
    if (e == NULL || e->kind != wsdb_object_kind) {
        e = entry_new(wsdb_object_kind);
        if (e == NULL || !str_map_put(db->entries, rel, e)) {
            entry_free(e);
            return;
        }
    }
    entry_set_command(e, command);
    str_list_free(&e->prereqs);
    str_list_init(&e->prereqs);
    for (size_t i = 0; i < str_list_count(prereqs); i++) {
        char prel[WSDB_PATH];
        relativize(db->root, str_list_get(prereqs, i), prel, sizeof prel);
        if (str_list_push(&e->prereqs, prel))
            record_input(db, prel);
    }
    db->dirty = true;
}

bool wsdb_binary_fresh(wsdb *db, const char *binary, const char *command) {
    char rel[WSDB_PATH];
    relativize(db->root, binary, rel, sizeof rel);
    wsdb_entry *e = str_map_get(db->entries, rel);
    if (e == NULL || e->kind != wsdb_binary_kind)
        return false;
    if (e->command == NULL || strcmp(e->command, command) != 0)
        return false;
    struct stat info;
    return stat(binary, &info) == 0;
}

void wsdb_record_binary(wsdb *db, const char *binary, const char *command) {
    char rel[WSDB_PATH];
    relativize(db->root, binary, rel, sizeof rel);
    wsdb_entry *e = str_map_get(db->entries, rel);
    if (e == NULL || e->kind != wsdb_binary_kind) {
        e = entry_new(wsdb_binary_kind);
        if (e == NULL || !str_map_put(db->entries, rel, e)) {
            entry_free(e);
            return;
        }
    }
    entry_set_command(e, command);
    db->dirty = true;
}

/* --- pruning --- */

struct prune_ctx {
    const char *prefix;
    const str_list *live_rel;
    str_list *to_remove;
};

static bool list_contains(const str_list *list, const char *value) {
    for (size_t i = 0; i < str_list_count(list); i++) {
        if (strcmp(str_list_get(list, i), value) == 0)
            return true;
    }
    return false;
}

static void prune_collect(const char *key, void *value, void *vctx) {
    struct prune_ctx *ctx = vctx;
    wsdb_entry *e = value;
    if (e->kind != wsdb_object_kind)
        return;
    if (strncmp(key, ctx->prefix, strlen(ctx->prefix)) != 0)
        return;
    if (list_contains(ctx->live_rel, key))
        return;
    (void)str_list_push(ctx->to_remove, key);
}

void wsdb_prune(wsdb *db, const str_list *live, const char *prefix) {
    char prefix_rel[WSDB_PATH];
    relativize(db->root, prefix, prefix_rel, sizeof prefix_rel);

    str_list live_rel;
    str_list_init(&live_rel);
    for (size_t i = 0; i < str_list_count(live); i++) {
        char rel[WSDB_PATH];
        relativize(db->root, str_list_get(live, i), rel, sizeof rel);
        (void)str_list_push(&live_rel, rel);
    }

    str_list to_remove;
    str_list_init(&to_remove);
    struct prune_ctx ctx = { prefix_rel, &live_rel, &to_remove };
    str_map_foreach(db->entries, prune_collect, &ctx);

    for (size_t i = 0; i < str_list_count(&to_remove); i++) {
        const char *rel = str_list_get(&to_remove, i);
        char full[WSDB_PATH];
        make_full(db->root, rel, full, sizeof full);
        remove(full); /* delete the orphaned object file */
        str_map_remove(db->entries, rel);
        db->dirty = true;
    }

    str_list_free(&live_rel);
    str_list_free(&to_remove);
}

/* --- binary serialization (host byte order; the DB is local and disposable) --- */

static bool write_bytes(FILE *f, const void *data, size_t len) {
    return fwrite(data, 1, len, f) == len;
}

static bool write_u32(FILE *f, uint32_t v) { return write_bytes(f, &v, sizeof v); }
static bool write_u64(FILE *f, uint64_t v) { return write_bytes(f, &v, sizeof v); }
static bool write_i64(FILE *f, int64_t v)  { return write_bytes(f, &v, sizeof v); }

static bool write_str(FILE *f, const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    return write_u32(f, len) && write_bytes(f, s, len);
}

struct save_ctx { FILE *f; bool ok; };

static void save_entry(const char *key, void *value, void *vctx) {
    struct save_ctx *c = vctx;
    if (!c->ok)
        return;
    wsdb_entry *e = value;
    c->ok = fputc((int)e->kind, c->f) != EOF && write_str(c->f, key);
    if (!c->ok)
        return;
    if (e->kind == wsdb_input_kind) {
        c->ok = write_i64(c->f, e->mtime_ns)
             && write_u64(c->f, e->size)
             && write_u64(c->f, e->hash);
    } else {
        c->ok = write_str(c->f, e->command != NULL ? e->command : "");
        if (c->ok && e->kind == wsdb_object_kind) {
            c->ok = write_u32(c->f, (uint32_t)str_list_count(&e->prereqs));
            for (size_t i = 0; c->ok && i < str_list_count(&e->prereqs); i++)
                c->ok = write_str(c->f, str_list_get(&e->prereqs, i));
        }
    }
}

static bool wsdb_save(wsdb *db) {
    char tmp[WSDB_PATH];
    snprintf(tmp, sizeof tmp, "%s/.bin/wsdb.tmp", db->root);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL)
        return false;
    struct save_ctx ctx = { f, true };
    ctx.ok = write_bytes(f, WSDB_MAGIC, WSDB_MAGIC_LEN)
          && write_u32(f, db->version)
          && write_u32(f, (uint32_t)str_map_size(db->entries));
    if (ctx.ok)
        str_map_foreach(db->entries, save_entry, &ctx);
    bool ok = ctx.ok && fclose(f) == 0;
    if (f != NULL && !ctx.ok)
        fclose(f);
    if (!ok) {
        remove(tmp);
        return false;
    }
    char path[WSDB_PATH];
    snprintf(path, sizeof path, "%s/.bin/wsdb", db->root);
    return rename(tmp, path) == 0;
}

static bool read_bytes(FILE *f, void *out, size_t len) {
    return fread(out, 1, len, f) == len;
}

static bool read_u32(FILE *f, uint32_t *out) { return read_bytes(f, out, sizeof *out); }
static bool read_u64(FILE *f, uint64_t *out) { return read_bytes(f, out, sizeof *out); }
static bool read_i64(FILE *f, int64_t *out)  { return read_bytes(f, out, sizeof *out); }

/* Read a length-prefixed string into a freshly allocated buffer. */
static char *read_str(FILE *f) {
    uint32_t len;
    if (!read_u32(f, &len) || len > WSDB_STRING_MAX)
        return NULL;
    char *s = malloc((size_t)len + 1);
    if (s == NULL)
        return NULL;
    if (!read_bytes(f, s, len)) {
        free(s);
        return NULL;
    }
    s[len] = '\0';
    return s;
}

/* Load the DB into a fresh map; on any inconsistency the whole file is
   discarded and the DB stays empty (fail-safe). */
static void wsdb_load(wsdb *db) {
    char path[WSDB_PATH];
    snprintf(path, sizeof path, "%s/.bin/wsdb", db->root);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return;

    char magic[WSDB_MAGIC_LEN];
    uint32_t version;
    uint32_t count;
    if (!read_bytes(f, magic, WSDB_MAGIC_LEN)
        || memcmp(magic, WSDB_MAGIC, WSDB_MAGIC_LEN) != 0
        || !read_u32(f, &version) || version != WSDB_VERSION
        || !read_u32(f, &count)) {
        fclose(f);
        return;
    }

    str_map *loaded = str_map_create(entry_free);
    if (loaded == NULL) {
        fclose(f);
        return;
    }

    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        int raw_kind = fgetc(f);
        char *key = raw_kind == EOF ? NULL : read_str(f);
        if (key == NULL) {
            ok = false;
            break;
        }
        wsdb_entry *e = entry_new((wsdb_kind)raw_kind);
        if (e == NULL) {
            free(key);
            ok = false;
            break;
        }
        if (raw_kind == wsdb_input_kind) {
            ok = read_i64(f, &e->mtime_ns) && read_u64(f, &e->size)
              && read_u64(f, &e->hash);
        } else {
            char *command = read_str(f);
            if (command != NULL) {
                e->command = command;
                if (raw_kind == wsdb_object_kind) {
                    uint32_t prereq_count;
                    ok = read_u32(f, &prereq_count) && prereq_count <= WSDB_STRING_MAX;
                    for (uint32_t p = 0; ok && p < prereq_count; p++) {
                        char *prereq = read_str(f);
                        if (prereq == NULL || !str_list_push(&e->prereqs, prereq))
                            ok = false;
                        free(prereq);
                    }
                }
            } else {
                ok = false;
            }
        }
        if (ok)
            ok = str_map_put(loaded, key, e);
        if (!ok)
            entry_free(e);
        free(key);
    }
    fclose(f);

    if (ok) {
        str_map_destroy(db->entries);
        db->entries = loaded;
    } else {
        str_map_destroy(loaded); /* discard partial/corrupt load */
    }
}

/* --- lifecycle --- */

wsdb *wsdb_open(const char *root) {
    wsdb *db = calloc(1, sizeof *db);
    if (db == NULL)
        return NULL;
    snprintf(db->root, sizeof db->root, "%s", root);
    db->version = WSDB_VERSION;
    db->lock_fd = -1;
    db->entries = str_map_create(entry_free);
    if (db->entries == NULL) {
        free(db);
        return NULL;
    }

    char bindir[WSDB_PATH];
    snprintf(bindir, sizeof bindir, "%s/.bin", root);
    if (!fs_make_dirs(bindir)) {
        str_map_destroy(db->entries);
        free(db);
        return NULL;
    }

    char lockpath[WSDB_PATH];
    snprintf(lockpath, sizeof lockpath, "%s/.bin/lock", root);
    db->lock_fd = open(lockpath, O_CREAT | O_RDWR, 0644);
    if (db->lock_fd < 0 || flock(db->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (db->lock_fd >= 0)
            close(db->lock_fd);
        str_map_destroy(db->entries);
        free(db);
        return NULL;
    }

    wsdb_load(db);
    db->dirty = false;
    return db;
}

void wsdb_close(wsdb *db) {
    if (db == NULL)
        return;
    if (db->dirty)
        (void)wsdb_save(db);
    if (db->lock_fd >= 0)
        close(db->lock_fd); /* releases the flock */
    str_map_destroy(db->entries);
    free(db);
}
