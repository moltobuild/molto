#include <molto/util/loader.h>

#include <molto/util/thread.h>

#include <molto/util/progress.h>
#include <molto/util/viewport.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* How long a frame is on the screen. Ten frames at this rate is a turn of the
   cell every eight tenths of a second: fast enough to read as motion, slow
   enough that a terminal over ssh is not redrawing a row a hundred times a
   second for a build it is not otherwise talking to. */
#define LOADER_INTERVAL_MS 80U

struct loader {
    FILE *out;
    viewport view;
    char label[LOADER_LABEL_MAX];
    bool colour;

    /* Only the drawer touches this, and only it draws. */
    size_t frame;

    atomic_bool drawing; /* cleared to ask the drawer to stop */
    bool running;        /* whether there is a drawer to join */
    thread drawer;
};

/* Compose the current frame and put it on the screen, one row.
 *
 * The terminal is measured every frame rather than once, for the reason
 * `viewport.h` gives: a window dragged narrower halfway through is ordinary,
 * and a row cut to the width of a moment ago wraps. */
static void draw(loader *load) {
    char line[SPINNER_BRAILLE_SIZE(LOADER_LABEL_MAX)];
    if(spinner_braille_render(line, sizeof line, load->frame, load->label, load->colour) == 0)
        return;
    const char *rows[1] = {line};
    viewport_paint(&load->view, rows, 1, viewport_measure(load->out));
    load->frame++;
}

/*
 * The drawer.
 *
 * It never decides it is finished, for the same reason the build's does not:
 * what it is drawing for is work it cannot see the end of. The only way out is
 * the flag `loader_stop` clears.
 */
static int drawer_run(void *arg) {
    loader *load = arg;
    while(atomic_load(&load->drawing)) {
        draw(load);
        thread_sleep_ms(LOADER_INTERVAL_MS);
    }
    return 0;
}

loader *loader_start(FILE *out, const char *label) {
    /* A terminal is what makes a spinner worth drawing; NO_COLOR is the person
       at that terminal saying they would rather read it plain. */
    if(out == NULL || label == NULL || !progress_is_interactive(out))
        return NULL;

    loader *load = calloc(1, sizeof *load);
    if(load == NULL)
        return NULL;
    load->out = out;
    load->colour = getenv("NO_COLOR") == NULL;
    snprintf(load->label, sizeof load->label, "%s", label);
    viewport_init(&load->view, out);
    atomic_init(&load->drawing, true);

    if(!thread_start(&load->drawer, drawer_run, load)) {
        viewport_free(&load->view);
        free(load);
        return NULL;
    }
    load->running = true;
    return load;
}

void loader_stop(loader *load) {
    if(load == NULL)
        return;
    if(load->running) {
        atomic_store(&load->drawing, false);
        thread_join(&load->drawer);
    }
    /* Clear after the drawer has been joined, never before: a frame painted
       between the clearing and the join would be left on the screen for good. */
    viewport_clear(&load->view);
    viewport_free(&load->view);
    free(load);
}
