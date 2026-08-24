#ifndef MOLTO_FRONTEND_SERVICE_H
#define MOLTO_FRONTEND_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/services/ir_service.h>
#include <molto/services/plugin_service.h>
#include <molto/services/recipe_service.h>

/*
 * The `frontend` capability (RFC-0014): a project directory in, an IR document
 * out.
 *
 * A frontend is what makes `Project.toml` one way of describing a project
 * rather than the only one. Molto's own manifest has a frontend, written here;
 * a `meson.build` needs a plugin, and the two produce the same document.
 *
 * The contract, in the order a plugin meets it:
 *
 *   1. Molto looks at the directory and finds which installed plugins declare
 *      `frontend` and name a file that is actually there in `extensions`.
 *   2. It refuses, before spawning anything, a plugin whose `ir_schema` is not
 *      the one Molto speaks or whose `molto_min` is newer than Molto. A version
 *      mismatch found here is a refusal; found halfway through a document it is
 *      a half-read document.
 *   3. It runs `molto-<name> frontend`, writes the request below to its standard
 *      input and closes it, reads an IR document from its standard output, and
 *      leaves its standard error alone so what the plugin says reaches the user.
 *   4. Exit 0 means the document on stdout is the answer. Exit 3 means the
 *      plugin declines — the file is not one it understands — which is not an
 *      error and lets Molto try the next candidate. Any other exit is a
 *      failure. So is a document that does not parse, one whose `origin` is not
 *      the plugin's own name, one that reports no files read, and one that
 *      fails ir_validate.
 *   5. A plugin that exceeds FRONTEND_TIMEOUT_MS is killed and a document past
 *      FRONTEND_ANSWER_MAX is refused mid-read. A plugin that hangs must not
 *      hang a build.
 *
 * What a frontend cannot do, and none of it is an oversight: it does not run the
 * tool it understands, it gets no network, it writes nothing, it cannot spawn a
 * process without the `generator` capability, and it cannot emit a node type
 * this IR revision does not carry. `docs/Plugins.md` is the long version.
 */

/* --- the request --- */

/* What a frontend reads on its standard input.
 *
 * RFC-0014 describes a plugin as reading an IR document, which is right for
 * `transform` and `target` and cannot be right for a `frontend`: a frontend is
 * asked about a directory, and there is no document yet. So it gets this — the
 * smallest thing that answers "which directory, and which file made you the
 * candidate" — and it is a document with a `schema` of its own so that it can
 * grow the same way the IR does.
 *
 *   {"schema": 1, "request": "frontend", "root": "/w/app", "entry": "meson.build"}
 *
 * `root` is absolute, and every relative path in the answer is anchored at it.
 * `entry` is the filename from the plugin's own `extensions` that selected it,
 * so a plugin providing several does not have to go looking. */
#define FRONTEND_REQUEST_KIND "frontend"

/* --- limits --- */

/* How long a frontend gets, and how large a document it may return.
 *
 * Both are Molto's to choose rather than the plugin's: a limit a plugin could
 * raise is not a limit. Generous enough that a real Meson tree is nowhere near
 * either, and finite so that a plugin which hangs or spews is a failed build
 * rather than a wedged terminal. */
#define FRONTEND_TIMEOUT_MS 30000u
#define FRONTEND_ANSWER_MAX ((size_t)16 * 1024 * 1024)

/* How many frontends one directory may match. */
#define FRONTEND_MAX_CANDIDATES 8

/* --- what happened --- */

typedef enum {
    frontend_ok,
    /* Every candidate declined, or there were none. Not an error: a directory
       no frontend understands is a directory Molto has nothing to say about. */
    frontend_none,
    /* The native frontend read a Project.toml and refused it. Separate from
       `frontend_failed` because nothing third-party ran, and telling "my
       manifest is wrong" from "a third-party binary misbehaved" is what the
       enumerated exit codes of RFC-0002 exist for. */
    frontend_bad_manifest,
    /* A plugin ran and broke: a crash, a timeout, an unparseable document, a
       document that failed validation, or a permission it does not have. The
       caller reports exit_plugin_failure. */
    frontend_failed,
} frontend_result;

/* One frontend a directory selects. */
typedef struct {
    char name[PLUGIN_NAME_MAX];
    char path[PLUGIN_PATH_MAX];
    /* The filename from the plugin's `extensions` that is present at the root.
       Passed back to the plugin as `entry`. */
    char entry[RECIPE_PLUGIN_ENTRY_MAX];
    recipe_coordinate coordinate;
    recipe_plugin plugin;
} frontend_choice;

/* --- selection --- */

/* True when `plugin` declares the named capability. The one place that question
   is answered, so a capability check cannot be spelled two ways. */
[[nodiscard]] bool frontend_declares(const recipe_plugin *plugin, const char *capability);

/* Every installed plugin that declares `frontend`, names a file present at
   `root`, and speaks a schema this Molto can exchange with. Sorted by plugin
   name, so a directory two plugins claim is offered to them in the same order
   on every machine — an order that depends on readdir is a build that differs
   between machines.

   False only when the listing did not fit; no candidates is `*count == 0`. */
[[nodiscard]] bool frontend_candidates(const char *root, frontend_choice *out, size_t capacity,
                                       size_t *count);

/* Why a candidate was skipped, when it was. Written for the report rather than
   for control flow: a plugin silently passed over is a plugin whose author
   cannot tell it was installed wrong. */
[[nodiscard]] bool frontend_compatible(const frontend_choice *choice, char *err, size_t err_size);

/* --- asking one --- */

/* Run one frontend and read its answer into `out`.
 *
 * `frontend_none` means it declined (exit 3). The document is validated against
 * `bounds` before this returns, so a caller never holds one that has not been
 * through the lowering rules — the check is not optional and is not the
 * caller's to remember. */
[[nodiscard]] frontend_result frontend_ask(const frontend_choice *choice, const char *root,
                                           const ir_bounds *bounds, ir_document *out, char *err,
                                           size_t err_size);

/* The same, with the deadline named.
 *
 * The deadline is the caller's for the same reason a build's report is: what a
 * command allows belongs to the command, and a test proving that a plugin which
 * hangs is stopped should not have to wait the thirty seconds a build would.
 * It is not a way for a *plugin* to ask for longer — no plugin is on this side
 * of the call, and a limit a plugin could raise is not a limit. */
[[nodiscard]] frontend_result frontend_ask_with(const frontend_choice *choice, const char *root,
                                                const ir_bounds *bounds, unsigned timeout_ms,
                                                ir_document *out, char *err, size_t err_size);

/* --- the native frontend --- */

/* `Project.toml` and the source walk under `root`, as an IR document with
 * `origin` = "native".
 *
 * It exists because RFC-0013 says `Project.toml` is a frontend and not a
 * privileged path: whatever the manifest can express, a plugin can express,
 * because they are writing the same document. A representation its own author's
 * tools do not produce is a representation nobody has tested.
 *
 * `profile` names which profile's options are folded in, since the profile
 * decides which defines are in force and a `#ifdef` decides what compiles.
 *
 * It describes the project and not its dependencies. Folding a dependency's
 * interface into a target's scope is a *transform* — `merge_deps` in the build
 * service already argues that case in its own comment, written before
 * transforms existed — and this revision has no transforms, so the document
 * carries the project's own targets and an empty `dependencies`. Doing it here
 * would also make `molto ir` resolve a graph, which means the network, for a
 * command whose whole purpose is to show what is already known. */
[[nodiscard]] bool frontend_native(const char *root, const char *profile, ir_document *out,
                                   char *err, size_t err_size);

/* --- the whole selection --- */

/* The document for the project at `root`: the native frontend when there is a
   `Project.toml`, and otherwise the first plugin frontend that does not
   decline. Checked in that order and never the other way round — a plugin
   cannot take over a directory Molto already understands, the same rule the CLI
   applies to a command name. */
[[nodiscard]] frontend_result frontend_run(const char *root, const char *profile, ir_document *out,
                                           char *err, size_t err_size);

#endif /* MOLTO_FRONTEND_SERVICE_H */
