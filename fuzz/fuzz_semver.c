#include "fuzz_input.h"

#include <molto/util/semver.h>
#include <molto/util/str_list.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A version string is the one thing the registry serves and never interprets
 * (RFC-0010), so `semver_parse` is where Molto decides what one means. Anything
 * it lets through is then ordered, and an ordering built on a version that was
 * not really one puts the wrong release on a machine.
 *
 * Which is why this target checks a property and not only for a crash. The
 * input is split into lines and each line is a candidate version, so one input
 * gives the comparison something to be antisymmetric about and gives
 * `semver_sort_desc` a list with the mix it sees in practice: some versions,
 * some strings that are not.
 */

/* `assert` would go away under NDEBUG and take the property with it, and a
   fuzz target that stops checking when someone changes a flag is worse than
   one that never checked. */
static void must(bool condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "fuzz_semver: %s\n", what);
        abort();
    }
}

/* `a` before `b` must mean `b` after `a`, and nothing may precede itself. An
   ordering that breaks either sorts differently depending on how the list
   arrived, which is a resolution that is not reproducible. */
static void check_ordering(const semver *a, const semver *b) {
    const int forward = semver_compare(a, b);
    const int backward = semver_compare(b, a);
    must((forward < 0) == (backward > 0) && (forward > 0) == (backward < 0),
         "comparison is not antisymmetric");
    must(semver_compare(a, a) == 0, "a version does not equal itself");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = fuzz_string(data, size);
    if (text == NULL)
        return 0;

    str_list lines;
    str_list_init(&lines);

    semver previous;
    bool have_previous = false;
    for (char *at = text, *end; at != NULL; at = end) {
        end = strchr(at, '\n');
        if (end != NULL)
            *end++ = '\0';
        if (!str_list_push(&lines, at))
            break;

        semver version;
        if (!semver_parse(at, &version))
            continue;
        if (have_previous)
            check_ordering(&previous, &version);
        previous = version;
        have_previous = true;
    }

    /* Both answers the resolver asks of a list it did not choose the contents
       of: which is highest, and what order to try them in. */
    if (str_list_count(&lines) > 0) {
        char highest[128];
        (void)semver_highest((const char *const *)lines.items, str_list_count(&lines), highest,
                             sizeof highest);
        (void)semver_sort_desc(&lines);
    }

    str_list_free(&lines);
    free(text);
    return 0;
}
