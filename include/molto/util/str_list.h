#ifndef MOLTO_STR_LIST_H
#define MOLTO_STR_LIST_H

#include <stdbool.h>
#include <stddef.h>

/* A growable list of heap-owned strings. */
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} str_list;

/* Initialize an empty list. */
void str_list_init(str_list *list);

/* Append a copy of `value` to the list. Returns false on allocation failure. */
[[nodiscard]] bool str_list_push(str_list *list, const char *value);

/* Number of elements in the list. */
[[nodiscard]] size_t str_list_count(const str_list *list);

/* Element at `index`, or NULL if out of range. */
[[nodiscard]] const char *str_list_get(const str_list *list, size_t index);

/* Free every element and reset the list to empty. */
void str_list_free(str_list *list);

#endif /* MOLTO_STR_LIST_H */
