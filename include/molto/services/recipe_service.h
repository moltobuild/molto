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

/* The schema that introduced `[plugin]`, and the least a recipe carrying that
   table may declare.
 *
 * Separate from the maximum above rather than spelled as it, because the two
 * answer different questions and will stop agreeing: the maximum rises with
 * every revision, and this stays at the one that added the table. `[plugin]`
 * at schema 3 is a plugin recipe; `[plugin]` at schema 1 is the hazard the
 * comment above describes, which is only a hazard because nothing refused it. */
#define RECIPE_SCHEMA_PLUGIN 2

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
 * `[[provide]]`: files the build needs that upstream ships under another name.
 *
 * The whole of what `configure` does for a default libpng build is copy
 * `scripts/pnglibconf.h.prebuilt` into `pnglibconf.h`; libjpeg ships
 * `jconfig.txt`, pcre2 `config.h.generic`, expat `expat_config.h.cmake`. Every
 * one of those is a source drop molto could otherwise consume, held back by one
 * file that upstream already wrote and only named differently.
 *
 * A list of tables and not a map, because a destination is a header and TOML
 * bare keys hold no dots. Both paths are relative to the root of the source and
 * are checked against it when they are applied — this reader only reads.
 *
 * It moves bytes the `[source]` digest already covered, reading none of them:
 * no diff, no substitution, nothing executed. A recipe that needs the file to
 * *differ* from what upstream shipped is asking to patch, which this cannot
 * express and RFC-0009 refuses.
 */
#define RECIPE_MAX_PROVIDE 8
#define RECIPE_PROVIDE_PATH_MAX 128

typedef struct {
    char file[RECIPE_PROVIDE_PATH_MAX];
    char from[RECIPE_PROVIDE_PATH_MAX];
} recipe_provision;

/* Tagged so a header lower in the include order can name it without reaching
   for this one: project_deps.h already includes source_service.h, and this file
   includes project_ctx.h, so the two cannot include each other. */
typedef struct recipe_provide {
    recipe_provision items[RECIPE_MAX_PROVIDE];
    size_t count;
} recipe_provide;

[[nodiscard]] bool recipe_read_provide(doc_view doc, recipe_provide *out, char *err,
                                       size_t err_size);

/*
 * `[build]`: the build system a source recipe's own sources need (RFC-0009).
 *
 * `none` is the one Molto can honour, and it is what a source drop that needs
 * no build says — headers, an amalgamation, or sources a consumer compiles as
 * if they were its own. The other four name a build system Molto does not run:
 * until RFC-0014's `via = "frontend"` can translate one, a recipe declaring
 * them is refused by whoever is about to build it rather than here, because the
 * message worth reading names the dependency and this reader does not know it.
 *
 * Read faithfully rather than collapsed to a boolean, so the day a frontend can
 * answer for `meson` this reader needs no change to say which one was asked
 * for.
 *
 * An absent table is `none`, which is the same shape `schema` and `form` above
 * already take and the same reason: the key is newer than the recipes, and one
 * that says nothing about a build system is one whose sources need none. It is
 * also what every source recipe published so far means, so reading them stays
 * correct.
 *
 * `via`, `args`, `env` and `jobs` are deliberately not here. Each of them only
 * means something for a system that is refused, so reading one today would be
 * code no build can reach — and a field nothing consumes is a field that drifts
 * from what it claims.
 */
typedef enum {
    recipe_build_none,
    recipe_build_make,
    recipe_build_cmake,
    recipe_build_autotools,
    recipe_build_meson,
} recipe_build_system;

typedef struct {
    recipe_build_system system;
} recipe_build;

[[nodiscard]] bool recipe_read_build(doc_view doc, recipe_build *out, char *err, size_t err_size);

/* What a recipe called it, for a message that has to name it back. */
[[nodiscard]] const char *recipe_build_system_name(recipe_build_system system);

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
