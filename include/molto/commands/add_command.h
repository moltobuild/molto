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

/* `source` is a `git`/`path`/`archive` location, or NULL for a registry
   dependency named by version. `version` may be NULL only for a source that
   carries its own bytes — a registry dependency without one would have to ask
   what the newest release is, which is `molto update`'s job and needs a
   version comparator that does not exist yet. */
[[nodiscard]] int add_command_run(const char *name, const char *version, const char *source_key,
                                  const char *source, const char *registry, bool development);

[[nodiscard]] int remove_command_run(const char *name);

#endif /* MOLTO_ADD_COMMAND_H */
