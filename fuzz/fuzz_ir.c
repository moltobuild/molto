#include "fuzz_input.h"

#include <molto/services/ir_service.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * The build IR is what a frontend plugin answers with (RFC-0013, RFC-0014), and
 * a plugin is third-party code by definition: `molto build` runs a program the
 * user installed and reads a document it produced. That document then describes
 * the work — the sources to compile, the flags to compile them with, the paths
 * the build may read.
 *
 * So this is the one parser where a fault is not only a crash. `ir_validate` is
 * what stands between a document and Molto doing what it says: it is what
 * refuses a path outside the workspace and an option that would load code into
 * the compiler. A document that slips past it is a plugin choosing what runs on
 * the user's machine, which is why the validator is fuzzed here and not only
 * the reader.
 */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = fuzz_string(data, size);
    if (text == NULL)
        return 0;

    ir_document doc;
    ir_document_init(&doc);

    char err[1024];
    if (ir_read_json(text, &doc, err, sizeof err)) {
        /* Zero-initialised because `roots` is read through, and a caller that
           assigns field by field leaves whatever the stack held in the ones it
           did not name. A frontend's document is validated against no roots at
           all, which is the narrowest the bounds ever are and therefore the
           case worth checking. */
        ir_bounds bounds;
        memset(&bounds, 0, sizeof bounds);
        bounds.workspace = "/project/root";
        bounds.build_dir = "/project/root/build";
        (void)ir_validate(&doc, &bounds, err, sizeof err);
    }
    ir_document_free(&doc);

    free(text);
    return 0;
}
