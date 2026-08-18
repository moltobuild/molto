#ifndef MOLTO_LINT_SERVICE_H
#define MOLTO_LINT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/build/diagnostic.h>
#include <molto/build/profile.h>

typedef struct {
    build_profile profile;
    bool refresh_toolchain;
    bool refresh_tools;
    /* How many files may be analysed at once; 0 takes the whole machine. The
       analysis is a compiler and a linter per file, so `-j` means here what it
       means to a build: a cap on what this command does to the machine. */
    size_t jobs;
    /* Re-analyse every file and rewrite its recorded result, for a tool that is
       not deterministic or depends on something the fingerprint cannot see
       (RFC-0006). Needing it routinely means the cache is wrong, not that the
       flag is useful. */
    bool refresh_analysis;
    /* Whether to say, while it happens, that the analysis is happening. The
       command decides it: `--format json` writes a document and nothing else,
       and a spinner is for a person rather than for a parser. Where it is drawn
       and whether anybody is watching are settled below it. */
    bool progress;
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
