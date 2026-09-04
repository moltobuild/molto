#ifndef MOLTO_FUZZ_INPUT_H
#define MOLTO_FUZZ_INPUT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * A fuzzer is handed bytes and a length; every parser in this tree is handed a
 * NUL-terminated string. The copy is what bridges the two, and it is a copy on
 * purpose: terminating the fuzzer's own buffer in place is a write past the end
 * of it, and the first thing the sanitizer would report is this file rather
 * than the parser under test.
 *
 * NULL when the allocation failed, which a target should treat as nothing to
 * do — never as a finding.
 */
static inline char *fuzz_string(const uint8_t *data, size_t size) {
    char *text = malloc(size + 1);
    if (text == NULL)
        return NULL;
    memcpy(text, data, size);
    text[size] = '\0';
    return text;
}

#endif /* MOLTO_FUZZ_INPUT_H */
