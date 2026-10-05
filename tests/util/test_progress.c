#include <moltest.h>

#include <molto/util/ansi.h>
#include <molto/util/progress.h>

#include <stdio.h>
#include <string.h>

/* The spinner, captured off a temporary file: what it writes, and what it is
   careful not to write when nobody is watching. */

static size_t captured(FILE *file, char *out, size_t out_size) {
    (void)fflush(file);
    rewind(file);
    const size_t read = fread(out, 1, out_size - 1, file);
    out[read] = '\0';
    return read;
}

MOLTEST(the_spinner_cycles_through_its_frames) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);

    char seen[SPINNER_FRAMES][64];
    for (size_t frame = 0; frame < SPINNER_FRAMES; frame++) {
        rewind(out);
        spinner_wait(out, "asking", frame);
        (void)captured(out, seen[frame], sizeof seen[frame]);
    }

    /* Four frames, all different, so a stalled one is visible as a stall. */
    for (size_t frame = 1; frame < SPINNER_FRAMES; frame++) {
        EXPECT_TRUE(strcmp(seen[frame], seen[frame - 1]) != 0);
    }

    /* The counter is a running total, not an index: a caller increments it
       forever and the spinner keeps turning. */
    rewind(out);
    spinner_wait(out, "asking", SPINNER_FRAMES * 1000);
    char wrapped[64] = "";
    (void)captured(out, wrapped, sizeof wrapped);
    EXPECT_STREQ(seen[0], wrapped);

    (void)fclose(out);
}

MOLTEST(the_spinner_names_the_work_and_claims_no_figure) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);

    spinner_wait(out, "asking the registry", 0);

    char text[128] = "";
    (void)captured(out, text, sizeof text);
    EXPECT_EQ('\r', text[0]);
    EXPECT_NOT_NULL(strstr(text, "asking the registry"));
    EXPECT_NULL(strchr(text, '\n'));
    /* No percentage and no count: the number of questions a conflict search
       will ask is not known until it stops asking. */
    EXPECT_NULL(strchr(text, '%'));

    (void)fclose(out);
}

MOLTEST(clearing_blanks_the_line_and_returns_to_it) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);

    progress_clear(out);

    char text[256] = "";
    const size_t size = captured(out, text, sizeof text);
    ASSERT_TRUE(size > 2);
    EXPECT_EQ('\r', text[0]);
    EXPECT_EQ('\r', text[size - 1]);
    EXPECT_EQ(' ', text[1]);

    (void)fclose(out);
}

/* A line nobody drew on must not be cleared: on a pipe the clear would be the
   only thing written, and it would land in whatever was being piped. */
MOLTEST(a_line_that_was_never_drawn_on_is_left_alone) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);

    progress_line line = {0};
    progress_line_clear(out, &line);

    char text[64] = "";
    EXPECT_EQ(0u, captured(out, text, sizeof text));

    line.drawn = true;
    progress_line_clear(out, &line);
    EXPECT_TRUE(captured(out, text, sizeof text) > 0);
    EXPECT_FALSE(line.drawn);

    (void)fclose(out);
}

/* The braille spinner, which unlike the one above hands back bytes instead of
   writing them: a region paints a frame in a single write, and a spinner that
   wrote itself could not be part of one. */

#define BRAILLE_LABEL "analyzing ..."
#define BRAILLE_LINE_SIZE SPINNER_BRAILLE_SIZE(sizeof BRAILLE_LABEL)

MOLTEST(the_braille_spinner_cycles_through_ten_frames) {
    char seen[SPINNER_BRAILLE_FRAMES][BRAILLE_LINE_SIZE];
    for (size_t frame = 0; frame < SPINNER_BRAILLE_FRAMES; frame++) {
        EXPECT_TRUE(spinner_braille_render(seen[frame], BRAILLE_LINE_SIZE, frame, BRAILLE_LABEL,
                                           false) > 0);
    }

    /* Ten frames, all different, so a stalled one is visible as a stall. */
    for (size_t frame = 0; frame < SPINNER_BRAILLE_FRAMES; frame++) {
        for (size_t other = frame + 1; other < SPINNER_BRAILLE_FRAMES; other++)
            EXPECT_TRUE(strcmp(seen[frame], seen[other]) != 0);
    }

    /* The counter is a running total, not an index: a drawer increments it for
       as long as the work lasts and the cell keeps turning. */
    char wrapped[BRAILLE_LINE_SIZE] = "";
    (void)spinner_braille_render(wrapped, sizeof wrapped, SPINNER_BRAILLE_FRAMES * 1000,
                                 BRAILLE_LABEL, false);
    EXPECT_STREQ(seen[0], wrapped);
}

MOLTEST(the_braille_line_names_the_work_and_ends_nothing) {
    char line[BRAILLE_LINE_SIZE] = "";
    const size_t size = spinner_braille_render(line, sizeof line, 0, BRAILLE_LABEL, false);

    ASSERT_TRUE(size > 0);
    EXPECT_EQ(size, strlen(line));
    EXPECT_NOT_NULL(strstr(line, BRAILLE_LABEL));
    /* One row of a region is one row: nothing here may end it, and nothing
       here may claim a figure the analysis never counted. */
    EXPECT_NULL(strchr(line, '\r'));
    EXPECT_NULL(strchr(line, '\n'));
    EXPECT_NULL(strchr(line, '%'));
    /* Without colour, not one escape — which is what a stream nobody is
       watching would be given if it were ever given anything. */
    EXPECT_NULL(strchr(line, '\033'));
}

/* The glyph is Molto talking; the label is the work's own name. Painting the
   label would claim it means something it does not. */
MOLTEST(colour_reaches_the_glyph_and_stops_before_the_label) {
    char line[BRAILLE_LINE_SIZE] = "";
    ASSERT_TRUE(spinner_braille_render(line, sizeof line, 0, BRAILLE_LABEL, true) > 0);

    EXPECT_EQ(0, strncmp(line, ANSI_MAGENTA, strlen(ANSI_MAGENTA)));
    /* The reset closes before the space, so the label starts in whatever the
       terminal was already using. */
    EXPECT_NOT_NULL(strstr(line, ANSI_RESET " " BRAILLE_LABEL));

    const char *label = strstr(line, BRAILLE_LABEL);
    ASSERT_NOT_NULL(label);
    EXPECT_NULL(strchr(label, '\033'));
}

/* A glyph is three bytes and one column, and an escape is bytes and no columns
   at all. A buffer sized against either count is refused rather than half
   filled: a line cut through a glyph would be drawn, and drawn wrong. */
MOLTEST(a_line_that_would_not_fit_whole_is_refused) {
    char line[sizeof BRAILLE_LABEL];

    EXPECT_EQ(0u, spinner_braille_render(line, sizeof line, 0, BRAILLE_LABEL, false));
    EXPECT_STREQ("", line);

    /* Room for the plain line is not room for the painted one. */
    char plain[BRAILLE_LINE_SIZE];
    const size_t bare = spinner_braille_render(plain, sizeof plain, 0, BRAILLE_LABEL, false);
    ASSERT_TRUE(bare > 0);
    char tight[BRAILLE_LINE_SIZE];
    EXPECT_EQ(0u, spinner_braille_render(tight, bare + 1, 0, BRAILLE_LABEL, true));
    EXPECT_STREQ("", tight);
}

/* The bar, which unlike the spinner does claim a figure — and may, because a
   build counts what it is going to compile before compiling any of it. */

#define BAR_CELLS 32

MOLTEST(the_bar_fills_in_proportion_to_the_work_done) {
    char bar[PROGRESS_BAR_SIZE(BAR_CELLS)];

    EXPECT_EQ(BAR_CELLS * 3u, progress_bar_render(bar, sizeof bar, 0, 4, BAR_CELLS));
    EXPECT_NULL(strstr(bar, "█"));

    (void)progress_bar_render(bar, sizeof bar, 2, 4, BAR_CELLS);
    EXPECT_EQ(0, strncmp(bar, "████████████████", 16 * 3));
    EXPECT_EQ(0, strcmp(bar + (16 * 3), "░░░░░░░░░░░░░░░░"));

    (void)progress_bar_render(bar, sizeof bar, 4, 4, BAR_CELLS);
    EXPECT_NULL(strstr(bar, "░"));
}

/* Floored on both counts: a bar that reads full while one unit is still
   compiling is a bar that lies, and so is a percentage that reads 100. */
MOLTEST(the_bar_reaches_the_end_only_when_the_work_does) {
    char bar[PROGRESS_BAR_SIZE(BAR_CELLS)];

    (void)progress_bar_render(bar, sizeof bar, 999, 1000, BAR_CELLS);
    EXPECT_NOT_NULL(strstr(bar, "░"));
    EXPECT_EQ(99u, progress_bar_percent(999, 1000));

    EXPECT_EQ(0u, progress_bar_percent(0, 4));
    EXPECT_EQ(50u, progress_bar_percent(2, 4));
    EXPECT_EQ(100u, progress_bar_percent(4, 4));
}

/* Nothing to compile is not nothing to say: the build is done, so the bar is
   full rather than undefined — and the arithmetic never divides by zero. */
MOLTEST(a_build_with_nothing_to_compile_is_a_full_bar) {
    char bar[PROGRESS_BAR_SIZE(BAR_CELLS)];

    (void)progress_bar_render(bar, sizeof bar, 0, 0, BAR_CELLS);
    EXPECT_NULL(strstr(bar, "░"));
    EXPECT_EQ(100u, progress_bar_percent(0, 0));

    /* More done than there was to do: clamped, not overflowed. */
    EXPECT_EQ(BAR_CELLS * 3u, progress_bar_render(bar, sizeof bar, 9, 4, BAR_CELLS));
    EXPECT_EQ(100u, progress_bar_percent(9, 4));
}

/* A glyph is three bytes and one column, so a buffer sized by the column count
   would be truncated a third of the way in. Refuse rather than half-draw. */
MOLTEST(a_buffer_sized_in_columns_is_refused) {
    char bar[BAR_CELLS + 1];

    EXPECT_EQ(0u, progress_bar_render(bar, sizeof bar, 4, 4, BAR_CELLS));
    EXPECT_STREQ("", bar);
}

MOLTEST(the_bar_carries_no_escapes_and_no_figure_of_its_own) {
    char bar[PROGRESS_BAR_SIZE(BAR_CELLS)];

    (void)progress_bar_render(bar, sizeof bar, 1, 3, BAR_CELLS);
    EXPECT_NULL(strchr(bar, '\033'));
    EXPECT_NULL(strchr(bar, '%'));
    EXPECT_NULL(strchr(bar, '\n'));
    EXPECT_NULL(strchr(bar, '\r'));
}

/* Erasing with an escape rather than a run of spaces: the run assumes the
   terminal is at least as wide as it is long, and a bar redrawn twenty times a
   second on a narrower one would scroll the display on every frame. */
MOLTEST(erasing_a_line_costs_one_escape_and_no_spaces) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);

    progress_erase_line(out);

    char text[64] = "";
    EXPECT_EQ(5u, captured(out, text, sizeof text));
    EXPECT_STREQ("\r\033[2K", text);

    (void)fclose(out);
}

MOLTEST(a_redirected_stream_is_not_interactive) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);

    EXPECT_FALSE(progress_is_interactive(out));

    (void)fclose(out);
}
