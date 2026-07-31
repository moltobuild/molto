#include <moltest.h>

#include <molto/util/str_map.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int free_count = 0;
static void counting_free(void *value) {
    free_count++;
    free(value);
}

static size_t foreach_seen = 0;
static void count_visit(const char *key, void *value, void *ctx) {
    (void)key;
    (void)value;
    (void)ctx;
    foreach_seen++;
}

static char *heap_string(const char *text) {
    char *copy = malloc(strlen(text) + 1);
    strcpy(copy, text);
    return copy;
}

MOLTEST(str_map) {
    str_map *map = str_map_create(counting_free);
    EXPECT_TRUE(map != NULL);
    EXPECT_TRUE(str_map_size(map) == 0);
    EXPECT_TRUE(str_map_get(map, "missing") == NULL);

    /* Put and get; value is an opaque pointer cast back on retrieval. */
    EXPECT_TRUE(str_map_put(map, "alpha", heap_string("one")));
    EXPECT_TRUE(str_map_put(map, "beta", heap_string("two")));
    EXPECT_TRUE(str_map_size(map) == 2);
    EXPECT_TRUE(strcmp((char *)str_map_get(map, "alpha"), "one") == 0);
    EXPECT_TRUE(strcmp((char *)str_map_get(map, "beta"), "two") == 0);

    /* Overwrite frees the previous value and keeps the count. */
    free_count = 0;
    EXPECT_TRUE(str_map_put(map, "alpha", heap_string("uno")));
    EXPECT_TRUE(free_count == 1);
    EXPECT_TRUE(strcmp((char *)str_map_get(map, "alpha"), "uno") == 0);
    EXPECT_TRUE(str_map_size(map) == 2);

    /* Remove frees the value and drops the key. */
    free_count = 0;
    EXPECT_TRUE(str_map_remove(map, "beta"));
    EXPECT_TRUE(free_count == 1);
    EXPECT_TRUE(str_map_get(map, "beta") == NULL);
    EXPECT_TRUE(str_map_size(map) == 1);
    EXPECT_TRUE(!str_map_remove(map, "beta"));

    /* Growth: insert many keys, then read them all back. */
    for (int i = 0; i < 200; i++) {
        char key[16];
        snprintf(key, sizeof key, "k%d", i);
        char value[16];
        snprintf(value, sizeof value, "v%d", i);
        EXPECT_TRUE(str_map_put(map, key, heap_string(value)));
    }
    EXPECT_TRUE(str_map_size(map) == 201); /* 200 + "alpha" */
    EXPECT_TRUE(strcmp((char *)str_map_get(map, "k123"), "v123") == 0);
    EXPECT_TRUE(strcmp((char *)str_map_get(map, "k0"), "v0") == 0);

    /* foreach visits every entry once. */
    foreach_seen = 0;
    str_map_foreach(map, count_visit, NULL);
    EXPECT_TRUE(foreach_seen == 201);

    str_map_destroy(map);
}
