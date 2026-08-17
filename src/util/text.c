#include <molto/util/text.h>

#include <stdbool.h>

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
