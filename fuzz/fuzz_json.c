#include "fuzz_input.h"

#include <molto/util/json.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * JSON reaches Molto from two directions, and neither is under its control:
 * `format.json` and `linter.json` come out of a checkout, and the registry's
 * answers come off the network. The reader is also shared with pickup and is
 * kept diffable against it, so a fault found here is a fault found twice.
 *
 * The walk matters as much as the parse. Every accessor is written to tolerate
 * being handed something that is not there — a missing key, an index past the
 * end, a lookup into a value of the wrong kind — and that promise is only worth
 * what an input that tries all three says about it. So the tree is walked in
 * full and every value is asked for as each type, including the ones it is not.
 */

/* Depth is bounded by the parser (JSON_MAX_DEPTH); this bound is the walk's
   own, so a document that parses cannot recurse this function past it. */
static void walk(json_value value, size_t depth) {
    if (depth > JSON_MAX_DEPTH || !json_is_valid(value))
        return;

    /* Asked for as everything it might be, which is what a reader written
       against a server that changed its mind does. */
    long long number;
    bool flag;
    (void)json_type_of(value);
    (void)json_string(value);
    (void)json_number(value, &number);
    (void)json_bool(value, &flag);

    const size_t count = json_count(value);
    for (size_t i = 0; i < count; i++) {
        walk(json_at(value, i), depth + 1);

        const char *key = json_key_at(value, i);
        if (key != NULL)
            walk(json_get(value, key), depth + 1);
        walk(json_member_at(value, i), depth + 1);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = fuzz_string(data, size);
    if (text == NULL)
        return 0;

    json_document *doc = json_parse(text);
    if (doc != NULL) {
        walk(json_root(doc), 0);
        json_free(doc);
    }

    free(text);
    return 0;
}
