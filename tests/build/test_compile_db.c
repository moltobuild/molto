#include <moltest.h>

#include <molto/build/compile_db.h>
#include <molto/services/fs_service.h>
#include <molto/util/json.h>
#include <molto/util/str_list.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Compose an argv the way the build service does: driver first, then the rest. */
static void push_all(str_list *argv, const char *const *items, size_t count) {
    for (size_t i = 0; i < count; i++)
        EXPECT_TRUE(str_list_push(argv, items[i]));
}

/* Read the written database back and parse it, which is the only check that
   matters: a document a JSON reader refuses is a database clangd refuses too. */
static json_document *read_database(const char *root) {
    char path[512];
    snprintf(path, sizeof path, "%s/compile_commands.json", root);
    char *text = fs_read_file(path);
    if (text == NULL)
        return NULL;
    json_document *doc = json_parse(text);
    free(text);
    return doc;
}

MOLTEST(compile_db) {
    char root[MOLTEST_PATH];
    ASSERT_TRUE(moltest_temp_dir("molto_cdb", root, sizeof root));

    compile_db *db = compile_db_create();
    ASSERT_NOT_NULL(db);
    EXPECT_EQ(0, (long long)compile_db_count(db));

    /* Two units, with paths under the root: the database reports them relative
       to `directory`, which is what makes it readable and movable. */
    char source[512];
    char object[512];
    snprintf(source, sizeof source, "%s/src/main.c", root);
    snprintf(object, sizeof object, "%s/build/debug/obj/src/main.c.o", root);

    str_list argv;
    str_list_init(&argv);
    const char *items[] = {"/usr/bin/gcc", "-c", source, "-o", object, "-O0", "-g", "-std=c17"};
    push_all(&argv, items, sizeof items / sizeof items[0]);
    /* A define carrying quotes and a backslash: unescaped, either one produces
       a document nothing can parse. */
    EXPECT_TRUE(str_list_push(&argv, "-DMOLTO_PKG_NAME=\"molto\""));
    EXPECT_TRUE(str_list_push(&argv, "-DPATH_SEP=\"\\\\\""));
    EXPECT_TRUE(compile_db_add(db, source, object, &argv));
    str_list_free(&argv);

    /* A second unit outside the root keeps its absolute path: a dependency
       compiled out of the global cache is not under the project. */
    str_list dep_argv;
    str_list_init(&dep_argv);
    const char *dep_items[] = {"/usr/bin/gcc", "-c", "/opt/cache/dot_env/src/dot_env.c", "-o",
                               "/opt/cache/dot_env/dot_env.c.o"};
    push_all(&dep_argv, dep_items, sizeof dep_items / sizeof dep_items[0]);
    EXPECT_TRUE(compile_db_add(db, "/opt/cache/dot_env/src/dot_env.c",
                               "/opt/cache/dot_env/dot_env.c.o", &dep_argv));
    str_list_free(&dep_argv);

    EXPECT_EQ(2, (long long)compile_db_count(db));
    EXPECT_TRUE(compile_db_write(db, root));
    compile_db_destroy(db);

    json_document *doc = read_database(root);
    ASSERT_NOT_NULL(doc);
    json_value array = json_root(doc);
    EXPECT_EQ(2, (long long)json_count(array));

    /* Entries keep the order they were added: two runs over one tree produce
       byte-identical databases, so a diff means something actually changed. */
    json_value first = json_at(array, 0);
    EXPECT_STREQ("src/main.c", json_string(json_get(first, "file")));
    EXPECT_STREQ("build/debug/obj/src/main.c.o", json_string(json_get(first, "output")));
    /* Compared against the resolved root: `directory` is absolute and symlink
       free, because a tool reads this file from wherever it is running. */
    char resolved[PATH_MAX];
    EXPECT_TRUE(fs_real_path(root, resolved, sizeof resolved));
    EXPECT_STREQ(resolved, json_string(json_get(first, "directory")));

    /* The arguments are what was executed, verbatim — escaping is undone by the
       reader, so what comes back is exactly what would reach the compiler. */
    json_value arguments = json_get(first, "arguments");
    EXPECT_EQ(10, (long long)json_count(arguments));
    EXPECT_STREQ("/usr/bin/gcc", json_string(json_at(arguments, 0)));
    EXPECT_STREQ("-c", json_string(json_at(arguments, 1)));
    EXPECT_STREQ(source, json_string(json_at(arguments, 2)));
    EXPECT_STREQ("-DMOLTO_PKG_NAME=\"molto\"", json_string(json_at(arguments, 8)));
    EXPECT_STREQ("-DPATH_SEP=\"\\\\\"", json_string(json_at(arguments, 9)));

    json_value second = json_at(array, 1);
    EXPECT_STREQ("/opt/cache/dot_env/src/dot_env.c", json_string(json_get(second, "file")));
    json_free(doc);

    /* Writing again replaces the previous database rather than appending to it,
       and leaves no temporary behind. */
    compile_db *smaller = compile_db_create();
    ASSERT_NOT_NULL(smaller);
    str_list one;
    str_list_init(&one);
    EXPECT_TRUE(str_list_push(&one, "/usr/bin/gcc"));
    EXPECT_TRUE(compile_db_add(smaller, source, object, &one));
    str_list_free(&one);
    EXPECT_TRUE(compile_db_write(smaller, root));
    compile_db_destroy(smaller);

    doc = read_database(root);
    ASSERT_NOT_NULL(doc);
    EXPECT_EQ(1, (long long)json_count(json_root(doc)));
    json_free(doc);

    char temp[512];
    snprintf(temp, sizeof temp, "%s/.compile_commands.json.tmp", root);
    EXPECT_FALSE(fs_path_exists(temp));

    /* An empty database is still a valid one: it says "nothing was compiled"
       rather than leaving whatever an earlier run happened to write. */
    compile_db *empty = compile_db_create();
    ASSERT_NOT_NULL(empty);
    EXPECT_TRUE(compile_db_write(empty, root));
    compile_db_destroy(empty);
    doc = read_database(root);
    ASSERT_NOT_NULL(doc);
    EXPECT_EQ(0, (long long)json_count(json_root(doc)));
    json_free(doc);

    /* NULL is the "no database wanted" case, and every call tolerates it so no
       call site has to test for it. */
    EXPECT_TRUE(compile_db_add(NULL, "a.c", "a.o", NULL));
    EXPECT_EQ(0, (long long)compile_db_count(NULL));
    EXPECT_TRUE(compile_db_write(NULL, root));
    compile_db_destroy(NULL);

    char cmd[600];
    (void)fs_remove_tree(root);
}
