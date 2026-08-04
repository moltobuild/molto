#ifndef MOLTO_LINT_SERVICE_H
#define MOLTO_LINT_SERVICE_H

#include <stdbool.h>

#include <molto/build/diagnostic.h>
#include <molto/build/profile.h>

typedef struct {
    build_profile profile;
    bool refresh_toolchain;
    bool refresh_tools;
} lint_request;

/*
 * Analyze the project rooted at `root` without producing any artifact.
 *
 * Every source under `src/` that linter.json does not exclude is put through
 * two passes: the compiler declared in [target], in a syntax-only pass, and
 * then whichever linter `pickup tools` reports, configured from linter.json.
 * The compiler pass costs nothing and needs nothing installed, which is why a
 * machine with no linter still gets a useful `molto lint` rather than an error.
 *
 * Nothing is printed here. What the diagnostics mean — whether they fail the
 * command, and in which shape they are shown — is the command's decision, which
 * is what lets --format json exist without a second copy of this.
 *
 * Returns a molto_exit_code describing the *analysis*, not its findings:
 *   0  the passes ran (errors in the code are in `out`, not in this value)
 *   1  a tool could not be run, or the workspace could not be read
 *   2  the manifest or linter.json is invalid, or asks for something that
 *      cannot be translated
 */
[[nodiscard]] int lint_project(const char *root, const lint_request *request, diagnostic_list *out);

#endif /* MOLTO_LINT_SERVICE_H */
