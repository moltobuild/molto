#include "fuzz_input.h"

#include <molto/build/depfile.h>
#include <molto/util/str_list.h>

#include <stdint.h>
#include <stdlib.h>

/*
 * A depfile is make syntax a compiler wrote, and it is what tells the cache
 * which headers a translation unit read (RFC-0006). Molto never sees it before
 * the compiler does: the target is a path Molto composed, but the prerequisites
 * are whatever the include search turned up, escaped by rules that differ
 * between gcc and clang and that a Windows drive letter has already been caught
 * breaking.
 *
 * What is at stake is not a crash alone. This list decides whether a file is
 * rebuilt, so a depfile that parses into the wrong prerequisites is a stale
 * object nobody notices — and the input that produces one comes from outside.
 */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = fuzz_string(data, size);
    if (text == NULL)
        return 0;

    str_list prereqs;
    str_list_init(&prereqs);
    if (depfile_parse(text, &prereqs)) {
        for (size_t i = 0; i < str_list_count(&prereqs); i++)
            (void)str_list_get(&prereqs, i);
    }
    str_list_free(&prereqs);

    free(text);
    return 0;
}
