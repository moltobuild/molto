#include <molto/util/str_map.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Empty slot marker in the hash index. */
#define SLOT_EMPTY SIZE_MAX

#define INDEX_INITIAL_CAPACITY 16
#define ENTRIES_INITIAL_CAPACITY 8

typedef struct {
    char *key;
    void *value;
} map_entry;

struct str_map {
    map_entry *entries;   /* dense array of entries (the iterable base) */
    size_t count;
    size_t capacity;
    size_t *index;        /* hash slots -> position in entries[], or SLOT_EMPTY */
    size_t index_capacity; /* power of two */
    str_map_free_fn free_value;
};

/* FNV-1a 64-bit hash of a key. */
static uint64_t hash_key(const char *key) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p != '\0'; p++) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Find the slot for `key`: the matching slot (sets *found) or the empty slot
   where it would be inserted. Requires a non-empty index. */
static size_t probe(const str_map *map, const char *key, bool *found) {
    size_t mask = map->index_capacity - 1;
    size_t slot = (size_t)(hash_key(key) & mask);
    while (map->index[slot] != SLOT_EMPTY) {
        if (strcmp(map->entries[map->index[slot]].key, key) == 0) {
            *found = true;
            return slot;
        }
        slot = (slot + 1) & mask;
    }
    *found = false;
    return slot;
}

/* Reinsert every entry into a freshly cleared index of `index_capacity`. */
static void rebuild_index(str_map *map) {
    for (size_t i = 0; i < map->index_capacity; i++)
        map->index[i] = SLOT_EMPTY;
    size_t mask = map->index_capacity - 1;
    for (size_t i = 0; i < map->count; i++) {
        size_t slot = (size_t)(hash_key(map->entries[i].key) & mask);
        while (map->index[slot] != SLOT_EMPTY)
            slot = (slot + 1) & mask;
        map->index[slot] = i;
    }
}

static bool grow_index(str_map *map, size_t new_capacity) {
    size_t *index = malloc(new_capacity * sizeof(size_t));
    if (index == NULL)
        return false;
    free(map->index);
    map->index = index;
    map->index_capacity = new_capacity;
    rebuild_index(map);
    return true;
}

static bool grow_entries(str_map *map) {
    size_t next = map->capacity == 0 ? ENTRIES_INITIAL_CAPACITY : map->capacity * 2;
    map_entry *entries = realloc(map->entries, next * sizeof(map_entry));
    if (entries == NULL)
        return false;
    map->entries = entries;
    map->capacity = next;
    return true;
}

str_map *str_map_create(str_map_free_fn free_value) {
    str_map *map = calloc(1, sizeof *map);
    if (map != NULL)
        map->free_value = free_value;
    return map;
}

bool str_map_put(str_map *map, const char *key, void *value) {
    if (map->index_capacity == 0) {
        if (!grow_index(map, INDEX_INITIAL_CAPACITY))
            return false;
    } else if ((map->count + 1) * 10 >= map->index_capacity * 7) {
        if (!grow_index(map, map->index_capacity * 2))
            return false;
    }

    bool found;
    size_t slot = probe(map, key, &found);
    if (found) {
        map_entry *entry = &map->entries[map->index[slot]];
        if (map->free_value != NULL)
            map->free_value(entry->value);
        entry->value = value;
        return true;
    }

    if (map->count == map->capacity && !grow_entries(map))
        return false;
    char *key_copy = strdup(key);
    if (key_copy == NULL)
        return false;
    map->entries[map->count] = (map_entry){ .key = key_copy, .value = value };
    map->index[slot] = map->count;
    map->count++;
    return true;
}

void *str_map_get(const str_map *map, const char *key) {
    if (map->index_capacity == 0)
        return NULL;
    bool found;
    size_t slot = probe(map, key, &found);
    return found ? map->entries[map->index[slot]].value : NULL;
}

bool str_map_remove(str_map *map, const char *key) {
    if (map->index_capacity == 0)
        return false;
    bool found;
    size_t slot = probe(map, key, &found);
    if (!found)
        return false;

    size_t pos = map->index[slot];
    free(map->entries[pos].key);
    if (map->free_value != NULL)
        map->free_value(map->entries[pos].value);

    /* Swap the last entry into the hole, then rebuild the index (simple and
       correct; removals are infrequent). */
    size_t last = map->count - 1;
    if (pos != last)
        map->entries[pos] = map->entries[last];
    map->count--;
    rebuild_index(map);
    return true;
}

size_t str_map_size(const str_map *map) {
    return map->count;
}

void str_map_foreach(const str_map *map,
                     void (*fn)(const char *key, void *value, void *ctx),
                     void *ctx) {
    for (size_t i = 0; i < map->count; i++)
        fn(map->entries[i].key, map->entries[i].value, ctx);
}

void str_map_destroy(str_map *map) {
    if (map == NULL)
        return;
    for (size_t i = 0; i < map->count; i++) {
        free(map->entries[i].key);
        if (map->free_value != NULL)
            map->free_value(map->entries[i].value);
    }
    free(map->entries);
    free(map->index);
    free(map);
}
