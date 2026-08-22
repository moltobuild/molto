#ifndef MOLTO_PLUGIN_SERVICE_H
#define MOLTO_PLUGIN_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/services/recipe_service.h>
#include <molto/services/registry_service.h>
#include <molto/services/source_service.h>

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

/* How many plugins one listing reports. Overflow is reported, never silently
   dropped: a plugin missing from `molto plugin list` is one nobody knows is
   installed, and the whole point of the command is answering that question. */
#define PLUGIN_MAX_LISTED 64

/* Where a plugin came from, which `molto plugin list` reports because "what is
   installed" and "what happens to be on this PATH" are different answers. */
typedef enum {
    plugin_origin_installed, /* ~/.molto/plugins/bin, put there by molto */
    plugin_origin_path,      /* found on PATH, and nobody's record says why */
} plugin_origin;

/* One plugin, as a listing knows it before any recipe is read. */
typedef struct {
    char name[PLUGIN_NAME_MAX];
    char path[PLUGIN_PATH_MAX];
    plugin_origin origin;
    bool has_recipe;
} plugin_entry;

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

/* What installing `name` would mean, answered before anything is downloaded. */
typedef struct {
    recipe_coordinate coordinate;
    recipe_plugin plugin;
    char download_url[SOURCE_URL_MAX];
    char checksum[SOURCE_DIGEST_MAX];
    /* The registry's answer for this artifact, verbatim. Kept beside the
       binary so the permissions it was installed under stay readable. */
    char body[REGISTRY_BODY_MAX];
} plugin_candidate;

/* Ask `base_url` what installing `name` would mean. `version` NULL is the
   newest published.

   Separate from the install because the answer has to be shown to a person
   before it happens: the permissions are the thing being consented to, and a
   function that fetched and installed in one call would leave nowhere to ask
   (RFC-0014). Nothing is written and nothing is downloaded here. */
[[nodiscard]] bool plugin_prepare(const char *base_url, const char *name, const char *version,
                                  plugin_candidate *out, char *err, size_t err_size);

/* Download, verify and install a prepared candidate. */
[[nodiscard]] bool plugin_install(const plugin_candidate *candidate, char *err, size_t err_size);

/* Remove an installed plugin: its binary and the recipe beside it. False when
   nothing by that name was installed by molto — a plugin found on PATH is not
   molto's to delete. */
[[nodiscard]] bool plugin_remove(const char *name, char *err, size_t err_size);

/* Where the recipe of an installed plugin is kept:
   ~/.molto/plugins/recipes/<name>.toml.

   Beside the binary rather than inside it, and on disk rather than fetched
   again, because the permissions a plugin was installed under have to be
   readable later without asking a registry that may be unreachable, may have
   yanked the version, or may answer differently than it did that day. */
[[nodiscard]] bool plugin_recipe_path(const char *name, char *out, size_t size);

/* Every plugin this machine offers, installed first and then whatever PATH
   adds, each name reported once — a plugin installed *and* on PATH is the
   installed one, which is the copy that would run.

   Sorted by name within each origin, so two runs list the same thing in the
   same order however the filesystem felt about it. False when the listing did
   not fit `capacity`. */
[[nodiscard]] bool plugin_list(plugin_entry *out, size_t capacity, size_t *count);

/* Read the recipe of an installed plugin: its coordinate and its `[plugin]`
   table. False, with a message, when there is no recipe beside it or the one
   there cannot be read. Either `coordinate` or `plugin` may be NULL.

   Two encodings, one reader (`doc_view`). A `.toml` is what someone wrote by
   hand while developing a plugin; a `.json` is the registry's answer, stored
   verbatim by the install because the registry serves a parsed recipe and
   there is no original TOML to keep. The hand-written one wins, so a developer
   can override what was installed without uninstalling it. */
[[nodiscard]] bool plugin_read_recipe(const char *name, recipe_coordinate *coordinate,
                                      recipe_plugin *plugin, char *err, size_t err_size);

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
