#include "test_framework.h"
#include "tests.h"

#include <molto/build/depfile.h>
#include <molto/util/str_list.h>

#include <string.h>

void suite_depfile(void) {
    /* A typical gcc depfile with a line continuation: the target is dropped and
       the three prerequisites are returned in order. */
    const char *typical =
        "build/debug/obj/main.c.o: src/main.c src/util.h \\\n"
        " src/config.h\n";
    str_list prereqs;
    str_list_init(&prereqs);
    CHECK(depfile_parse(typical, &prereqs));
    CHECK(str_list_count(&prereqs) == 3);
    CHECK(strcmp(str_list_get(&prereqs, 0), "src/main.c") == 0);
    CHECK(strcmp(str_list_get(&prereqs, 1), "src/util.h") == 0);
    CHECK(strcmp(str_list_get(&prereqs, 2), "src/config.h") == 0);
    str_list_free(&prereqs);

    /* Runs of tabs/spaces/newlines produce no empty tokens. */
    str_list spaced;
    str_list_init(&spaced);
    CHECK(depfile_parse("t.o:  a.c\t\tb.h  \n", &spaced));
    CHECK(str_list_count(&spaced) == 2);
    str_list_free(&spaced);

    /* An escaped space is a literal space inside a single path. */
    str_list escaped;
    str_list_init(&escaped);
    CHECK(depfile_parse("t.o: a\\ b.h\n", &escaped));
    CHECK(str_list_count(&escaped) == 1);
    CHECK(strcmp(str_list_get(&escaped, 0), "a b.h") == 0);
    str_list_free(&escaped);

    /* No colon -> malformed -> false and no entries. */
    str_list none;
    str_list_init(&none);
    CHECK(!depfile_parse("no colon here\n", &none));
    CHECK(str_list_count(&none) == 0);
    str_list_free(&none);

    /* Empty text has no colon either. */
    str_list empty;
    str_list_init(&empty);
    CHECK(!depfile_parse("", &empty));
    str_list_free(&empty);

    /* A colon with nothing after it is a valid, empty prerequisite list. */
    str_list bare;
    str_list_init(&bare);
    CHECK(depfile_parse("target.o:\n", &bare));
    CHECK(str_list_count(&bare) == 0);
    str_list_free(&bare);
}
