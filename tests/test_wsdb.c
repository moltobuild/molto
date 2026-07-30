#include "test_framework.h"
#include "tests.h"

#include <molto/services/fs_service.h>
#include <molto/util/str_list.h>
#include <molto/workspace/wsdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Create a file with content, making parent directories as needed. */
static void write_at(const char *root, const char *rel, const char *content) {
    char path[4200];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    char dir[4200];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        (void)fs_make_dirs(dir);
    }
    (void)fs_write_file(path, content);
}

static void full(const char *root, const char *rel, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", root, rel);
}

void suite_wsdb(void) {
    char root[] = "/tmp/molto_wsdb_XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    /* Inputs and an object file that will be tracked. */
    write_at(root, "src/main.c", "#include \"util.h\"\nint main(void){return 0;}\n");
    write_at(root, "src/util.h", "int answer(void);\n");
    write_at(root, "build/debug/obj/src/main.c.o", "objectbytes");

    char object[4200], main_c[4200], util_h[4200];
    full(root, "build/debug/obj/src/main.c.o", object, sizeof object);
    full(root, "src/main.c", main_c, sizeof main_c);
    full(root, "src/util.h", util_h, sizeof util_h);

    str_list prereqs;
    str_list_init(&prereqs);
    CHECK(str_list_push(&prereqs, main_c));
    CHECK(str_list_push(&prereqs, util_h));

    wsdb *db = wsdb_open(root);
    CHECK(db != NULL);

    /* Locking: a second open on the same workspace fails. */
    wsdb *second = wsdb_open(root);
    CHECK(second == NULL);

    /* Record then query: fresh under the same command. */
    wsdb_record_object(db, object, "cmd-v1", &prereqs);
    CHECK(wsdb_object_fresh(db, object, "cmd-v1"));

    /* A changed command -> stale. */
    CHECK(!wsdb_object_fresh(db, object, "cmd-v2"));

    /* Touch a prereq without changing content -> still fresh (hash confirms). */
    sleep(1);
    write_at(root, "src/util.h", "int answer(void);\n"); /* same content, new mtime */
    CHECK(wsdb_object_fresh(db, object, "cmd-v1"));

    /* Change the content of a prereq -> stale. */
    write_at(root, "src/util.h", "int answer(void); /* changed */\n");
    CHECK(!wsdb_object_fresh(db, object, "cmd-v1"));

    /* Persistence: re-record, close (saves) and reopen. */
    wsdb_record_object(db, object, "cmd-v1", &prereqs);
    CHECK(wsdb_object_fresh(db, object, "cmd-v1"));
    wsdb_close(db);

    db = wsdb_open(root);
    CHECK(db != NULL);
    CHECK(wsdb_object_fresh(db, object, "cmd-v1"));

    /* Pruning: an orphaned object (not in the live set) is removed from the DB
       and deleted from disk. */
    write_at(root, "build/debug/obj/src/orphan.c.o", "orphanbytes");
    char orphan[4200];
    full(root, "build/debug/obj/src/orphan.c.o", orphan, sizeof orphan);
    str_list orphan_prereqs;
    str_list_init(&orphan_prereqs);
    CHECK(str_list_push(&orphan_prereqs, main_c));
    wsdb_record_object(db, orphan, "cmd-orphan", &orphan_prereqs);

    str_list live;
    str_list_init(&live);
    CHECK(str_list_push(&live, object)); /* only the real object is live */
    char prefix[4200];
    full(root, "build/debug/obj/src/", prefix, sizeof prefix);
    wsdb_prune(db, &live, prefix);
    CHECK(!fs_path_exists(orphan));            /* file deleted */
    CHECK(!wsdb_object_fresh(db, orphan, "cmd-orphan")); /* entry gone */
    CHECK(wsdb_object_fresh(db, object, "cmd-v1"));       /* live one kept */

    wsdb_close(db);

    /* Fail-safe: a corrupt database opens empty. */
    char dbfile[4200];
    full(root, ".bin/wsdb", dbfile, sizeof dbfile);
    CHECK(fs_write_file(dbfile, "not a valid wsdb file"));
    db = wsdb_open(root);
    CHECK(db != NULL);
    CHECK(!wsdb_object_fresh(db, object, "cmd-v1")); /* discarded -> empty */
    wsdb_close(db);

    str_list_free(&prereqs);
    str_list_free(&orphan_prereqs);
    str_list_free(&live);
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
