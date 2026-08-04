#include <moltest.h>

#include <molto/services/fs_service.h>
#include <molto/util/str_list.h>
#include <molto/workspace/wsdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
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

/* Header of a well-formed database file: magic, version, entry count. */
static bool write_wsdb_header(FILE *file, uint32_t version, uint32_t count) {
    return fwrite("MOLTOWSDB", 1, 9, file) == 9
        && fwrite(&version, sizeof version, 1, file) == 1
        && fwrite(&count, sizeof count, 1, file) == 1;
}

/* Length-prefixed string, as the database stores them. */
static bool write_wsdb_string(FILE *file, const char *text) {
    uint32_t length = (uint32_t)strlen(text);
    return fwrite(&length, sizeof length, 1, file) == 1
        && fwrite(text, 1, length, file) == length;
}

/* An artifact entry: kind byte, key, command, and (objects only) prerequisites. */
static bool write_wsdb_artifact(FILE *file, int kind, const char *key, const char *command,
                                bool with_prereqs) {
    if (fputc(kind, file) == EOF || !write_wsdb_string(file, key)
        || !write_wsdb_string(file, command))
        return false;
    if (!with_prereqs)
        return true;
    uint32_t prereq_count = 0;
    return fwrite(&prereq_count, sizeof prereq_count, 1, file) == 1;
}

MOLTEST(wsdb_discards_a_database_with_an_unknown_entry_kind) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    /* A structurally valid file whose first entry carries a kind byte that names
       nothing. The rest of the file parses fine, so only an explicit check
       catches it — and one bad entry condemns the whole file. */
    char dbfile[256];
    snprintf(dbfile, sizeof dbfile, "%s/.bin", fixture.root);
    ASSERT_TRUE(fs_make_dirs(dbfile));
    snprintf(dbfile, sizeof dbfile, "%s/.bin/wsdb", fixture.root);
    FILE *file = fopen(dbfile, "wb");
    ASSERT_NOT_NULL(file);
    EXPECT_TRUE(write_wsdb_header(file, 2u, 2u));
    EXPECT_TRUE(write_wsdb_artifact(file, 99, "build/debug/obj/src/ghost.c.o",
                                    "cmd-ghost", false));
    EXPECT_TRUE(write_wsdb_artifact(file, 1, "build/debug/obj/src/main.c.o",
                                    "cmd-v1", true));
    EXPECT_TRUE(fclose(file) == 0);

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    /* The well-formed second entry is discarded along with the corrupt one. */
    EXPECT_FALSE(wsdb_object_fresh(db, fixture.object, "cmd-v1"));

    EXPECT_TRUE(wsdb_close(db));
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_reports_a_state_it_could_not_save) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    wsdb_record_object(db, fixture.object, "cmd-v1", &fixture.prereqs);

    /* Make .bin/ read-only so the atomic save cannot write its staging file.
       Losing the incremental state must be reported, not swallowed. */
    char bindir[256];
    snprintf(bindir, sizeof bindir, "%s/.bin", fixture.root);
    ASSERT_TRUE(chmod(bindir, 0500) == 0);

    EXPECT_FALSE(wsdb_close(db));

    EXPECT_TRUE(chmod(bindir, 0700) == 0);
    fixture_teardown(&fixture);
}

/* --- analysis results (RFC-0006) --- */

MOLTEST(wsdb_replays_a_result_until_a_prerequisite_changes) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);

    str_list diagnostics;
    str_list_init(&diagnostics);
    ASSERT_TRUE(str_list_push(&diagnostics, "src/main.c"));
    ASSERT_TRUE(str_list_push(&diagnostics, "unused variable 'x'"));

    EXPECT_TRUE(wsdb_record_result(db, "lint:src/main.c", "fp-v1", &fixture.prereqs,
                                   &diagnostics));
    EXPECT_TRUE(wsdb_result_fresh(db, "lint:src/main.c", "fp-v1"));
    /* The fingerprint carries the command, the tool version and the translated
       configuration, so a change in any of them is a different analysis. */
    EXPECT_FALSE(wsdb_result_fresh(db, "lint:src/main.c", "fp-v2"));

    str_list read_back;
    str_list_init(&read_back);
    ASSERT_TRUE(wsdb_result_values(db, "lint:src/main.c", &read_back));
    ASSERT_EQ(2, str_list_count(&read_back));
    EXPECT_STREQ("unused variable 'x'", str_list_get(&read_back, 1));

    /* Editing a header re-analyses what included it, which is the whole reason
       the prerequisites are watched rather than just the file itself. */
    sleep(1);
    write_at(fixture.root, "src/util.h", "int answer(void);\nint other(void);\n");
    EXPECT_FALSE(wsdb_result_fresh(db, "lint:src/main.c", "fp-v1"));

    str_list_free(&read_back);
    str_list_free(&diagnostics);
    wsdb_close(db);
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_tells_a_clean_file_apart_from_one_never_analysed) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);

    str_list empty;
    str_list_init(&empty);
    EXPECT_TRUE(wsdb_record_result(db, "lint:src/main.c", "fp-v1", &fixture.prereqs, &empty));

    /* A file that was analysed and had nothing to say is a recorded answer.
       Conflating it with an absent entry is how a cache ends up re-analysing
       every clean file — or worse, replaying silence for a file with
       warnings. */
    str_list read_back;
    str_list_init(&read_back);
    EXPECT_TRUE(wsdb_result_values(db, "lint:src/main.c", &read_back));
    EXPECT_EQ(0, str_list_count(&read_back));
    EXPECT_TRUE(wsdb_result_fresh(db, "lint:src/main.c", "fp-v1"));

    EXPECT_FALSE(wsdb_result_values(db, "lint:src/never.c", &read_back));
    EXPECT_FALSE(wsdb_result_fresh(db, "lint:src/never.c", "fp-v1"));

    str_list_free(&read_back);
    str_list_free(&empty);
    wsdb_close(db);
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_survives_a_result_round_trip_through_the_file) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    str_list diagnostics;
    str_list_init(&diagnostics);
    ASSERT_TRUE(str_list_push(&diagnostics, "a message with  spaces and a\ttab"));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);
    EXPECT_TRUE(wsdb_record_result(db, "lint:src/main.c", "fp-v1", &fixture.prereqs,
                                   &diagnostics));
    EXPECT_TRUE(wsdb_close(db));

    /* Values are length-prefixed on disk, so a diagnostic needs no escaping and
       survives whatever a compiler chose to print. */
    wsdb *reopened = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(reopened);
    str_list read_back;
    str_list_init(&read_back);
    ASSERT_TRUE(wsdb_result_values(reopened, "lint:src/main.c", &read_back));
    ASSERT_EQ(1, str_list_count(&read_back));
    EXPECT_STREQ("a message with  spaces and a\ttab", str_list_get(&read_back, 0));
    EXPECT_TRUE(wsdb_result_fresh(reopened, "lint:src/main.c", "fp-v1"));

    str_list_free(&read_back);
    str_list_free(&diagnostics);
    wsdb_close(reopened);
    fixture_teardown(&fixture);
}

MOLTEST(wsdb_refuses_a_result_that_watches_nothing) {
    workspace_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    wsdb *db = wsdb_open(fixture.root);
    ASSERT_NOT_NULL(db);

    str_list nothing;
    str_list_init(&nothing);
    /* An entry with no prerequisites could never go stale, so it would answer
       "fresh" for the rest of the workspace's life. */
    EXPECT_FALSE(wsdb_record_result(db, "lint:src/main.c", "fp-v1", &nothing, &nothing));
    EXPECT_FALSE(wsdb_result_fresh(db, "lint:src/main.c", "fp-v1"));

    str_list_free(&nothing);
    wsdb_close(db);
    fixture_teardown(&fixture);
}
