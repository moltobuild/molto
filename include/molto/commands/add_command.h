#ifndef MOLTO_ADD_COMMAND_H
#define MOLTO_ADD_COMMAND_H

#include <stdbool.h>

/*
 * `molto add <name>[@<version>]` and `molto remove <name>` (RFC-0002).
 *
 * Both write to `Project.toml`, which Molto touches only when asked. Neither
 * resolves anything: the next build does that, and reports what it finds
 * against the same rules a hand-edited manifest goes through. Doing the work
 * here as well would mean two places that can disagree about what a dependency
 * means.
 */

/*
 * Whether this invocation has to ask a registry anything.
 *
 * The only slow step `molto add` has, and so the only one worth announcing: a
 * name with nothing behind it is a question for the network, and every other
 * form of the command is a line rewritten in a file. Naming the condition is
 * what keeps the request and the spinner in step — one that turned for a
 * `--path` dependency would be animating a `snprintf`.
 */
[[nodiscard]] bool add_command_asks_registry(const char *version, const char *source);

/* `source` is a `git`/`path`/`archive` location, or NULL for a registry
   dependency named by version. `version` may be NULL: for a source carrying
   its own bytes there is nothing to ask, and for a registry dependency it
   means the newest release — asked for once, here, and written into the
   manifest as an exact number like any other. */
[[nodiscard]] int add_command_run(const char *name, const char *version, const char *source_key,
                                  const char *source, const char *registry, bool development);

[[nodiscard]] int remove_command_run(const char *name);

#endif /* MOLTO_ADD_COMMAND_H */
