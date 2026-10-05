#include <moltest.h>

#include <molto/util/json.h>
#include <molto/util/json_write.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Writing JSON, checked by reading it back.
 *
 * Every assertion here that matters is made against the repository's own
 * parser: a writer tested only against an expected string proves that it wrote
 * what the test author imagined, and a writer tested against a parser proves
 * it wrote a document. */

/* Run `write` into a buffer and hand back the text. Caller frees.

   Through the writer's own buffer destination rather than `open_memstream`,
   which is POSIX and does not exist on Windows. It also means the tests
   exercise the destination the frontend request uses, which is the one with
   the overflow rule nothing else would cover. */
#define WRITTEN_MAX 8192

static char *written_by(void (*write)(json_writer *)) {
    char *text = malloc(WRITTEN_MAX);
    if (text == NULL)
        return NULL;

    json_writer writer;
    json_writer_init_buffer(&writer, text, WRITTEN_MAX);
    write(&writer);
    if (json_writer_overflowed(&writer)) {
        free(text);
        return NULL;
    }
    return text;
}

static void write_escapes(json_writer *writer) {
    json_object_open(writer, NULL);
    json_write_field(writer, "quote", "a \" b");
    json_write_field(writer, "backslash", "a \\ b");
    json_write_field(writer, "newline", "a\nb");
    json_write_field(writer, "tab", "a\tb");
    json_write_field(writer, "control", "a\x01" "b");
    json_object_close(writer);
}

MOLTEST(json_write_escapes_what_would_break_a_document) {
    char *text = written_by(write_escapes);
    ASSERT_NOT_NULL(text);

    json_document *doc = json_parse(text);
    ASSERT_NOT_NULL(doc);
    const json_value root = json_root(doc);

    /* Round-tripped, so the escaping is right in both directions. */
    EXPECT_STREQ("a \" b", json_string(json_get(root, "quote")));
    EXPECT_STREQ("a \\ b", json_string(json_get(root, "backslash")));
    EXPECT_STREQ("a\nb", json_string(json_get(root, "newline")));
    EXPECT_STREQ("a\tb", json_string(json_get(root, "tab")));
    EXPECT_STREQ("a\x01" "b", json_string(json_get(root, "control")));

    /* A control character has to leave as \u00xx, not as itself. */
    EXPECT_NOT_NULL(strstr(text, "\\u0001"));

    json_free(doc);
    free(text);
}

static void write_nested(json_writer *writer) {
    json_object_open(writer, NULL);
    json_write_field(writer, "name", "my_app");
    json_array_open(writer, "components");
    json_object_open(writer, NULL);
    json_write_field(writer, "name", "png");
    json_array_open(writer, "tags");
    json_write_element(writer, "a");
    json_write_element(writer, "b");
    json_array_close(writer);
    json_object_close(writer);
    json_array_close(writer);
    json_object_close(writer);
}

MOLTEST(json_write_nests_objects_and_arrays) {
    char *text = written_by(write_nested);
    ASSERT_NOT_NULL(text);

    json_document *doc = json_parse(text);
    ASSERT_NOT_NULL(doc);
    const json_value root = json_root(doc);

    EXPECT_STREQ("my_app", json_string(json_get(root, "name")));
    const json_value components = json_get(root, "components");
    ASSERT_EQ(1u, json_count(components));

    const json_value first = json_at(components, 0);
    EXPECT_STREQ("png", json_string(json_get(first, "name")));
    const json_value tags = json_get(first, "tags");
    ASSERT_EQ(2u, json_count(tags));
    EXPECT_STREQ("a", json_string(json_at(tags, 0)));
    EXPECT_STREQ("b", json_string(json_at(tags, 1)));

    json_free(doc);
    free(text);
}

static void write_empty(json_writer *writer) {
    json_object_open(writer, NULL);
    json_array_open(writer, "components");
    json_array_close(writer);
    json_object_open(writer, "metadata");
    json_object_close(writer);
    json_object_close(writer);
}

MOLTEST(json_write_leaves_an_empty_container_on_one_line) {
    /* A package with no dependencies is the ordinary case, not the edge one,
       and `"components": []` is what it has to say. Spread over two lines it is
       still valid and still unreadable. */
    char *text = written_by(write_empty);
    ASSERT_NOT_NULL(text);

    EXPECT_NOT_NULL(strstr(text, "\"components\": []"));
    EXPECT_NOT_NULL(strstr(text, "\"metadata\": {}"));

    json_document *doc = json_parse(text);
    ASSERT_NOT_NULL(doc);
    EXPECT_EQ(0u, json_count(json_get(json_root(doc), "components")));

    json_free(doc);
    free(text);
}

MOLTEST(json_write_is_byte_for_byte_repeatable) {
    /* The whole value of a document that can be committed is that its diff is
       worth reading, and that requires two runs to agree exactly. */
    char *first = written_by(write_nested);
    char *second = written_by(write_nested);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    EXPECT_STREQ(first, second);
    free(first);
    free(second);
}
