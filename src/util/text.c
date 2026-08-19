#include <molto/util/text.h>

#include <stdbool.h>
#include <string.h>

/* The bit pattern every byte of a UTF-8 character after the first one carries.
   Counting the bytes that do not have it counts characters, and does so
   without decoding anything or caring whether the text is valid: a stray byte
   in the middle of a source line is one column, which is what a terminal will
   make of it too. */
#define UTF8_CONTINUATION_MASK 0xC0
#define UTF8_CONTINUATION_MARK 0x80

static bool is_continuation(char byte) {
    return ((unsigned char)byte & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_MARK;
}

size_t text_columns(const char *s, size_t bytes) {
    if(s == NULL)
        return 0;
    size_t columns = 0;
    for(size_t i = 0; i < bytes && s[i] != '\0'; i++)
        columns += is_continuation(s[i]) ? 0 : 1;
    return columns;
}

/* How far a tab advances from `column`. Never zero: a tab sitting on a stop
   moves to the next one rather than staying put. */
static size_t tab_advance(size_t column, size_t tab_width) {
    return tab_width - (column % tab_width);
}

size_t text_column_of_byte(const char *s, size_t offset, size_t tab_width) {
    if(s == NULL || tab_width == 0)
        return 0;
    size_t column = 0;
    for(size_t i = 0; i < offset && s[i] != '\0'; i++) {
        if(s[i] == '\t')
            column += tab_advance(column, tab_width);
        else if(!is_continuation(s[i]))
            column++;
    }
    return column;
}

void text_expand_tabs(const char *s, size_t tab_width, char *out, size_t out_size) {
    if(out == NULL || out_size == 0)
        return;
    out[0] = '\0';
    if(s == NULL || tab_width == 0)
        return;

    size_t used = 0;
    size_t column = 0;
    for(const char *at = s; *at != '\0' && used + 1 < out_size; at++) {
        if(*at == '\t') {
            const size_t width = tab_advance(column, tab_width);
            for(size_t i = 0; i < width && used + 1 < out_size; i++)
                out[used++] = ' ';
            column += width;
            continue;
        }
        out[used++] = *at;
        if(!is_continuation(*at))
            column++;
    }
    out[used] = '\0';
}

/* --- eliding --- */

#define ELLIPSIS "\xe2\x80\xa6"
#define ELLIPSIS_BYTES (sizeof ELLIPSIS - 1)

/* The largest count not past `bytes` that does not land inside a character.
   Walks backwards over continuation bytes, which is the only direction that
   can shorten a prefix without inspecting what precedes it. */
static size_t whole_prefix(const char *s, size_t bytes) {
    while(bytes > 0 && is_continuation(s[bytes]))
        bytes--;
    return bytes;
}

/* The smallest offset not before `from` that starts a character. Walks
   forwards, so a suffix only ever gets shorter than the budget, never longer. */
static size_t whole_suffix(const char *s, size_t from, size_t len) {
    while(from < len && is_continuation(s[from]))
        from++;
    return from;
}

void text_elide_middle(const char *s, char *out, size_t out_size) {
    if(out_size == 0)
        return;
    out[0] = '\0';
    if(s == NULL)
        return;

    const size_t len = strlen(s);
    if(len < out_size) {
        memcpy(out, s, len + 1);
        return;
    }

    /* Everything below divides this: the bytes the answer may occupy. */
    const size_t room = out_size - 1;
    if(room < ELLIPSIS_BYTES + 2) {
        const size_t head = whole_prefix(s, room);
        memcpy(out, s, head);
        out[head] = '\0';
        return;
    }

    const size_t budget = room - ELLIPSIS_BYTES;
    const size_t head = whole_prefix(s, budget / 2);
    const size_t tail_from = whole_suffix(s, len - (budget - budget / 2), len);
    const size_t tail = len - tail_from;

    memcpy(out, s, head);
    memcpy(out + head, ELLIPSIS, ELLIPSIS_BYTES);
    memcpy(out + head + ELLIPSIS_BYTES, s + tail_from, tail);
    out[head + ELLIPSIS_BYTES + tail] = '\0';
}
