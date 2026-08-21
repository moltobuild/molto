#ifndef MOLTO_PLUGIN_SERVICE_H
#define MOLTO_PLUGIN_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Plugins, as far as the command line is concerned (RFC-0014).
 *
 * A plugin is an executable named `molto-<name>`, and a name Molto's own table
 * does not carry is looked for among them. This is the `command` capability and
 * only that one: the plugin is spawned with the arguments as they were typed
 * and its output goes straight to the terminal. Nothing is exchanged, so
 * nothing here knows about the IR — that is RFC-0013's, and it arrives later.
 *
 * Two places are searched, in this order:
 *
 *   1. ~/.molto/plugins/bin, where `molto plugin install` puts things;
 *   2. PATH, so a plugin can be developed without installing it.
 *
 * The install directory is first so that what the user asked Molto to install
 * is what runs, rather than whatever a shell profile happened to put earlier on
 * PATH.
 */

#define PLUGIN_NAME_MAX 64
#define PLUGIN_PATH_MAX 4096

/* Whether `name` may name a plugin: lowercase letters, digits, `_` and `-`,
   starting with a letter or a digit. The same shape a recipe's name has
   (RFC-0009), and the reason to check it here is narrower — the name becomes a
   filename, so anything holding a separator or a `..` could reach out of the
   directory being searched. A rejected name is not looked for at all. */
[[nodiscard]] bool plugin_name_valid(const char *name);

/* Where installed plugins live: ~/.molto/plugins/bin. False when HOME is unset
   or the path does not fit. */
[[nodiscard]] bool plugin_dir(char *out, size_t size);

/* The executable serving `name`, written to `out` as an absolute path.
   False when the name is invalid or nothing provides it. */
[[nodiscard]] bool plugin_resolve(const char *name, char *out, size_t size);

/* Run the executable at `path`, handing it `argc`/`argv` unchanged and letting
   it inherit stdio, so what it prints is what the user sees.

   The plugin's own exit code is propagated verbatim — it is a subcommand, and
   a caller scripting `molto deb` needs its answer, not a translation. The one
   substitution is a plugin that never ran: a failed fork, or a file that could
   not be executed, reports exit_plugin_failure, because "the plugin broke" and
   "the plugin ran and said no" are different facts (RFC-0014).

   A plugin killed by a signal is *not* separated out today. process_run reports
   it as 128+N, which a plugin exiting with 137 of its own accord is
   indistinguishable from, and inventing a distinction the layer below does not
   make would be a guess. It becomes possible when the IR exchange gives
   process_service a reason to grow a richer result. */
[[nodiscard]] int plugin_run(const char *path, int argc, char **argv);

#endif /* MOLTO_PLUGIN_SERVICE_H */
