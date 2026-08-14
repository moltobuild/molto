#include <molto/util/progress.h>

#include <unistd.h>

/* Wide enough to cover any label this draws, so clearing takes one write. */
#define PROGRESS_LINE_WIDTH 100

static const char *const spinner_frames[SPINNER_FRAMES] = {"-", "\\", "|", "/"};

bool progress_is_interactive(FILE *out) { return isatty(fileno(out)) == 1; }

void spinner_wait(FILE *out, const char *label, size_t frame) {
    /* A carriage return and no newline: every frame overwrites the last, so
       the whole spinner occupies one line for its whole life. */
    fprintf(out, "\r%s %s   ", label, spinner_frames[frame % SPINNER_FRAMES]);
    (void)fflush(out);
}

void progress_clear(FILE *out) {
    fputc('\r', out);
    for(int i = 0; i < PROGRESS_LINE_WIDTH; i++)
        fputc(' ', out);
    fputc('\r', out);
    (void)fflush(out);
}

void progress_line_clear(FILE *out, progress_line *line) {
    if(!line->drawn)
        return;
    progress_clear(out);
    line->drawn = false;
}
