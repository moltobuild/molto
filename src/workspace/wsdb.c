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

#define WSDB_MAGIC "MOLTOWSDB"
#define WSDB_MAGIC_LEN 9
/* Version 5 records, beside every prerequisite, the content hash that
   prerequisite had when the artifact was built; version 4 added the
   analysis-result kind (RFC-0006), version 3 resolved toolchains. An older
   database is discarded and rebuilt. */
#define WSDB_VERSION 5u
#define WSDB_ROOT_MAX 4096
#define WSDB_PATH (WSDB_ROOT_MAX + 64)        /* room for a root plus a "/.bin/..." tail */
#define WSDB_STRING_MAX (16u * 1024u * 1024u) /* reject absurd lengths on load */

/* The directory Molto owns inside a workspace, and the files it keeps there. */
#define DIR_BIN ".bin"
#define FILE_DATABASE "wsdb"
#define FILE_STAGING "wsdb.tmp" /* written, then renamed over FILE_DATABASE */
#define FILE_LOCK "lock"

typedef enum {
    wsdb_input_kind = 0,
    wsdb_object_kind = 1,
    wsdb_binary_kind = 2,
    /* A toolchain resolved for this workspace. Its command is the request that
       produced it, and its prereqs hold the answer: keeping it here is what
       spares an external query on every build. */
    wsdb_toolchain_kind = 3,
    /* What a tool said about one file (RFC-0006). Its command is the analysis
       fingerprint, its prereqs are the file and the headers it included, and
       its values are the diagnostics to replay. */
    wsdb_result_kind = 4,
} wsdb_kind;

/* Kinds whose entry carries a list alongside its command: prerequisites for an
   object and a result, the resolved answer for a toolchain. */
static bool kind_has_list(int kind) {
    return kind == wsdb_object_kind || kind == wsdb_toolchain_kind || kind == wsdb_result_kind;
}

/* Kinds carrying a second list: what was recorded, as opposed to what it
   depended on. Only a result has both. */
static bool kind_has_values(int kind) { return kind == wsdb_result_kind; }

typedef struct {
    wsdb_kind kind;
    int64_t mtime_ns; /* inputs: last-seen modification time (nanoseconds) */
    uint64_t size;    /* inputs: last-seen size */
    uint64_t hash;    /* inputs: FNV-1a 64 of last-seen content */
    char *command;    /* artifacts: command fingerprint (heap) */
    str_list prereqs; /* objects and results: prerequisite keys */
    /* The content hash each prerequisite had when this artifact was built,
       parallel to `prereqs`.

       It lives here, and not only in the input entry, because an input entry
       is shared by every artifact that depends on it. Judging staleness
       against a shared "last seen" is wrong the moment two artifacts are built
       in separate passes: the first pass refreshes the shared baseline, and
       the second concludes that a header it has not seen since is unchanged.
       That produced a stale object linked against fresh ones — the same struct
       with two layouts — which is a crash with no message. */
    uint64_t *prereq_hashes;
    size_t prereq_hash_count;
    str_list values; /* results: the recorded output */
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
    if(e != NULL) {
        e->kind = kind;
        str_list_init(&e->prereqs);
        str_list_init(&e->values);
    }
    return e;
}

static void entry_free(void *value) {
    wsdb_entry *e = value;
    free(e->command);
    free(e->prereq_hashes);
    str_list_free(&e->prereqs);
    str_list_free(&e->values);
    free(e);
}

static void entry_set_command(wsdb_entry *e, const char *command) {
    free(e->command);
    e->command = command != NULL ? strdup(command) : NULL;
}

/* --- path helpers (keys are stored relative to the workspace root) --- */

/* False if the path did not fit: a truncated key would alias another entry. */
[[nodiscard]] static bool relativize(const char *root, const char *path, char *out,
                                     size_t out_size) {
    size_t root_len = strlen(root);
    if(strncmp(path, root, root_len) == 0 && path[root_len] == '/')
        return fs_format_path(out, out_size, "%s", path + root_len + 1);
    return fs_format_path(out, out_size, "%s", path);
}

[[nodiscard]] static bool make_full(const char *root, const char *rel, char *out, size_t out_size) {
    if(rel[0] == '/')
        return fs_format_path(out, out_size, "%s", rel);
    return fs_format_path(out, out_size, "%s/%s", root, rel);
}

/* Path of one of the files Molto owns under `<root>/.bin/`. */
[[nodiscard]] static bool bin_path(const char *root, const char *name, char *out, size_t out_size) {
    return fs_format_path(out, out_size, "%s/" DIR_BIN "/%s", root, name);
}

/* Nanoseconds in one second, for composing a timestamp. */
#define NANOS_PER_SECOND 1000000000LL

/* Modification time of an already stat-ed file, in nanoseconds. */
static int64_t stat_mtime_ns(const struct stat *info) {
    return (int64_t)info->st_mtim.tv_sec * NANOS_PER_SECOND + (int64_t)info->st_mtim.tv_nsec;
}

/* FNV-1a 64-bit hash of a file's content, and false when it cannot be read.
 *
 * A read that fails halfway leaves the loop the same way the end of the file
 * does, so the error has to be asked for: hashing the bytes that did arrive
 * would answer for a file nobody managed to read, and answer it consistently
 * — which is a stale artifact called fresh for as long as the failure lasts. */
[[nodiscard]] static bool hash_file(const char *path, uint64_t *out) {
    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return false;
    uint64_t hash = 1469598103934665603ULL;
    unsigned char buffer[4096];
    size_t read;
    while((read = fread(buffer, 1, sizeof buffer, file)) > 0) {
        for(size_t i = 0; i < read; i++) {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
    }
    const bool ok = ferror(file) == 0;
    (void)fclose(file);
    if(ok)
        *out = hash;
    return ok;
}

/* --- freshness --- */

/* The content hash `rel` has right now, and false when it cannot be read.

   The input entry is a memo, not a baseline: it caches (mtime, size) -> hash so
   an unchanged file is not read again on every build. Whether an artifact is
   stale is decided against the hash stored in that artifact's own entry. */
[[nodiscard]] static bool current_hash(wsdb *db, const char *rel, uint64_t *out) {
    char full[WSDB_PATH];
    if(!make_full(db->root, rel, full, sizeof full))
        return false;
    struct stat info;
    if(stat(full, &info) != 0)
        return false; /* deleted */

    wsdb_entry *e = str_map_get(db->entries, rel);
    if(e != NULL && e->kind == wsdb_input_kind && stat_mtime_ns(&info) == e->mtime_ns &&
       (uint64_t)info.st_size == e->size) {
        *out = e->hash;
        return true;
    }

    uint64_t hash = 0;
    if(!hash_file(full, &hash))
        return false;
    if(e == NULL || e->kind != wsdb_input_kind) {
        e = entry_new(wsdb_input_kind);
        if(e == NULL || !str_map_put(db->entries, rel, e)) {
            entry_free(e);
            *out = hash; /* the memo is an optimisation; the answer stands */
            return true;
        }
    }
    e->mtime_ns = stat_mtime_ns(&info);
    e->size = (uint64_t)info.st_size;
    e->hash = hash;
    db->dirty = true;
    *out = hash;
    return true;
}

/* Replace an artifact's prerequisites, recording what each one hashes to now.
   A prerequisite that cannot be read leaves the artifact without a complete
   baseline, which costs a rebuild rather than a wrong answer. */
[[nodiscard]] static bool set_prereqs(wsdb *db, wsdb_entry *e, const str_list *prereqs) {
    str_list_free(&e->prereqs);
    str_list_init(&e->prereqs);
    free(e->prereq_hashes);
    e->prereq_hashes = NULL;
    e->prereq_hash_count = 0;

    const size_t count = str_list_count(prereqs);
    if(count > 0) {
        e->prereq_hashes = calloc(count, sizeof *e->prereq_hashes);
        if(e->prereq_hashes == NULL)
            return false;
    }

    bool ok = true;
    for(size_t i = 0; i < count; i++) {
        char rel[WSDB_PATH];
        uint64_t hash = 0;
        if(!relativize(db->root, str_list_get(prereqs, i), rel, sizeof rel) ||
           !str_list_push(&e->prereqs, rel) || !current_hash(db, rel, &hash)) {
            ok = false;
            continue;
        }
        e->prereq_hashes[e->prereq_hash_count++] = hash;
    }
    return ok;
}

/* True when every prerequisite still hashes to what it did when this artifact
   was built. A count that does not line up means the entry was written by a
   failed record: rebuild rather than guess which hash belongs to which file. */
static bool prereqs_unchanged_upto(wsdb *db, const wsdb_entry *e, size_t count) {
    if(count > e->prereq_hash_count || count > str_list_count(&e->prereqs))
        return false;
    for(size_t i = 0; i < count; i++) {
        uint64_t hash = 0;
        if(!current_hash(db, str_list_get(&e->prereqs, i), &hash) || hash != e->prereq_hashes[i])
            return false;
    }
    return true;
}

static bool prereqs_unchanged(wsdb *db, const wsdb_entry *e) {
    if(e->prereq_hash_count != str_list_count(&e->prereqs))
        return false;
    return prereqs_unchanged_upto(db, e, e->prereq_hash_count);
}

bool wsdb_object_fresh(wsdb *db, const char *object, const char *command) {
    char rel[WSDB_PATH];
    if(!relativize(db->root, object, rel, sizeof rel))
        return false;
    wsdb_entry *e = str_map_get(db->entries, rel);
    if(e == NULL || e->kind != wsdb_object_kind)
        return false;
    if(e->command == NULL || strcmp(e->command, command) != 0)
        return false;
    struct stat info;
    if(stat(object, &info) != 0)
        return false;
    return prereqs_unchanged(db, e);
}

bool wsdb_record_object(wsdb *db, const char *object, const char *command,
                        const str_list *prereqs) {
    char rel[WSDB_PATH];
    if(!relativize(db->root, object, rel, sizeof rel))
        return false;
    wsdb_entry *e = str_map_get(db->entries, rel);
    if(e == NULL || e->kind != wsdb_object_kind) {
        e = entry_new(wsdb_object_kind);
        if(e == NULL || !str_map_put(db->entries, rel, e)) {
            entry_free(e);
            return false;
        }
    }
    entry_set_command(e, command);
    db->dirty = true;
    return set_prereqs(db, e, prereqs);
}

bool wsdb_binary_fresh(wsdb *db, const char *binary, const char *command) {
    char rel[WSDB_PATH];
    if(!relativize(db->root, binary, rel, sizeof rel))
        return false;
    wsdb_entry *e = str_map_get(db->entries, rel);
    if(e == NULL || e->kind != wsdb_binary_kind)
        return false;
    if(e->command == NULL || strcmp(e->command, command) != 0)
        return false;
    struct stat info;
    return stat(binary, &info) == 0;
}

bool wsdb_record_binary(wsdb *db, const char *binary, const char *command) {
    char rel[WSDB_PATH];
    if(!relativize(db->root, binary, rel, sizeof rel))
        return false;
    wsdb_entry *e = str_map_get(db->entries, rel);
    if(e == NULL || e->kind != wsdb_binary_kind) {
        e = entry_new(wsdb_binary_kind);
        if(e == NULL || !str_map_put(db->entries, rel, e)) {
            entry_free(e);
            return false;
        }
    }
    entry_set_command(e, command);
    db->dirty = true;
    return true;
}

/* --- resolved toolchains --- */

bool wsdb_toolchain_fresh(wsdb *db, const char *key, const char *request) {
    wsdb_entry *e = str_map_get(db->entries, key);
    if(e == NULL || e->kind != wsdb_toolchain_kind)
        return false;
    if(e->command == NULL || strcmp(e->command, request) != 0)
        return false;
    if(str_list_count(&e->prereqs) == 0)
        return false;
    /* Only the first entry is a file; the rest are plain facts about it. So
       this watches exactly the compiler binary — replaced binary, stale
       answer — with the same content check that guards a source file. */
    return prereqs_unchanged_upto(db, e, 1);
}

bool wsdb_record_toolchain(wsdb *db, const char *key, const char *request, const str_list *values) {
    if(str_list_count(values) == 0)
        return false;

    wsdb_entry *e = str_map_get(db->entries, key);
    if(e == NULL || e->kind != wsdb_toolchain_kind) {
        e = entry_new(wsdb_toolchain_kind);
        if(e == NULL || !str_map_put(db->entries, key, e)) {
            entry_free(e);
            return false;
        }
    }
    entry_set_command(e, request);
    str_list_free(&e->prereqs);
    str_list_init(&e->prereqs);
    free(e->prereq_hashes);
    e->prereq_hashes = NULL;
    e->prereq_hash_count = 0;
    db->dirty = true;

    bool ok = true;
    for(size_t i = 0; i < str_list_count(values); i++)
        ok = str_list_push(&e->prereqs, str_list_get(values, i)) && ok;

    /* Only the compiler is an input; the rest are plain facts about it, so one
       hash is recorded and one is checked. */
    e->prereq_hashes = calloc(1, sizeof *e->prereq_hashes);
    if(e->prereq_hashes == NULL)
        return false;
    if(!current_hash(db, str_list_get(&e->prereqs, 0), &e->prereq_hashes[0]))
        return false;
    e->prereq_hash_count = 1;
    return ok;
}

bool wsdb_toolchain_values(wsdb *db, const char *key, str_list *out) {
    wsdb_entry *e = str_map_get(db->entries, key);
    if(e == NULL || e->kind != wsdb_toolchain_kind)
        return false;
    for(size_t i = 0; i < str_list_count(&e->prereqs); i++) {
        if(!str_list_push(out, str_list_get(&e->prereqs, i)))
            return false;
    }
    return true;
}

/* --- analysis results (RFC-0006) --- */

bool wsdb_result_fresh(wsdb *db, const char *key, const char *fingerprint) {
    wsdb_entry *e = str_map_get(db->entries, key);
    if(e == NULL || e->kind != wsdb_result_kind)
        return false;
    if(e->command == NULL || strcmp(e->command, fingerprint) != 0)
        return false;
    /* An entry with nothing to watch would answer "fresh" forever. Recording
       one is a bug in the caller, and reading one has to be survivable. */
    if(str_list_count(&e->prereqs) == 0)
        return false;
    return prereqs_unchanged(db, e);
}

bool wsdb_record_result(wsdb *db, const char *key, const char *fingerprint, const str_list *prereqs,
                        const str_list *values) {
    if(str_list_count(prereqs) == 0)
        return false;

    wsdb_entry *e = str_map_get(db->entries, key);
    if(e == NULL || e->kind != wsdb_result_kind) {
        e = entry_new(wsdb_result_kind);
        if(e == NULL || !str_map_put(db->entries, key, e)) {
            entry_free(e);
            return false;
        }
    }
    entry_set_command(e, fingerprint);
    str_list_free(&e->values);
    str_list_init(&e->values);
    db->dirty = true;

    bool ok = set_prereqs(db, e, prereqs);
    for(size_t i = 0; i < str_list_count(values); i++)
        ok = str_list_push(&e->values, str_list_get(values, i)) && ok;
    return ok;
}

bool wsdb_result_values(wsdb *db, const char *key, str_list *out) {
    wsdb_entry *e = str_map_get(db->entries, key);
    if(e == NULL || e->kind != wsdb_result_kind)
        return false;
    /* An empty list is an answer: the file was analysed and had nothing to
       say. It is not the same as having no entry, which is why this returns
       true without pushing anything. */
    for(size_t i = 0; i < str_list_count(&e->values); i++) {
        if(!str_list_push(out, str_list_get(&e->values, i)))
            return false;
    }
    return true;
}

/* --- pruning --- */

struct prune_ctx {
    const char *prefix;
    const str_list *live_rel;
    str_list *to_remove;
};

static bool list_contains(const str_list *list, const char *value) {
    for(size_t i = 0; i < str_list_count(list); i++) {
        if(strcmp(str_list_get(list, i), value) == 0)
            return true;
    }
    return false;
}

/* Outputs Molto produces and therefore may delete. Inputs are the user's files
   and are never touched, however stale their entry looks. */
static bool kind_is_output(wsdb_kind kind) {
    return kind == wsdb_object_kind || kind == wsdb_binary_kind;
}

static void prune_collect(const char *key, void *value, void *vctx) {
    struct prune_ctx *ctx = vctx;
    wsdb_entry *e = value;
    if(!kind_is_output(e->kind))
        return;
    if(strncmp(key, ctx->prefix, strlen(ctx->prefix)) != 0)
        return;
    if(list_contains(ctx->live_rel, key))
        return;
    (void)str_list_push(ctx->to_remove, key);
}

void wsdb_prune(wsdb *db, const str_list *live, const char *prefix) {
    char prefix_rel[WSDB_PATH];
    if(!relativize(db->root, prefix, prefix_rel, sizeof prefix_rel))
        return;

    /* An incomplete live set would make a live artifact look orphaned, so a
       failure here cancels the prune: keeping a stale file beats deleting a
       good one. */
    str_list live_rel;
    str_list_init(&live_rel);
    bool ok = true;
    for(size_t i = 0; ok && i < str_list_count(live); i++) {
        char rel[WSDB_PATH];
        ok = relativize(db->root, str_list_get(live, i), rel, sizeof rel) &&
             str_list_push(&live_rel, rel);
    }
    if(!ok) {
        str_list_free(&live_rel);
        return;
    }

    str_list to_remove;
    str_list_init(&to_remove);
    struct prune_ctx ctx = {prefix_rel, &live_rel, &to_remove};
    str_map_foreach(db->entries, prune_collect, &ctx);

    for(size_t i = 0; i < str_list_count(&to_remove); i++) {
        const char *rel = str_list_get(&to_remove, i);
        char full[WSDB_PATH];
        if(!make_full(db->root, rel, full, sizeof full))
            continue;
        remove(full); /* delete the orphaned artifact */
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
static bool write_i64(FILE *f, int64_t v) { return write_bytes(f, &v, sizeof v); }

static bool write_str(FILE *f, const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    return write_u32(f, len) && write_bytes(f, s, len);
}

struct save_ctx {
    FILE *f;
    bool ok;
};

static void save_entry(const char *key, void *value, void *vctx) {
    struct save_ctx *c = vctx;
    if(!c->ok)
        return;
    wsdb_entry *e = value;
    c->ok = fputc((int)e->kind, c->f) != EOF && write_str(c->f, key);
    if(!c->ok)
        return;
    if(e->kind == wsdb_input_kind) {
        c->ok =
            write_i64(c->f, e->mtime_ns) && write_u64(c->f, e->size) && write_u64(c->f, e->hash);
    } else {
        c->ok = write_str(c->f, e->command != NULL ? e->command : "");
        if(c->ok && kind_has_list(e->kind)) {
            c->ok = write_u32(c->f, (uint32_t)str_list_count(&e->prereqs)) &&
                    write_u32(c->f, (uint32_t)e->prereq_hash_count);
            for(size_t i = 0; c->ok && i < str_list_count(&e->prereqs); i++)
                c->ok = write_str(c->f, str_list_get(&e->prereqs, i));
            /* Written after the names rather than interleaved, because a
               toolchain records fewer hashes than it has entries. */
            for(size_t i = 0; c->ok && i < e->prereq_hash_count; i++)
                c->ok = write_u64(c->f, e->prereq_hashes[i]);
        }
        if(c->ok && kind_has_values(e->kind)) {
            c->ok = write_u32(c->f, (uint32_t)str_list_count(&e->values));
            for(size_t i = 0; c->ok && i < str_list_count(&e->values); i++)
                c->ok = write_str(c->f, str_list_get(&e->values, i));
        }
    }
}

static bool wsdb_save(wsdb *db) {
    char tmp[WSDB_PATH];
    if(!bin_path(db->root, FILE_STAGING, tmp, sizeof tmp))
        return false;
    FILE *f = fopen(tmp, "wb");
    if(f == NULL)
        return false;
    struct save_ctx ctx = {f, true};
    ctx.ok = write_bytes(f, WSDB_MAGIC, WSDB_MAGIC_LEN) && write_u32(f, db->version) &&
             write_u32(f, (uint32_t)str_map_size(db->entries));
    if(ctx.ok)
        str_map_foreach(db->entries, save_entry, &ctx);
    bool ok = ctx.ok && fclose(f) == 0;
    if(f != NULL && !ctx.ok)
        fclose(f);
    if(!ok) {
        remove(tmp);
        return false;
    }
    char path[WSDB_PATH];
    if(!bin_path(db->root, FILE_DATABASE, path, sizeof path)) {
        remove(tmp);
        return false;
    }
    return rename(tmp, path) == 0;
}

static bool read_bytes(FILE *f, void *out, size_t len) { return fread(out, 1, len, f) == len; }

static bool read_u32(FILE *f, uint32_t *out) { return read_bytes(f, out, sizeof *out); }
static bool read_u64(FILE *f, uint64_t *out) { return read_bytes(f, out, sizeof *out); }
static bool read_i64(FILE *f, int64_t *out) { return read_bytes(f, out, sizeof *out); }

/* Read a length-prefixed string into a freshly allocated buffer. */
static char *read_str(FILE *f) {
    uint32_t len;
    if(!read_u32(f, &len) || len > WSDB_STRING_MAX)
        return NULL;
    char *s = malloc((size_t)len + 1);
    if(s == NULL)
        return NULL;
    if(!read_bytes(f, s, len)) {
        free(s);
        return NULL;
    }
    s[len] = '\0';
    return s;
}

/* True if a byte read from the file names a kind we know. Anything else is
   corruption, not a future format: the version header covers real upgrades. */
static bool kind_is_known(int raw_kind) {
    return raw_kind == wsdb_input_kind || raw_kind == wsdb_object_kind ||
           raw_kind == wsdb_binary_kind || raw_kind == wsdb_toolchain_kind ||
           raw_kind == wsdb_result_kind;
}

/* Load the DB into a fresh map; on any inconsistency the whole file is
   discarded and the DB stays empty (fail-safe). */
static void wsdb_load(wsdb *db) {
    char path[WSDB_PATH];
    if(!bin_path(db->root, FILE_DATABASE, path, sizeof path))
        return;
    FILE *f = fopen(path, "rb");
    if(f == NULL)
        return;

    char magic[WSDB_MAGIC_LEN];
    uint32_t version;
    uint32_t count;
    if(!read_bytes(f, magic, WSDB_MAGIC_LEN) || memcmp(magic, WSDB_MAGIC, WSDB_MAGIC_LEN) != 0 ||
       !read_u32(f, &version) || version != WSDB_VERSION || !read_u32(f, &count)) {
        fclose(f);
        return;
    }

    str_map *loaded = str_map_create(entry_free);
    if(loaded == NULL) {
        fclose(f);
        return;
    }

    bool ok = true;
    for(uint32_t i = 0; ok && i < count; i++) {
        int raw_kind = fgetc(f);
        char *key = kind_is_known(raw_kind) ? read_str(f) : NULL;
        if(key == NULL) {
            ok = false;
            break;
        }
        wsdb_entry *e = entry_new((wsdb_kind)raw_kind);
        if(e == NULL) {
            free(key);
            ok = false;
            break;
        }
        if(raw_kind == wsdb_input_kind) {
            ok = read_i64(f, &e->mtime_ns) && read_u64(f, &e->size) && read_u64(f, &e->hash);
        } else {
            char *command = read_str(f);
            if(command != NULL) {
                e->command = command;
                if(kind_has_list(raw_kind)) {
                    uint32_t prereq_count;
                    uint32_t hash_count;
                    ok = read_u32(f, &prereq_count) && prereq_count <= WSDB_STRING_MAX &&
                         read_u32(f, &hash_count) && hash_count <= prereq_count;
                    for(uint32_t p = 0; ok && p < prereq_count; p++) {
                        char *prereq = read_str(f);
                        if(prereq == NULL || !str_list_push(&e->prereqs, prereq))
                            ok = false;
                        free(prereq);
                    }
                    if(ok && hash_count > 0) {
                        e->prereq_hashes = calloc(hash_count, sizeof *e->prereq_hashes);
                        ok = e->prereq_hashes != NULL;
                        for(uint32_t h = 0; ok && h < hash_count; h++)
                            ok = read_u64(f, &e->prereq_hashes[h]);
                        if(ok)
                            e->prereq_hash_count = hash_count;
                    }
                }
                if(ok && kind_has_values(raw_kind)) {
                    uint32_t value_count;
                    ok = read_u32(f, &value_count) && value_count <= WSDB_STRING_MAX;
                    for(uint32_t v = 0; ok && v < value_count; v++) {
                        char *value = read_str(f);
                        if(value == NULL || !str_list_push(&e->values, value))
                            ok = false;
                        free(value);
                    }
                }
            } else {
                ok = false;
            }
        }
        if(ok)
            ok = str_map_put(loaded, key, e);
        if(!ok)
            entry_free(e);
        free(key);
    }
    fclose(f);

    if(ok) {
        str_map_destroy(db->entries);
        db->entries = loaded;
    } else {
        str_map_destroy(loaded); /* discard partial/corrupt load */
    }
}

/* --- lifecycle --- */

wsdb *wsdb_open(const char *root) {
    wsdb *db = calloc(1, sizeof *db);
    if(db == NULL)
        return NULL;
    snprintf(db->root, sizeof db->root, "%s", root);
    db->version = WSDB_VERSION;
    db->lock_fd = -1;
    db->entries = str_map_create(entry_free);
    if(db->entries == NULL) {
        free(db);
        return NULL;
    }

    char bindir[WSDB_PATH];
    if(!fs_format_path(bindir, sizeof bindir, "%s/" DIR_BIN, root) || !fs_make_dirs(bindir)) {
        str_map_destroy(db->entries);
        free(db);
        return NULL;
    }

    char lockpath[WSDB_PATH];
    if(!bin_path(root, FILE_LOCK, lockpath, sizeof lockpath)) {
        str_map_destroy(db->entries);
        free(db);
        return NULL;
    }
    db->lock_fd = open(lockpath, O_CREAT | O_RDWR, 0644);
    if(db->lock_fd < 0 || flock(db->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if(db->lock_fd >= 0)
            close(db->lock_fd);
        str_map_destroy(db->entries);
        free(db);
        return NULL;
    }

    wsdb_load(db);
    db->dirty = false;
    return db;
}

bool wsdb_close(wsdb *db) {
    if(db == NULL)
        return true;
    bool saved = !db->dirty || wsdb_save(db);
    if(db->lock_fd >= 0)
        close(db->lock_fd); /* releases the flock */
    str_map_destroy(db->entries);
    free(db);
    return saved;
}
