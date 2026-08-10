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
   with no message. */
#define RECIPE_SCHEMA_MAX 1

#define RECIPE_COORDINATE_MAX 128
#define RECIPE_MAX_SOURCES 32
#define RECIPE_SOURCE_MAX 128

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
 * What the consumer gets: the join with RFC-0007.
 *
 * `options` is the manifest's own option type rather than one of this file's,
 * so compile_flags_push_options puts a recipe's defines, includes and flags on
 * a compile line with the code that already exists — and a recipe and a
 * manifest cannot come to disagree about what `-I` means.
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
    recipe_artifact_type type; /* default: static (RFC-0009) */
    char sources[RECIPE_MAX_SOURCES][RECIPE_SOURCE_MAX];
    size_t source_count;
    char exclude[RECIPE_MAX_SOURCES][RECIPE_SOURCE_MAX];
    size_t exclude_count;
    char link[PROJECT_MAX_LINK][PROJECT_LINK_NAME_MAX];
    size_t link_count;
    project_options options; /* defines -> -D, include -> -I, flags verbatim */
} recipe_artifacts;

/* Read the top-level coordinate. Refuses a schema newer than this reader
   understands, a form that is neither binary nor source, and a missing name or
   version. An absent `schema` or `form` means what it could only have meant
   before those keys existed: schema 1, already built. */
[[nodiscard]] bool recipe_read_coordinate(doc_view doc, recipe_coordinate *out, char *err,
                                          size_t err_size);

/* Read `[artifacts]`. An absent table is not an error — a binary recipe
   describes itself with `[package]` instead — and yields the defaults. Every
   list overflows into an error rather than a truncation: a file dropped from
   `sources` is a link that fails much later and for no visible reason. */
[[nodiscard]] bool recipe_read_artifacts(doc_view doc, recipe_artifacts *out, char *err,
                                         size_t err_size);

/* True when `name` survives `sources`/`exclude`: it is listed (or `sources` is
   empty, meaning all of them) and not excluded. The one place that rule is
   spelled out, so the build and any report of it agree. */
[[nodiscard]] bool recipe_artifacts_wants(const recipe_artifacts *artifacts, const char *name);

#endif /* MOLTO_RECIPE_SERVICE_H */
