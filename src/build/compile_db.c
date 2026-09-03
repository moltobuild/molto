#include <molto/build/compile_db.h>

#include <molto/services/fs_service.h>
#include <molto/util/json_write.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the database goes, and where it is composed before it gets there. The
   temporary lives beside its destination so the rename cannot cross a
   filesystem, which is the one thing that would make it non-atomic. */
#define COMPILE_DB_FILENAME "compile_commands.json"
#define COMPILE_DB_TEMP_FILENAME ".compile_commands.json.tmp"

/* Size of the buffers holding the two paths above. */
#define COMPILE_DB_PATH_SIZE 4096

/* How many entries the array grows by when it fills. */
#define COMPILE_DB_GROWTH 64

/* One translation unit as the database describes it. */
typedef struct {
    char *file;         /* the source, as it was compiled */
    char *output;       /* the object it produced */
    str_list arguments; /* the whole command line, driver first */
} compile_entry;

struct compile_db {
    compile_entry *entries;
    size_t count;
    size_t capacity;
};

compile_db *compile_db_create(void) { return calloc(1, sizeof(compile_db)); }

static void entry_free(compile_entry *entry) {
    free(entry->file);
    free(entry->output);
    str_list_free(&entry->arguments);
}

void compile_db_destroy(compile_db *db) {
    if(db == NULL)
        return;
    for(size_t i = 0; i < db->count; i++)
        entry_free(&db->entries[i]);
    free(db->entries);
    free(db);
}

size_t compile_db_count(const compile_db *db) { return db == NULL ? 0 : db->count; }

/* Make room for one more entry. */
[[nodiscard]] static bool reserve_one(compile_db *db) {
    if(db->count < db->capacity)
        return true;
    size_t capacity = db->capacity + COMPILE_DB_GROWTH;
    compile_entry *grown = realloc(db->entries, capacity * sizeof *grown);
    if(grown == NULL)
        return false;
    db->entries = grown;
    db->capacity = capacity;
    return true;
}

[[nodiscard]] static bool copy_arguments(str_list *out, const str_list *arguments) {
    str_list_init(out);
    for(size_t i = 0; i < str_list_count(arguments); i++) {
        if(!str_list_push(out, str_list_get(arguments, i))) {
            str_list_free(out);
            return false;
        }
    }
    return true;
}

bool compile_db_add(compile_db *db, const char *file, const char *output,
                    const str_list *arguments) {
    if(db == NULL)
        return true;
    if(!reserve_one(db))
        return false;

    compile_entry entry = {0};
    entry.file = strdup(file);
    entry.output = strdup(output);
    if(entry.file == NULL || entry.output == NULL || !copy_arguments(&entry.arguments, arguments)) {
        free(entry.file);
        free(entry.output);
        return false;
    }
    db->entries[db->count++] = entry;
    return true;
}

/* The directory every relative path in the database is resolved against.
   Absolute, because a tool reads this file from wherever it happens to be
   running; `root` itself is only absolute when the caller made it so. */
static void resolve_directory(const char *root, char *out, size_t out_size) {
    char resolved[COMPILE_DB_PATH_SIZE];
    const char *directory = fs_real_path(root, resolved, sizeof resolved) ? resolved : root;
    snprintf(out, out_size, "%s", directory);
}

static void write_entry(FILE *out, const compile_entry *entry, const char *directory) {
    fputs("  {\n    \"directory\": ", out);
    json_write_string(out, directory);
    fputs(",\n    \"file\": ", out);
    json_write_string(out, fs_relative_to(entry->file, directory));
    fputs(",\n    \"output\": ", out);
    json_write_string(out, fs_relative_to(entry->output, directory));
    fputs(",\n    \"arguments\": [", out);
    for(size_t i = 0; i < str_list_count(&entry->arguments); i++) {
        fputs(i > 0 ? ", " : "", out);
        json_write_string(out, str_list_get(&entry->arguments, i));
    }
    fputs("]\n  }", out);
}

[[nodiscard]] static bool write_all(FILE *out, const compile_db *db, const char *directory) {
    fputs("[\n", out);
    for(size_t i = 0; i < db->count; i++) {
        fputs(i > 0 ? ",\n" : "", out);
        write_entry(out, &db->entries[i], directory);
    }
    fputs("\n]\n", out);
    return ferror(out) == 0;
}

bool compile_db_write(const compile_db *db, const char *root) {
    if(db == NULL)
        return true;

    char path[COMPILE_DB_PATH_SIZE];
    char temp[COMPILE_DB_PATH_SIZE];
    if(!fs_format_path(path, sizeof path, "%s/" COMPILE_DB_FILENAME, root) ||
       !fs_format_path(temp, sizeof temp, "%s/" COMPILE_DB_TEMP_FILENAME, root))
        return fs_report_long_path(root);

    char directory[COMPILE_DB_PATH_SIZE];
    resolve_directory(root, directory, sizeof directory);

    FILE *out = fopen(temp, "wb");
    if(out == NULL)
        return false;
    const bool written = write_all(out, db, directory);
    if(fclose(out) != 0 || !written) {
        remove(temp);
        return false;
    }
    if(!fs_replace(temp, path)) {
        remove(temp);
        return false;
    }
    return true;
}
