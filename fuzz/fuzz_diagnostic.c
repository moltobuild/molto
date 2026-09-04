#include "fuzz_input.h"

#include <molto/build/diagnostic.h>

#include <stdint.h>
#include <stdlib.h>

/*
 * What a compiler and a linker wrote, which is the least controlled input in
 * the whole build: it is produced by a program Molto did not write, in a format
 * nobody standardised, and it is parsed on the worker thread of every compile
 * and every lint pass. A vendor Molto has never seen, a locale that translates
 * the word "error", a source path with a colon in it — all of them arrive here
 * as bytes.
 *
 * Both grammars run over the same input. `diagnostic_parse` reads what a tool
 * that merely warned said; `diagnostic_parse_link` reads what a failing linker
 * said, which the first grammar cannot. Neither may reject the other's input by
 * crashing on it, and a build that streams a compiler's output through the
 * wrong one of the two is a mistake this catches rather than a corruption.
 *
 * `diagnostic_format` runs afterwards because a parsed diagnostic is not the
 * end of the journey: it is rendered against the project root, and the caret it
 * draws is positioned from a column the input supplied.
 */

static void render(const diagnostic_list *list) {
    for (size_t i = 0; i < diagnostic_list_count(list); i++) {
        const diagnostic *item = diagnostic_list_get(list, i);
        if (item == NULL)
            continue;
        char text[4096];
        (void)diagnostic_format(item, "/project/root", text, sizeof text);
        (void)diagnostic_severity_name(item->severity);
    }
    (void)diagnostic_count_severity(list, diagnostic_severity_error);
    (void)diagnostic_count_severity(list, diagnostic_severity_warning);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = fuzz_string(data, size);
    if (text == NULL)
        return 0;

    diagnostic_list compiled;
    diagnostic_list_init(&compiled);
    if (diagnostic_parse(text, &compiled)) {
        /* Both column conventions, because which one applies is decided by the
           vendor string and a caret is placed with it. */
        diagnostic_list_set_columns(&compiled, diagnostic_columns_byte);
        render(&compiled);
        diagnostic_list_set_columns(&compiled, diagnostic_columns_of_vendor("gcc"));
        render(&compiled);

        /* The round trip the result cache stores an entry through: what comes
           back out has to be readable by the same code that reads a fresh
           parse. */
        str_list values;
        str_list_init(&values);
        if (diagnostic_list_to_values(&compiled, &values)) {
            diagnostic_list replayed;
            diagnostic_list_init(&replayed);
            if (diagnostic_list_from_values(&values, &replayed))
                render(&replayed);
            diagnostic_list_free(&replayed);
        }
        str_list_free(&values);
    }
    diagnostic_list_free(&compiled);

    diagnostic_list linked;
    diagnostic_list_init(&linked);
    if (diagnostic_parse_link(text, &linked))
        render(&linked);
    diagnostic_list_free(&linked);

    free(text);
    return 0;
}
