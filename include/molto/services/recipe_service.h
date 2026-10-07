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
   here, and it is only honest because this reader does understand the table.

   Schema 3 is `form = "platform"` (RFC-0022), for the same reason: a reader
   that predates the form must refuse it rather than take a recipe with no
   [source] for a binary whose archive went missing.

   Schema 4 is `[artifacts.<os>]` and `[artifacts.private.<os>]`. A reader that
   predates them would build without the Windows-only `-lbcrypt` and fail at
   the link with a message naming nobody's mistake, so it must refuse.

   Schema 5 is `[build].sources` (RFC-0025). A reader that predates it would
   find no `[artifacts].sources` and compile every file in the tarball. */
#define RECIPE_SCHEMA_MAX 5

/* The schema that introduced `[plugin]`, and the least a recipe carrying that
   table may declare.
 *
 * Separate from the maximum above rather than spelled as it, because the two
 * answer different questions and will stop agreeing: the maximum rises with
 * every revision, and this stays at the one that added the table. `[plugin]`
 * at schema 3 is a plugin recipe; `[plugin]` at schema 1 is the hazard the
 * comment above describes, which is only a hazard because nothing refused it. */
#define RECIPE_SCHEMA_PLUGIN 2

/* The schema that introduced `form = "platform"`, and the least such a recipe
   may declare. */
#define RECIPE_SCHEMA_PLATFORM 3

/* The schema that introduced the per-OS tables, and the least a recipe
   carrying one may declare. */
#define RECIPE_SCHEMA_PER_OS 4

/* The schema that introduced `[build].sources`, and the least a recipe
   carrying it may declare. */
#define RECIPE_SCHEMA_BUILD_SOURCES 5

#define RECIPE_COORDINATE_MAX 128
/* An upstream tarball holds far more than its library, so a recipe on one
   names its sources rather than excluding the rest: libxml2 is 37 files, libpq
   and what it links from src/common and src/port 38. */
#define RECIPE_MAX_SOURCES 128
/* What one OS adds is a handful of port files, never a library. */
#define RECIPE_OS_MAX_SOURCES 32
#define RECIPE_SOURCE_MAX 128

/* Room for a language standard. The same size as the manifest's, because that
   is the field one ends up in. */
#define RECIPE_STD_MAX 16

typedef enum {
    recipe_form_binary,
    recipe_form_source,
    /* Files a platform's own repository publishes, pinned per platform and
       unpacked rather than built (RFC-0022). */
    recipe_form_platform,
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
/* The operating systems a per-OS table can name, keyed on the OS and not the
   triple, as RFC-0003's `[target.<os>]` is: what differs between x86_64 and
   aarch64 Windows is almost never a library. */
typedef enum {
    recipe_os_linux,
    recipe_os_macos,
    recipe_os_windows,
    RECIPE_OS_COUNT,
    recipe_os_none = RECIPE_OS_COUNT, /* an OS no table names: nothing is added */
} recipe_os;

/*
 * What `[artifacts.<os>]` and `[artifacts.private.<os>]` add on that OS.
 *
 * Lists only, and only appended (RFC-0003's merge rules): `-lbcrypt` on
 * Windows, a port file only on Windows, a define only on macOS. There is no
 * way to remove what `[artifacts]` said; an entry that does not apply to one
 * OS belongs in the tables of the ones it does apply to. No `[deps]`, because
 * a graph that differed by platform would give each platform its own lock.
 */
typedef struct {
    char sources[RECIPE_OS_MAX_SOURCES][RECIPE_SOURCE_MAX];
    size_t source_count;
    char exclude[RECIPE_OS_MAX_SOURCES][RECIPE_SOURCE_MAX];
    size_t exclude_count;
    char link[PROJECT_MAX_LINK][PROJECT_LINK_NAME_MAX];
    size_t link_count;
    project_options options;
    project_options private_options;
} recipe_os_artifacts;

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
    project_options options;                     /* defines -> -D, include -> -I, flags verbatim */
    project_options private_options;             /* the same three, and only for its own sources */
    recipe_os_artifacts per_os[RECIPE_OS_COUNT]; /* empty once recipe_artifacts_select_os ran */
    bool os_selected;
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
 * `via = "delegate"` with `system = "autotools"` is the one delegation molto
 * honours: it runs upstream's `configure` in the unpacked source, with `args`
 * and `env`, then `make` for each of `targets` (headers a Makefile rule writes,
 * like libpq's `src/port/pg_config_paths.h`), and compiles `[artifacts]`
 * itself. It never runs `make` to build the library. `jobs` is not read: there
 * is nothing parallel left to hand over.
 */
typedef enum {
    recipe_build_none,
    recipe_build_make,
    recipe_build_cmake,
    recipe_build_autotools,
    recipe_build_meson,
} recipe_build_system;

typedef enum {
    recipe_via_unset, /* the recipe did not say */
    recipe_via_frontend,
    recipe_via_delegate,
} recipe_build_via;

/* Where the list of what to compile comes from (RFC-0025): the recipe's own
   `[artifacts].sources`, or the compile lines of upstream's build once it is
   configured — `make -n`, or CMake's compile_commands.json. */
typedef enum {
    recipe_sources_recipe,
    recipe_sources_make,
    recipe_sources_cmake,
} recipe_build_sources;

#define RECIPE_BUILD_MAX_ARGS 64
#define RECIPE_BUILD_ARG_MAX 160
#define RECIPE_BUILD_MAX_ENV 16
#define RECIPE_BUILD_MAX_TARGETS 8
#define RECIPE_BUILD_MAX_GOALS 16

typedef struct {
    recipe_build_system system;
    recipe_build_via via;
    char args[RECIPE_BUILD_MAX_ARGS][RECIPE_BUILD_ARG_MAX];
    size_t arg_count;
    /* `NAME=value`, in the order the recipe wrote them. */
    char env[RECIPE_BUILD_MAX_ENV][RECIPE_BUILD_ARG_MAX];
    size_t env_count;
    char targets[RECIPE_BUILD_MAX_TARGETS][RECIPE_BUILD_ARG_MAX];
    size_t target_count;
    recipe_build_sources sources;
    /* What `make -n` is asked for; none is make's default goal. */
    char goals[RECIPE_BUILD_MAX_GOALS][RECIPE_BUILD_ARG_MAX];
    size_t goal_count;
} recipe_build;

/* True for the builds molto configures itself: autotools or cmake, delegated. */
[[nodiscard]] bool recipe_build_configures(const recipe_build *build);

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

/* The OS a build is for: the one a `--target` triple names, or this machine's
   when `platform` is NULL or empty. recipe_os_none for a triple naming none of
   the three, which then gets `[artifacts]` alone. */
[[nodiscard]] recipe_os recipe_os_for_platform(const char *platform);

/* Fold the table for `os` into `[artifacts]` and `[artifacts.private]`, and
   forget the others. Done once per package and per build, after which the
   rest of molto reads one flat table and never asks which OS it is for. A
   second call is a no-op. False, naming the key, when a list overflows. */
[[nodiscard]] bool recipe_artifacts_select_os(recipe_artifacts *artifacts, recipe_os os, char *err,
                                              size_t err_size);

#endif /* MOLTO_RECIPE_SERVICE_H */
