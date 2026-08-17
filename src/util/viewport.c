#include <molto/util/viewport.h>

#include <molto/util/ansi.h>
#include <molto/util/text.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Bytes the closing reset needs, terminator included, kept back from the end
   of the buffer so a line that fills it can still be closed. */
#define RESET_ROOM (sizeof ANSI_RESET)

/* The first byte of a CSI sequence's final byte range, and the last. A
   sequence ends at the first byte in it; everything before is parameters. */
#define CSI_FINAL_FIRST 0x40
#define CSI_FINAL_LAST 0x7E

/* --- measuring --- */

static size_t clamped(unsigned long value, size_t limit) {
    return value > limit ? limit : (size_t)value;
}

/* One dimension, as a string that may be anything at all. Zero is refused
   along with the rubbish: a terminal with no columns is not a terminal, and
   dividing the screen into thirds of nothing helps nobody. */
static size_t dimension(const char *value, size_t fallback, size_t limit) {
    if(value == NULL || *value == '\0')
        return fallback;
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(value, &end, 10);
    if(errno != 0 || end == value || *end != '\0' || parsed == 0)
        return fallback;
    return clamped(parsed, limit);
}

viewport_size viewport_size_from_env(const char *columns, const char *lines) {
    return (viewport_size){
        .columns = dimension(columns, VIEWPORT_FALLBACK_COLUMNS, VIEWPORT_COLUMNS_MAX),
        .rows = dimension(lines, VIEWPORT_FALLBACK_ROWS, VIEWPORT_ROWS_MAX),
    };
}

viewport_size viewport_measure(FILE *out) {
    if(out != NULL) {
        struct winsize window;
        if(ioctl(fileno(out), TIOCGWINSZ, &window) == 0 && window.ws_col > 0 && window.ws_row > 0)
            return (viewport_size){
                .columns = clamped(window.ws_col, VIEWPORT_COLUMNS_MAX),
                .rows = clamped(window.ws_row, VIEWPORT_ROWS_MAX),
            };
    }
    /* No terminal behind it, or a terminal that would not say. The two
       variables are what a caller has left to describe one. */
    return viewport_size_from_env(getenv("COLUMNS"), getenv("LINES"));
}

/* --- cutting a line --- */

/* Whether `byte` begins a UTF-8 character rather than continuing one. */
static bool is_lead(unsigned char byte) { return (byte & 0xC0) != 0x80; }

/* Bytes the character at `s` occupies: the lead and every continuation behind
   it. A stray continuation byte is one byte and one column, which is what
   text_columns makes of it too. */
static size_t character_bytes(const char *s) {
    size_t bytes = 1;
    while(s[bytes] != '\0' && !is_lead((unsigned char)s[bytes]))
        bytes++;
    return bytes;
}

/* Bytes the escape sequence at `s` occupies, or zero when what is there is not
   a complete one — a lone escape, an introducer this does not write, or a
   sequence the line ended in the middle of. */
static size_t escape_bytes(const char *s) {
    if(s[0] != '\033' || s[1] != '[')
        return 0;
    size_t bytes = 2;
    while((unsigned char)s[bytes] >= 0x20 && (unsigned char)s[bytes] < CSI_FINAL_FIRST)
        bytes++;
    const unsigned char final = (unsigned char)s[bytes];
    if(final < CSI_FINAL_FIRST || final > CSI_FINAL_LAST)
        return 0;
    return bytes + 1;
}

/* Whether a sequence turns colour off rather than on. `\033[0m` says so, and
   so does its parameterless spelling; everything else ending in `m` paints. */
static bool is_reset(const char *s, size_t bytes) {
    if(s[bytes - 1] != 'm')
        return false;
    for(size_t i = 2; i + 1 < bytes; i++)
        if(s[i] != '0' && s[i] != ';')
            return false;
    return true;
}

/* The whole line, when it holds no escapes and already fits. Most rows of a
   build are this, and the scan below would spell out a memcpy to reach the
   same answer. */
static bool copy_whole(const char *line, size_t columns, char *out, size_t out_size,
                       size_t *written) {
    if(strchr(line, '\033') != NULL || text_columns(line, SIZE_MAX) > columns)
        return false;
    const size_t bytes = strlen(line);
    if(bytes + 1 > out_size)
        return false;
    memcpy(out, line, bytes + 1);
    *written = bytes;
    return true;
}

size_t viewport_fit(const char *line, size_t columns, char *out, size_t out_size) {
    if(out == NULL || out_size == 0)
        return 0;
    out[0] = '\0';
    if(line == NULL || out_size < VIEWPORT_FIT_SIZE(columns))
        return 0;

    size_t written = 0;
    if(copy_whole(line, columns, out, out_size, &written))
        return written;

    /* Kept back so the closing reset always has somewhere to go. The buffer is
       sized for `columns` characters and the escapes a line of Molto's own
       carries, but the line is not necessarily one of Molto's own: a path with
       a thousand escapes in it must stop early rather than run off the end. */
    const size_t limit = out_size - RESET_ROOM;
    size_t taken = 0;
    bool painted = false;
    const char *s = line;

    /* Stopping the moment the budget is spent, rather than after: a sequence
       copied past the last column would open a colour with nothing left to
       paint, and the close below would shut it again on the same row. */
    while(*s != '\0' && taken < columns) {
        const size_t escape = escape_bytes(s);
        if(escape > 0) {
            if(written + escape > limit)
                break;
            memcpy(out + written, s, escape);
            written += escape;
            if(s[escape - 1] == 'm')
                painted = !is_reset(s, escape);
            s += escape;
            continue;
        }
        if(*s == '\033') {
            s++; /* a broken sequence is dropped, not passed on */
            continue;
        }
        const size_t bytes = character_bytes(s);
        if(written + bytes > limit)
            break;
        memcpy(out + written, s, bytes);
        written += bytes;
        taken++;
        s += bytes;
    }

    /* Cut in the middle of colour: close it, or it runs into the row below and
       out onto the prompt. Cut with no colour open: add nothing, because a
       stream that wanted none must receive none. */
    if(*s != '\0' && painted) {
        memcpy(out + written, ANSI_RESET, sizeof ANSI_RESET - 1);
        written += sizeof ANSI_RESET - 1;
    }
    out[written] = '\0';
    return written;
}

/* --- how tall the region may be --- */

size_t viewport_height(size_t in_flight, size_t rows, size_t maximum) {
    const size_t wanted = in_flight < maximum ? in_flight : maximum;
    const size_t third = rows / 3;
    return wanted < third ? wanted : third;
}

/* --- the region --- */

/* Erase the row the cursor is on, wherever on it the cursor is. */
#define ERASE_ROW "\033[2K"

/* Bytes a cursor movement can take: the escape, a row count no terminal will
   ever exceed, and the letter. */
#define MOVE_MAX 16

void viewport_init(viewport *view, FILE *out) {
    view->out = out;
    view->drawn = 0;
    view->frame = NULL;
    view->frame_size = 0;
}

void viewport_free(viewport *view) {
    if(view == NULL)
        return;
    free(view->frame);
    view->frame = NULL;
    view->frame_size = 0;
}

/* Room for the whole frame, worked out before any of it is written so the
   appends below cannot be the thing that runs off the end. False when the
   memory is not there, which draws no frame — a region is decoration, and a
   build that cannot spare this has worse news to give. */
static bool frame_room(viewport *view, size_t rows, size_t columns) {
    const size_t per_row = sizeof ERASE_ROW + VIEWPORT_FIT_SIZE(columns) + 2;
    /* One row over, for the last live row drawn a second time on the way back
       up, and two movements with a carriage return apiece. */
    const size_t needed = (rows + 1) * per_row + 2 * (MOVE_MAX + 1) + 1;
    if(view->frame_size >= needed)
        return true;
    char *grown = realloc(view->frame, needed);
    if(grown == NULL)
        return false;
    view->frame = grown;
    view->frame_size = needed;
    return true;
}

static void put(viewport *view, size_t *used, const char *text) {
    const size_t bytes = strlen(text);
    if(*used + bytes >= view->frame_size)
        return;
    memcpy(view->frame + *used, text, bytes);
    *used += bytes;
}

/* Up `rows` rows and back to the first column. Zero rows still needs the
   carriage return: the cursor is sitting at the end of whatever was drawn. */
static void put_up(viewport *view, size_t *used, size_t rows) {
    char move[MOVE_MAX];
    if(rows > 0) {
        (void)snprintf(move, sizeof move, "\033[%zuA", rows);
        put(view, used, move);
    }
    put(view, used, "\r");
}

static void put_fitted(viewport *view, size_t *used, const char *line, size_t columns) {
    if(*used + VIEWPORT_FIT_SIZE(columns) >= view->frame_size)
        return;
    *used += viewport_fit(line, columns, view->frame + *used, view->frame_size - *used);
}

static void write_frame(viewport *view, size_t used) {
    (void)fwrite(view->frame, 1, used, view->out);
    (void)fflush(view->out);
}

void viewport_paint(viewport *view, const char *const *lines, size_t count, viewport_size size) {
    if(view == NULL || view->out == NULL || lines == NULL || count == 0)
        return;
    const size_t columns = size.columns > 0 ? size.columns : VIEWPORT_FALLBACK_COLUMNS;
    /* Every row the last frame reached, whether this one still wants it or
       not: the ones it has given up are blanked in the same pass. */
    const size_t rows = count > view->drawn ? count : view->drawn;
    if(!frame_room(view, rows, columns))
        return;

    size_t used = 0;
    put_up(view, &used, view->drawn > 0 ? view->drawn - 1 : 0);
    for(size_t i = 0; i < rows; i++) {
        put(view, &used, ERASE_ROW);
        if(i < count)
            put_fitted(view, &used, lines[i], columns);
        /* The carriage return goes before the newline: a row drawn to the last
           column leaves the terminal with a wrap pending, and a bare newline
           would spend it. */
        if(i + 1 < rows)
            put(view, &used, "\r\n");
    }
    if(rows > count) {
        /* Back over the rows just given up, and the last live row once more,
           so the cursor finishes at the end of the region rather than under
           it — which is where the next frame counts from. */
        put_up(view, &used, rows - count);
        put(view, &used, ERASE_ROW);
        put_fitted(view, &used, lines[count - 1], columns);
    }

    write_frame(view, used);
    view->drawn = count;
}

void viewport_clear(viewport *view) {
    if(view == NULL || view->out == NULL || view->drawn == 0)
        return;
    const size_t rows = view->drawn;
    if(!frame_room(view, rows, 0))
        return;

    size_t used = 0;
    put_up(view, &used, rows - 1);
    for(size_t i = 0; i < rows; i++) {
        put(view, &used, ERASE_ROW);
        if(i + 1 < rows)
            put(view, &used, "\r\n");
    }
    put_up(view, &used, rows - 1);

    write_frame(view, used);
    view->drawn = 0;
}
