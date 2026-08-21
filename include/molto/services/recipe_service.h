#ifndef MOLTO_RECIPE_SERVICE_H
#define MOLTO_RECIPE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/project_ctx.h>
#include <molto/util/doc.h>

/*
 * A recipe's coordinate and its `[artifacts]` table (RFC-0009), read through
 * doc_view so the same code reads a recipe.toml on disk and the parsed recipe
 * a registry serves inside an artifact's `metadata`.
 *
 * `[source]` is deliberately not here: source_service owns it, and owning it in
 * one place is the point of having a source_spec at all.
 */

/* The highest recipe format this reader understands. A recipe declaring more
   is refused rather than interpreted: RFC-0009 allows a later schema to give an
   existing key a new meaning, so reading one optimistically is reading it
   wrong. "Upgrade molto" is a fixable error; a misread recipe is a broken build
   with no message.

   Schema 2 is `[plugin]` (RFC-0014). A plugin's recipe is required to declare
   it, so that a molto predating plugins refuses the document rather than
   ignoring a table it does not know and installing an executable whose
   permissions it never saw. Raising this is what makes such a recipe readable
   here, and it is only honest because this reader does understand the table. */
#define RECIPE_SCHEMA_MAX 2

#define RECIPE_COORDINATE_MAX 128
#define RECIPE_MAX_SOURCES 32
#define RECIPE_SOURCE_MAX 128

/* Room for a language standard. The same size as the manifest's, because that
   is the field one ends up in. */
#define RECIPE_STD_MAX 16

typedef enum {
    recipe_form_binary,
    recipe_form_source,
} recipe_form;

typedef struct {
    long schema;
    recipe_form form;
    char kind[RECIPE_COORDINATE_MAX];
    char name[RECIPE_COORDINATE_MAX];
    char version[RECIPE_COORDINATE_MAX];
    char target[RECIPE_COORDINATE_MAX];
} recipe_coordinate;

typedef enum {
    recipe_artifact_source, /* the consumer compiles these sources as its own */
    recipe_artifact_static,
    recipe_artifact_shared,
} recipe_artifact_type;

/*
 * What the consumer gets and what the package keeps: the join with RFC-0007.
 *
 * `options` is the interface. Its defines are ABI, its include directories are
 * where the headers are, and both reach the command line of everything that
 * depends on this package — because a define that changes a struct inside a
 * header is not a preference, and a consumer compiled without it is compiled
 * against a different type.
 *
 * `private_options` is the same three lists applied only while this package's
 * own sources compile. It is where `-fno-strict-aliasing`, a `-Wno-…` and an
 * internal `-I` belong: what a library needs in order to build, and what no
 * caller should have to adopt to use it. Without the split, every one of those
 * is everybody's.
 *
 * `link` has no private counterpart. A `-l` is a library the final binary is
 * linked against, and there is no line it could be private to. Neither has
 * `std`, and for the opposite reason: the standard a package's sources are
 * compiled with never leaves them, so there is nothing for a private version
 * to distinguish itself from. Empty means the consumer's, which is what a
 * package that never had an opinion says by saying nothing.
 *
 * Both are the manifest's own option type rather than one of this file's, so
 * compile_flags_push_options puts either on a compile line with the code that
 * already exists — and a recipe and a manifest cannot come to disagree about
 * what `-I` means.
 *
 * `sources` and `exclude` are both here because a source drop is not a library:
 * an upstream archive holds what upstream ships, which is usually more than the
 * library. SQLite's amalgamation carries shell.c and its own main(), so a
 * consumer that compiles the whole drop links two of them. `sources` fails
 * closed (a file added upstream tomorrow does not join the build by itself);
 * `exclude` is the shorter statement when the drop is almost all library, and
 * is applied after `sources` so a recipe can narrow a list rather than restate
 * it.
 */
typedef struct {
    recipe_artifact_type type;    /* default: static (RFC-0009) */
    char std[RECIPE_STD_MAX];     /* C standard for its own sources; "" = the consumer's */
    char cpp_std[RECIPE_STD_MAX]; /* the same for C++, decided separately */
    char sources[RECIPE_MAX_SOURCES][RECIPE_SOURCE_MAX];
    size_t source_count;
    char exclude[RECIPE_MAX_SOURCES][RECIPE_SOURCE_MAX];
    size_t exclude_count;
    char link[PROJECT_MAX_LINK][PROJECT_LINK_NAME_MAX];
    size_t link_count;
    project_options options;         /* defines -> -D, include -> -I, flags verbatim */
    project_options private_options; /* the same three, and only for its own sources */
} recipe_artifacts;

/* Read the top-level coordinate. Refuses a schema newer than this reader
   understands, a form that is neither binary nor source, and a missing name or
   version. An absent `schema` or `form` means what it could only have meant
   before those keys existed: schema 1, already built. */
[[nodiscard]] bool recipe_read_coordinate(doc_view doc, recipe_coordinate *out, char *err,
                                          size_t err_size);

/* Read `[artifacts]` and `[artifacts.private]`. An absent table is not an error
   — a binary recipe describes itself with `[package]` instead — and yields the
   defaults; declaring only the private one is enough to be read. Every list
   overflows into an error rather than a truncation: a file dropped from
   `sources` is a link that fails much later and for no visible reason. */
[[nodiscard]] bool recipe_read_artifacts(doc_view doc, recipe_artifacts *out, char *err,
                                         size_t err_size);

/*
 * `[plugin]`, on a recipe whose `[tool].kind` is `plugin` (RFC-0014).
 *
 * What a plugin declares before anything of it is downloaded: the capabilities
 * it provides, the file extensions that select it as a frontend, the
 * permissions it asks for, the IR schema it speaks and the oldest Molto it
 * works with.
 *
 * The lists are read exactly as written and are not checked against the
 * vocabularies RFC-0014 defines. A reader that dropped a permission it did not
 * recognise would report a plugin as asking for less than it does, which is the
 * one place in this format where ignoring the unknown is dangerous rather than
 * merely forgiving. Deciding what an unfamiliar name means belongs to whoever
 * is about to act on it; reporting it belongs here.
 */
#define RECIPE_PLUGIN_ENTRY_MAX 48
#define RECIPE_PLUGIN_MAX_CAPABILITIES 8
#define RECIPE_PLUGIN_MAX_EXTENSIONS 16
#define RECIPE_PLUGIN_MAX_PERMISSIONS 16

typedef struct {
    char capabilities[RECIPE_PLUGIN_MAX_CAPABILITIES][RECIPE_PLUGIN_ENTRY_MAX];
    size_t capability_count;
    char extensions[RECIPE_PLUGIN_MAX_EXTENSIONS][RECIPE_PLUGIN_ENTRY_MAX];
    size_t extension_count;
    char permissions[RECIPE_PLUGIN_MAX_PERMISSIONS][RECIPE_PLUGIN_ENTRY_MAX];
    size_t permission_count;
    long ir_schema;                        /* 0 when the recipe names none */
    char molto_min[RECIPE_COORDINATE_MAX]; /* "" when the recipe names none */
} recipe_plugin;

/* Read `[plugin]`. A recipe without the table is not a plugin: false with an
   error, rather than an empty declaration that would read as "asks for
   nothing". `capabilities` is required and must not be empty, because a plugin
   that provides no capability is a binary Molto has no reason to run. */
[[nodiscard]] bool recipe_read_plugin(doc_view doc, recipe_plugin *out, char *err, size_t err_size);

/* True when `name` survives `sources`/`exclude`: it is listed (or `sources` is
   empty, meaning all of them) and not excluded. The one place that rule is
   spelled out, so the build and any report of it agree. */
[[nodiscard]] bool recipe_artifacts_wants(const recipe_artifacts *artifacts, const char *name);

#endif /* MOLTO_RECIPE_SERVICE_H */
