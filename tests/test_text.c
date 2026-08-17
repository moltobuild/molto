#include <moltest.h>

#include <molto/util/text.h>

#include <string.h>

/*
 * The one arithmetic a caret depends on.
 *
 * Every case here is a byte count going in and a column count coming out,
 * because that is the direction the conversion runs: a compiler names a byte,
 * a terminal understands columns.
 */

MOLTEST(ascii_costs_one_column_a_byte) {
    const char *line = "    return user.name;";
    EXPECT_EQ(0u, text_columns(line, 0));
    EXPECT_EQ(4u, text_columns(line, 4));
    EXPECT_EQ(strlen(line), text_columns(line, strlen(line)));
}

/* On a line that opens with a comment holding "αβγ", gcc reports the `x` of a
   following `int x;` at column 28, which is its byte offset; the caret belongs
   at column 25. Three two-byte Greek letters are the whole of the difference. */
MOLTEST(a_multibyte_character_costs_one_column_and_several_bytes) {
    const char *line = "/*αβγ*/int x;";
    EXPECT_EQ(3u, text_columns(line, 3));  /* three bytes in, three of them counted */
    EXPECT_EQ(8u, text_columns(line, 11)); /* eleven bytes in, up to the "i" of int */
}

/* A byte count past the end is the whole string, not a walk off it. */
MOLTEST(a_count_past_the_end_stops_at_the_end) {
    EXPECT_EQ(5u, text_columns("hello", 500));
    EXPECT_EQ(0u, text_columns("", 500));
}

/* A tab is one byte and one reported column, which is why an excerpt may
   substitute a single space for it and keep the arithmetic exact. */
MOLTEST(a_tab_is_one_column) { EXPECT_EQ(2u, text_columns("\t\tint x;", 2)); }

/* Text that is not valid UTF-8 still has to produce a number: a lone
   continuation byte is counted as nothing, and a terminal will make no more of
   it than that. */
MOLTEST(a_stray_byte_is_not_a_reason_to_refuse_an_answer) {
    EXPECT_EQ(1u, text_columns("a\x80\x80z", 3));
    EXPECT_EQ(2u, text_columns("a\x80\x80z", 4));
}

MOLTEST(no_text_is_no_columns) { EXPECT_EQ(0u, text_columns(NULL, 8)); }
