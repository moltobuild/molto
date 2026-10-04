#ifndef MOLTO_ADD_COMMAND_H
#define MOLTO_ADD_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

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

/* `molto add git+<url>[#<ref>]`: the spelling Molto.lock already uses for a
   git source, accepted on the command line. */
#define ADD_GIT_PREFIX "git+"

/*
 * Split `git+<url>[#<ref>]` into the URL, the reference (empty when absent)
 * and the package name, which is the repository's last path segment without
 * `.git`. False, with a reason in `err`, when it is not that shape, carries a
 * character a manifest string cannot, or names something that is not a
 * package name. Touches nothing outside its arguments.
 */
[[nodiscard]] bool add_git_spec_parse(const char *spec, char *name, size_t name_size, char *url,
                                      size_t url_size, char *reference, size_t reference_size,
                                      char *err, size_t err_size);

/* Add the dependency `spec` names. Without `#<ref>` the repository's default
   branch is asked for and written as `branch`; a ref is written as `rev`,
   `tag` or `branch`, whichever the repository says it is. */
[[nodiscard]] int add_git_command_run(const char *spec, bool development);

[[nodiscard]] int remove_command_run(const char *name);

#endif /* MOLTO_ADD_COMMAND_H */
