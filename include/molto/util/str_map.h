#ifndef MOLTO_STR_MAP_H
#define MOLTO_STR_MAP_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A string-keyed map (the C equivalent of a dictionary). Internally it keeps a
 * dense array of entries — the iterable "base" — plus a hash index mapping each
 * key to its position in that array, giving O(1) lookup and cheap iteration.
 *
 * Values are opaque `void *`, so any pointer (including a pointer to a struct)
 * can be stored and cast back on retrieval. Keys are copied. An optional
 * free_value callback releases values on remove/destroy.
 */
typedef struct str_map str_map;

/* Called to free a stored value. NULL if values are not owned by the map. */
typedef void (*str_map_free_fn)(void *value);

/* Create an empty map. `free_value` may be NULL. Returns NULL on failure. */
[[nodiscard]] str_map *str_map_create(str_map_free_fn free_value);

/* Insert or replace `key` -> `value` (a copy of `key` is stored). If the key
   already exists its previous value is freed. Returns false on allocation
   failure. */
[[nodiscard]] bool str_map_put(str_map *map, const char *key, void *value);

/* Value for `key`, or NULL if absent. */
[[nodiscard]] void *str_map_get(const str_map *map, const char *key);

/* Remove `key` (freeing its value). Returns true if it was present. */
bool str_map_remove(str_map *map, const char *key);

/* Number of entries. */
[[nodiscard]] size_t str_map_size(const str_map *map);

/* Visit every entry in storage order. `ctx` is passed through. */
void str_map_foreach(const str_map *map,
                     void (*fn)(const char *key, void *value, void *ctx),
                     void *ctx);

/* Free the map and, via the free_value callback, its values. Safe on NULL. */
void str_map_destroy(str_map *map);

#endif /* MOLTO_STR_MAP_H */
