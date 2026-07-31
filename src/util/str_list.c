#include <molto/util/str_list.h>

#include <stdlib.h>
#include <string.h>

void str_list_init(str_list *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool str_list_grow(str_list *list) {
    size_t next = list->capacity == 0 ? 8 : list->capacity * 2;
    char **items = realloc(list->items, next * sizeof(char *));
    if (items == NULL)
        return false;
    list->items = items;
    list->capacity = next;
    return true;
}

bool str_list_push(str_list *list, const char *value) {
    if (list->count == list->capacity && !str_list_grow(list))
        return false;
    char *copy = strdup(value);
    if (copy == NULL)
        return false;
    list->items[list->count] = copy;
    list->count++;
    return true;
}

size_t str_list_count(const str_list *list) {
    return list->count;
}

const char *str_list_get(const str_list *list, size_t index) {
    if (index >= list->count)
        return NULL;
    return list->items[index];
}

/* Compare two elements through the pointers qsort hands to the callback. */
static int compare_items(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

void str_list_sort(str_list *list) {
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(char *), compare_items);
}

void str_list_free(str_list *list) {
    for (size_t i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    str_list_init(list);
}
