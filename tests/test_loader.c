#include <moltest.h>

#include <molto/util/loader.h>

#include <stdio.h>

/* The loader draws on a terminal and nowhere else. There is no terminal in a
   test, which makes the half worth pinning here the half that refuses: the
   drawing itself is `viewport_paint`, pinned byte for byte in test_viewport.c,
   and the frame it paints is pinned in test_progress.c. */

static size_t written_to(FILE *file) {
    (void)fflush(file);
    rewind(file);
    char seen[256];
    return fread(seen, 1, sizeof seen, file);
}

/* A spinner in a log file is noise and a spinner in a pipe is corruption of
   whatever was being piped — so a redirected stream gets no loader, and not
   even the escape that would clear a row nobody drew. */
MOLTEST(a_redirected_stream_gets_no_loader_and_no_bytes) {
    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);

    loader *load = loader_start(out, "analyzing ...");
    EXPECT_NULL(load);
    loader_stop(load);

    EXPECT_EQ(0u, written_to(out));

    (void)fclose(out);
}

/* Returning nothing is how the decision stays out of the caller: a command
   starts a loader and stops it without ever asking whether it got one. */
MOLTEST(stopping_a_loader_that_was_never_started_does_nothing) {
    loader_stop(NULL);
    loader_stop(loader_start(NULL, "analyzing ..."));
}
