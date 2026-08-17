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
