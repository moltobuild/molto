#include <moltest.h>

#include <molto/services/fs_service.h>
#include <molto/util/str_list.h>
#include <molto/workspace/wsdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A throwaway workspace with one object and its two prerequisites. */
typedef struct {
    char root[64];
    char object[256];
    char main_c[256];
    char util_h[256];
    str_list prereqs;
} workspace_fixture;

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

static bool fixture_setup(workspace_fixture *fixture) {
    snprintf(fixture->root, sizeof fixture->root, "%s", "/tmp/molto_wsdb_XXXXXX");
    if (mkdtemp(fixture->root) == NULL)
        return false;

    write_at(fixture->root, "src/main.c", "#include \"util.h\"\nint main(void){return 0;}\n");
    write_at(fixture->root, "src/util.h", "int answer(void);\n");
    write_at(fixture->root, "build/debug/obj/src/main.c.o", "objectbytes");

    snprintf(fixture->object, sizeof fixture->object, "%s/build/debug/obj/src/main.c.o",
             fixture->root);
    snprintf(fixture->main_c, sizeof fixture->main_c, "%s/src/main.c", fixture->root);
    snprintf(fixture->util_h, sizeof fixture->util_h, "%s/src/util.h", fixture->root);

    str_list_init(&fixture->prereqs);
    return str_list_push(&fixture->prereqs, fixture->main_c)
        && str_list_push(&fixture->prereqs, fixture->util_h);
}

static void fixture_teardown(workspace_fixture *fixture) {
    str_list_free(&fixture->prereqs);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", fixture->root);
    (void)system(cmd);
}

MOLTEST(wsdb_records_and_queries_an_object) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);

    wsdb_record_object(db, fixture.object, "cmd-v1", &fixture.prereqs);
    EXPECT_TRUE(wsdb_object_fresh(db, fixture.object, "cmd-v1"));
    /* A different command means the object must be rebuilt. */
    EXPECT_FALSE(wsdb_object_fresh(db, fixture.object, "cmd-v2"));

    wsdb_close(db);
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_allows_a_single_writer) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    /* A second opener finds the workspace locked. */
    EXPECT_NULL(wsdb_open(fixture.root));

    wsdb_close(db);
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_ignores_a_touch_that_does_not_change_content) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    wsdb_record_object(db, fixture.object, "cmd-v1", &fixture.prereqs);

    /* Rewriting a prerequisite with identical content only moves its mtime;
       the content hash confirms nothing changed. */
    sleep(1);
    write_at(fixture.root, "src/util.h", "int answer(void);\n");
    EXPECT_TRUE(wsdb_object_fresh(db, fixture.object, "cmd-v1"));

    wsdb_close(db);
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_detects_a_changed_prerequisite) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    wsdb_record_object(db, fixture.object, "cmd-v1", &fixture.prereqs);

    write_at(fixture.root, "src/util.h", "int answer(void); /* changed */\n");
    EXPECT_FALSE(wsdb_object_fresh(db, fixture.object, "cmd-v1"));

    wsdb_close(db);
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_persists_across_sessions) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    wsdb_record_object(db, fixture.object, "cmd-v1", &fixture.prereqs);
    wsdb_close(db); /* saves */

    db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    EXPECT_TRUE(wsdb_object_fresh(db, fixture.object, "cmd-v1"));

    wsdb_close(db);
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_prunes_orphaned_objects) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    wsdb_record_object(db, fixture.object, "cmd-v1", &fixture.prereqs);

    /* An object whose source disappeared is no longer in the live set. */
    write_at(fixture.root, "build/debug/obj/src/orphan.c.o", "orphanbytes");
    char orphan[256];
    snprintf(orphan, sizeof orphan, "%s/build/debug/obj/src/orphan.c.o", fixture.root);
    str_list orphan_prereqs;
    str_list_init(&orphan_prereqs);
    EXPECT_TRUE(str_list_push(&orphan_prereqs, fixture.main_c));
    wsdb_record_object(db, orphan, "cmd-orphan", &orphan_prereqs);

    str_list live;
    str_list_init(&live);
    EXPECT_TRUE(str_list_push(&live, fixture.object));
    char prefix[256];
    snprintf(prefix, sizeof prefix, "%s/build/debug/obj/src/", fixture.root);
    wsdb_prune(db, &live, prefix);

    EXPECT_FALSE(fs_path_exists(orphan));                       /* file deleted */
    EXPECT_FALSE(wsdb_object_fresh(db, orphan, "cmd-orphan"));  /* entry dropped */
    EXPECT_TRUE(wsdb_object_fresh(db, fixture.object, "cmd-v1")); /* live one kept */

    str_list_free(&orphan_prereqs);
    str_list_free(&live);
    wsdb_close(db);
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_recovers_from_a_corrupt_database) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    wsdb_record_object(db, fixture.object, "cmd-v1", &fixture.prereqs);
    wsdb_close(db);

    char dbfile[256];
    snprintf(dbfile, sizeof dbfile, "%s/.bin/wsdb", fixture.root);
    EXPECT_TRUE(fs_write_file(dbfile, "not a valid wsdb file"));

    /* Garbage is discarded and the database opens empty, so everything rebuilds. */
    db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    EXPECT_FALSE(wsdb_object_fresh(db, fixture.object, "cmd-v1"));

    wsdb_close(db);
    fixture_teardown(&fixture);
}
