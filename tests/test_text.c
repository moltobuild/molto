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

/* To a string a tab is one character. To a terminal it is a jump to the next
   stop, which is what the two functions below are for. */
MOLTEST(a_tab_is_one_character_to_a_string) { EXPECT_EQ(2u, text_columns("\t\tint x;", 2)); }

/* clang reports the `nope` of "\t\treturn nope;" at byte 10; gcc reports the
   same character at column 24, because two tabs are sixteen columns and
   "return " is seven more. Converting the first into the second is the whole
   job. */
MOLTEST(a_byte_offset_becomes_the_column_a_terminal_would_be_at) {
    const char *line = "\t\treturn nope;";
    EXPECT_EQ(23u, text_column_of_byte(line, 9, 8));
    EXPECT_EQ(16u, text_column_of_byte(line, 2, 8)); /* both tabs, no more */
    EXPECT_EQ(8u, text_column_of_byte(line, 1, 8));  /* one tab */
    EXPECT_EQ(0u, text_column_of_byte(line, 0, 8));
}

/* A tab already sitting on a stop moves to the next one rather than staying
   where it is. */
MOLTEST(a_tab_on_a_stop_still_advances) {
    EXPECT_EQ(16u, text_column_of_byte("12345678\tx", 9, 8));
}

/* A multibyte character is one column and several bytes, so the two counts
   drift apart by exactly the extra bytes. */
MOLTEST(a_byte_offset_past_multibyte_text_comes_back_shorter) {
    const char *line = "    /* αβγ */ return nope;";
    EXPECT_EQ(21u, text_column_of_byte(line, 24, 8));
}

MOLTEST(tabs_are_expanded_to_the_stops_a_terminal_uses) {
    char out[64] = "";
    text_expand_tabs("\tx", 8, out, sizeof out);
    EXPECT_STREQ("        x", out);

    text_expand_tabs("ab\tc", 8, out, sizeof out);
    EXPECT_STREQ("ab      c", out);

    text_expand_tabs("no tabs here", 8, out, sizeof out);
    EXPECT_STREQ("no tabs here", out);
}

/* Shortened rather than refused, and never left holding whatever was there. */
MOLTEST(expanding_into_too_small_a_buffer_shortens_the_line) {
    char out[5] = "";
    text_expand_tabs("\tx", 8, out, sizeof out);
    EXPECT_STREQ("    ", out);

    text_expand_tabs(NULL, 8, out, sizeof out);
    EXPECT_STREQ("", out);
}

/* Text that is not valid UTF-8 still has to produce a number: a lone
   continuation byte is counted as nothing, and a terminal will make no more of
   it than that. */
MOLTEST(a_stray_byte_is_not_a_reason_to_refuse_an_answer) {
    EXPECT_EQ(1u, text_columns("a\x80\x80z", 3));
    EXPECT_EQ(2u, text_columns("a\x80\x80z", 4));
}

MOLTEST(no_text_is_no_columns) { EXPECT_EQ(0u, text_columns(NULL, 8)); }
