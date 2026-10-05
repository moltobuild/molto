#include <moltest.h>

#include <molto/build/depfile.h>
#include <molto/util/str_list.h>

#include <string.h>

MOLTEST(depfile) {
    /* A typical gcc depfile with a line continuation: the target is dropped and
       the three prerequisites are returned in order. */
    const char *typical =
        "build/debug/obj/main.c.o: src/main.c src/util.h \\\n"
        " src/config.h\n";
    str_list prereqs;
    str_list_init(&prereqs);
    EXPECT_TRUE(depfile_parse(typical, &prereqs));
    EXPECT_TRUE(str_list_count(&prereqs) == 3);
    EXPECT_TRUE(strcmp(str_list_get(&prereqs, 0), "src/main.c") == 0);
    EXPECT_TRUE(strcmp(str_list_get(&prereqs, 1), "src/util.h") == 0);
    EXPECT_TRUE(strcmp(str_list_get(&prereqs, 2), "src/config.h") == 0);
    str_list_free(&prereqs);

    /* Runs of tabs/spaces/newlines produce no empty tokens. */
    str_list spaced;
    str_list_init(&spaced);
    EXPECT_TRUE(depfile_parse("t.o:  a.c\t\tb.h  \n", &spaced));
    EXPECT_TRUE(str_list_count(&spaced) == 2);
    str_list_free(&spaced);

    /* An escaped space is a literal space inside a single path. */
    str_list escaped;
    str_list_init(&escaped);
    EXPECT_TRUE(depfile_parse("t.o: a\\ b.h\n", &escaped));
    EXPECT_TRUE(str_list_count(&escaped) == 1);
    EXPECT_TRUE(strcmp(str_list_get(&escaped, 0), "a b.h") == 0);
    str_list_free(&escaped);

    /* No colon -> malformed -> false and no entries. */
    str_list none;
    str_list_init(&none);
    EXPECT_TRUE(!depfile_parse("no colon here\n", &none));
    EXPECT_TRUE(str_list_count(&none) == 0);
    str_list_free(&none);

    /* Empty text has no colon either. */
    str_list empty;
    str_list_init(&empty);
    EXPECT_TRUE(!depfile_parse("", &empty));
    str_list_free(&empty);

    /* A colon with nothing after it is a valid, empty prerequisite list. */
    str_list bare;
    str_list_init(&bare);
    EXPECT_TRUE(depfile_parse("target.o:\n", &bare));
    EXPECT_TRUE(str_list_count(&bare) == 0);
    str_list_free(&bare);
}

MOLTEST(a_drive_letter_does_not_divide_a_depfile) {
    /* What gcc writes on Windows. Three colons, and only the middle one is the
       separator; splitting on the first leaves a prerequisite list that starts
       with half of the target. */
    const char *text = "D:/ws/build/debug/obj/main.c.o: D:/ws/src/main.c D:/ws/include/a.h\n";

    str_list prereqs;
    str_list_init(&prereqs);
    ASSERT_TRUE(depfile_parse(text, &prereqs));

#ifdef _WIN32
    ASSERT_EQ(2, (int)str_list_count(&prereqs));
    EXPECT_STREQ("D:/ws/src/main.c", str_list_get(&prereqs, 0));
    EXPECT_STREQ("D:/ws/include/a.h", str_list_get(&prereqs, 1));
#else
    /* Here `D` is an ordinary one-letter target and its colon really is the
       separator. Asserted rather than skipped, because the POSIX reading is
       the one that must not change. */
    EXPECT_STREQ("/ws/build/debug/obj/main.c.o:", str_list_get(&prereqs, 0));
#endif
    str_list_free(&prereqs);
}
