#include <molto/util/progress.h>

#include <string.h>
#include <unistd.h>

/* Wide enough to cover any label this draws, so clearing takes one write. */
#define PROGRESS_LINE_WIDTH 100

/* One column of the bar, done and not done. Three bytes each, one column each:
   everything here counts columns and sizes buffers in bytes. */
#define BAR_CELL_DONE "█"    /* full block */
#define BAR_CELL_PENDING "░" /* light shade */
#define BAR_CELL_BYTES 3

/* What a whole percentage is out of. */
#define PERCENT_WHOLE 100

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

/* How many of `cells` columns `done` out of `total` has earned. Floored, so a
   full bar says every unit is accounted for and nothing weaker. */
static size_t cells_done(size_t done, size_t total, size_t cells) {
    if(total == 0 || done >= total)
        return cells;
    return (done * cells) / total;
}

size_t progress_bar_render(char *out, size_t size, size_t done, size_t total, size_t cells) {
    if(out == NULL || size == 0)
        return 0;
    out[0] = '\0';
    if(size < PROGRESS_BAR_SIZE(cells))
        return 0;
    const size_t filled = cells_done(done, total, cells);
    size_t written = 0;
    for(size_t cell = 0; cell < cells; cell++) {
        memcpy(out + written, cell < filled ? BAR_CELL_DONE : BAR_CELL_PENDING, BAR_CELL_BYTES);
        written += BAR_CELL_BYTES;
    }
    out[written] = '\0';
    return written;
}

size_t progress_bar_percent(size_t done, size_t total) {
    if(total == 0 || done >= total)
        return PERCENT_WHOLE;
    return (done * PERCENT_WHOLE) / total;
}

void progress_erase_line(FILE *out) {
    fputs("\r\033[2K", out);
    (void)fflush(out);
}
