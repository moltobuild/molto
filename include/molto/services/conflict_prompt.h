#ifndef MOLTO_CONFLICT_PROMPT_H
#define MOLTO_CONFLICT_PROMPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <molto/services/dep_graph.h>

/*
 * Telling a person that two versions disagree, and asking what to do.
 *
 * A conflict between exact versions has no arithmetic answer (RFC-0008): both
 * dependents named a version, and one of them has to change. Molto can find a
 * change that works, but it cannot make it — accepting writes a new version
 * into `Project.toml`, where it lands in the diff and in review, which is the
 * whole point of exact versions.
 *
 * Rendering and asking are separate because only one of them needs a terminal.
 * The text is what can be got wrong, and it is tested without one.
 */

/* The message, ending after the proposal and before the question. */
void conflict_prompt_render(const dep_conflict *conflict, char *out, size_t out_size);

/* Ask on `out` and read the answer from `in`. An empty line accepts, which is
   what `[Y/n]` promises; end of input is a refusal. */
[[nodiscard]] bool conflict_prompt_ask(FILE *in, FILE *out);

/* Print the conflict on stderr and, when there is a proposal and stdin is a
   terminal, ask — writing the accepted version into `root`/Project.toml.
 *
 * True only when the manifest was edited, which is the caller's signal to
 * resolve again. Without a terminal it prints and answers false: a CI run that
 * silently accepted a version nobody chose would reintroduce exactly what the
 * exact-version rule exists to prevent, and `molto login` already set the
 * precedent of refusing to prompt into a pipe. */
[[nodiscard]] bool conflict_prompt_apply(const char *root, const dep_conflict *conflict);

#endif /* MOLTO_CONFLICT_PROMPT_H */
