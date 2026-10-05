#include <moltest.h>

#include <molto/util/str_list.h>

#include <string.h>

MOLTEST(str_list) {
    str_list list;
    str_list_init(&list);
    EXPECT_TRUE(str_list_count(&list) == 0);
    EXPECT_TRUE(str_list_get(&list, 0) == NULL);

    EXPECT_TRUE(str_list_push(&list, "alpha"));
    EXPECT_TRUE(str_list_push(&list, "beta"));
    EXPECT_TRUE(str_list_count(&list) == 2);
    EXPECT_TRUE(strcmp(str_list_get(&list, 0), "alpha") == 0);
    EXPECT_TRUE(strcmp(str_list_get(&list, 1), "beta") == 0);
    EXPECT_TRUE(str_list_get(&list, 2) == NULL);

    /* Growth past the initial capacity. */
    for (int i = 0; i < 50; i++)
        EXPECT_TRUE(str_list_push(&list, "item"));
    EXPECT_TRUE(str_list_count(&list) == 52);

    str_list_free(&list);
    EXPECT_TRUE(str_list_count(&list) == 0);
}
