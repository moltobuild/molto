#include <moltest.h>

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

MOLTEST(a_redirected_stream_is_not_interactive) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);

    EXPECT_FALSE(progress_is_interactive(out));

    (void)fclose(out);
}
