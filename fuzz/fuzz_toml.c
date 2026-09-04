#include "fuzz_input.h"

#include <molto/util/str_list.h>
#include <molto/util/toml.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * TOML is the format Molto reads text it did not write: a `Project.toml` from a
 * dependency's tree, a `recipe.toml` the registry served, a `Molto.lock` that
 * was edited by hand, and pickup's answer to `tools --format toml`. Every one
 * of those arrives before anything has decided it is well-formed, which makes
 * this parser the widest untrusted surface in the tree.
 *
 * Parsing is only half of what is exercised here. A document that parses can
 * still be walked wrong, and the walk is where a length, an index or a section
 * name composed from the input goes — so the accessors run over whatever comes
 * back, including the two that enumerate (`toml_section_keys`,
 * `toml_section_members`) and the table-array spelling, which builds a section
 * name from a name the document supplied.
 */

/* Read one section every way the API allows. */
static void walk_section(const toml_document *doc, const char *section) {
    char text[256];
    long number;
    bool flag;
    (void)toml_get_string(doc, section, "name", text, sizeof text);
    (void)toml_get_int(doc, section, "jobs", &number);
    (void)toml_get_bool(doc, section, "debug_info", &flag);
    (void)toml_has_section(doc, section);

    str_list keys;
    str_list_init(&keys);
    if (toml_section_keys(doc, section, &keys)) {
        for (size_t i = 0; i < str_list_count(&keys); i++) {
            str_list values;
            str_list_init(&values);
            (void)toml_get_array(doc, section, str_list_get(&keys, i), &values);
            str_list_free(&values);
        }
    }
    str_list_free(&keys);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = fuzz_string(data, size);
    if (text == NULL)
        return 0;

    char err[512];
    toml_document *doc = toml_parse(text, err, sizeof err);
    if (doc != NULL) {
        /* The root first, whose members name the tables to descend into. Two
           levels is enough to reach a manifest's deepest real section
           (`deps.<name>.opts`) without turning one input into a walk of
           everything the document could nest. */
        str_list top;
        str_list_init(&top);
        walk_section(doc, "");
        if (toml_section_members(doc, "", &top)) {
            for (size_t i = 0; i < str_list_count(&top); i++) {
                const char *name = str_list_get(&top, i);
                walk_section(doc, name);

                str_list nested;
                str_list_init(&nested);
                if (toml_section_members(doc, name, &nested)) {
                    for (size_t j = 0; j < str_list_count(&nested); j++) {
                        char section[TOML_SECTION_MAX];
                        if (snprintf(section, sizeof section, "%s.%s", name,
                                     str_list_get(&nested, j))
                            < (int)sizeof section)
                            walk_section(doc, section);
                    }
                }
                str_list_free(&nested);

                /* A [[name]] element is stored under a section this composes,
                   and the count comes from the document. */
                const size_t count = toml_table_array_count(doc, name);
                for (size_t j = 0; j < count; j++) {
                    char section[TOML_SECTION_MAX];
                    if (toml_table_array_section(name, j, section, sizeof section))
                        walk_section(doc, section);
                }
            }
        }
        str_list_free(&top);
        toml_free(doc);
    }

    free(text);
    return 0;
}
