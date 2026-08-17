#include <moltest.h>

#include <molto/util/viewport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The region, taken apart into the two halves it was built as: the arithmetic,
   which composes bytes and answers questions and touches no stream at all, and
   the drawing, which is pinned against a temporary file.
 *
 * Everything here is the first half. */

/* What a terminal would advance by, counted independently of the code under
   test: a test that measures with the implementation's own ruler measures
   nothing. Escapes are skipped, and a character is one column however many
   bytes it takes. */
static size_t visible_columns(const char *s) {
    size_t columns = 0;
    size_t i = 0;
    while(s[i] != '\0') {
        if(s[i] == '\033' && s[i + 1] == '[') {
            i += 2;
            while(s[i] != '\0' && (s[i] < '@' || s[i] > '~'))
                i++;
            if(s[i] != '\0')
                i++;
            continue;
        }
        if(((unsigned char)s[i] & 0xC0) != 0x80)
            columns++;
        i++;
    }
    return columns;
}

#define FIT_BUFFER 512

MOLTEST(a_line_that_fits_is_copied_unchanged) {
    char out[FIT_BUFFER];

    EXPECT_EQ(11u, viewport_fit("hello world", 40, out, sizeof out));
    EXPECT_STREQ("hello world", out);

    /* Exactly the width is not too wide. */
    EXPECT_EQ(11u, viewport_fit("hello world", 11, out, sizeof out));
    EXPECT_STREQ("hello world", out);
}

/* A glyph is three bytes and one column, so a cut counted in bytes would land
   two thirds of the way into a character and put a broken one on the screen. */
MOLTEST(a_line_is_cut_by_columns_and_not_by_bytes) {
    char out[FIT_BUFFER];
    char blocks[121] = "";
    for(size_t i = 0; i < 40; i++)
        (void)strcat(blocks, "█");

    EXPECT_EQ(30u, viewport_fit(blocks, 10, out, sizeof out));
    EXPECT_EQ(10u, visible_columns(out));
}

MOLTEST(an_escape_costs_bytes_and_no_columns) {
    char out[FIT_BUFFER];

    /* Three columns of a line whose first six bytes are worth none of them. */
    EXPECT_TRUE(viewport_fit("\033[32m●\033[0m project", 3, out, sizeof out) > 0);
    EXPECT_EQ(3u, visible_columns(out));
    EXPECT_EQ(0, strncmp(out, "\033[32m●\033[0m", 12));
}

MOLTEST(a_cut_never_lands_inside_an_escape) {
    char out[FIT_BUFFER];

    /* The budget runs out where the sequence begins. */
    (void)viewport_fit("ab\033[31mcdef", 2, out, sizeof out);
    EXPECT_STREQ("ab", out);

    /* And every escape that did survive is a whole one. */
    (void)viewport_fit("\033[31mabcdef", 3, out, sizeof out);
    for(size_t i = 0; out[i] != '\0'; i++) {
        if(out[i] != '\033')
            continue;
        EXPECT_EQ('[', out[i + 1]);
        size_t end = i + 2;
        while(out[end] != '\0' && (out[end] < '@' || out[end] > '~'))
            end++;
        EXPECT_TRUE(out[end] != '\0');
    }
}

MOLTEST(a_cut_never_lands_inside_a_character) {
    char out[FIT_BUFFER];

    /* Two bytes each, so three columns is six bytes and not five. */
    EXPECT_EQ(6u, viewport_fit("áéíóú", 3, out, sizeof out));
    EXPECT_STREQ("áéí", out);
}

MOLTEST(a_truncated_line_closes_the_colour_it_opened) {
    char out[FIT_BUFFER];

    (void)viewport_fit("\033[31mabcdef", 3, out, sizeof out);
    EXPECT_STREQ("\033[31mabc\033[0m", out);

    /* Colour that the line itself had already closed is not reopened to be
       closed again. */
    (void)viewport_fit("\033[31mab\033[0mcdef", 4, out, sizeof out);
    EXPECT_NULL(strstr(out, "\033[0m\033[0m"));
}

/* The pin for NO_COLOR: a stream that asked for no escapes receives none, and
   a cut is not an excuse to write one. */
MOLTEST(a_plain_line_that_is_cut_gains_no_escape) {
    char out[FIT_BUFFER];

    EXPECT_EQ(3u, viewport_fit("abcdef", 3, out, sizeof out));
    EXPECT_STREQ("abc", out);
    EXPECT_NULL(strchr(out, '\033'));
}

MOLTEST(a_line_with_no_room_at_all_is_an_empty_line) {
    char out[FIT_BUFFER];

    EXPECT_EQ(0u, viewport_fit("hello", 0, out, sizeof out));
    EXPECT_STREQ("", out);

    EXPECT_EQ(0u, viewport_fit(NULL, 10, out, sizeof out));
    EXPECT_STREQ("", out);
}

/* A buffer sized by the column count would be overrun by the third character
   outside ASCII. Refuse rather than half-write, as the bar does. */
MOLTEST(a_buffer_sized_in_columns_is_refused) {
    char out[16];

    EXPECT_EQ(0u, viewport_fit("hello", 12, out, sizeof out));
    EXPECT_STREQ("", out);
}

/* Escapes cost bytes and no columns, so a line of nothing else has no length a
   column count can predict. It must stop at the buffer, not at the budget. */
MOLTEST(a_line_of_nothing_but_escapes_stops_at_the_buffer) {
    char line[4096] = "";
    for(size_t i = 0; i < 400; i++)
        (void)strcat(line, "\033[31m");
    (void)strcat(line, "abc");

    char out[VIEWPORT_FIT_SIZE(8)];
    const size_t written = viewport_fit(line, 8, out, sizeof out);
    EXPECT_TRUE(written < sizeof out);
    EXPECT_EQ(written, strlen(out));
}

MOLTEST(the_region_is_a_third_of_the_screen_at_most) {
    EXPECT_EQ(8u, viewport_height(20, 24, 8));
    EXPECT_EQ(4u, viewport_height(20, 12, 8));

    /* Never more rows than there is work to put in them. */
    EXPECT_EQ(2u, viewport_height(2, 24, 8));
    EXPECT_EQ(0u, viewport_height(0, 24, 8));

    /* A screen too short to spare three rows keeps the bar and nothing else. */
    EXPECT_EQ(0u, viewport_height(20, 2, 8));
}

MOLTEST(the_size_comes_from_the_environment_when_nothing_else_will_say) {
    viewport_size size = viewport_size_from_env("120", "40");
    EXPECT_EQ(120u, size.columns);
    EXPECT_EQ(40u, size.rows);

    size = viewport_size_from_env(NULL, NULL);
    EXPECT_EQ((size_t)VIEWPORT_FALLBACK_COLUMNS, size.columns);
    EXPECT_EQ((size_t)VIEWPORT_FALLBACK_ROWS, size.rows);

    /* Neither rubbish nor zero is a measurement, and the two axes fall back
       one at a time. */
    size = viewport_size_from_env("abc", "0");
    EXPECT_EQ((size_t)VIEWPORT_FALLBACK_COLUMNS, size.columns);
    EXPECT_EQ((size_t)VIEWPORT_FALLBACK_ROWS, size.rows);

    size = viewport_size_from_env("100000", "100000");
    EXPECT_EQ((size_t)VIEWPORT_COLUMNS_MAX, size.columns);
    EXPECT_EQ((size_t)VIEWPORT_ROWS_MAX, size.rows);
}

/* A temporary file is not a terminal, so the ioctl cannot answer and the
   environment is what is left. That fallback is the path the tests above pin,
   and this is what reaches it. */
MOLTEST(a_stream_with_no_terminal_behind_it_falls_back) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);

    (void)setenv("COLUMNS", "97", 1);
    (void)setenv("LINES", "31", 1);
    const viewport_size size = viewport_measure(out);
    EXPECT_EQ(97u, size.columns);
    EXPECT_EQ(31u, size.rows);
    (void)unsetenv("COLUMNS");
    (void)unsetenv("LINES");

    (void)fclose(out);
}
