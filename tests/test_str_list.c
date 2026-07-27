#include "test_framework.h"
#include "tests.h"

#include <molto/util/str_list.h>

#include <string.h>

void suite_str_list(void) {
    str_list list;
    str_list_init(&list);
    CHECK(str_list_count(&list) == 0);
    CHECK(str_list_get(&list, 0) == NULL);

    CHECK(str_list_push(&list, "alpha"));
    CHECK(str_list_push(&list, "beta"));
    CHECK(str_list_count(&list) == 2);
    CHECK(strcmp(str_list_get(&list, 0), "alpha") == 0);
    CHECK(strcmp(str_list_get(&list, 1), "beta") == 0);
    CHECK(str_list_get(&list, 2) == NULL);

    /* Growth past the initial capacity. */
    for (int i = 0; i < 50; i++)
        CHECK(str_list_push(&list, "item"));
    CHECK(str_list_count(&list) == 52);

    str_list_free(&list);
    CHECK(str_list_count(&list) == 0);
}
