#ifndef MOLTO_PROJECT_DEPS_H
#define MOLTO_PROJECT_DEPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <molto/services/source_service.h>
#include <molto/util/doc.h>
#include <molto/util/toml.h>

/*
 * The `[deps]` and `[registries]` tables of a manifest (RFC-0003), as a typed
 * model.
 *
 * Two spellings, one meaning: `sqlite = "3.53.4"`,
 * `sqlite = { version = "3.53.4" }` and a `[deps.sqlite]` header all produce
 * the same entry.
 *
 * The distinction that matters downstream is not which key was written but
 * whether the manifest said *where* the dependency is. A `version` dependency
 * carries a coordinate and a registry has to answer before there is anything
 * to fetch. A `git`, `path` or `archive` dependency already names its source
 * and goes straight to the fetcher — but it still owes an `[artifacts]` table,
 * which it can only supply by carrying a recipe.toml at the root of whatever
 * is fetched, since `[deps]` has nowhere to put one.
 */

/* Field sizes are the manifest's, not the fetcher's. The TOML parser refuses
   any string value past 255 bytes, so nothing longer can reach here — and
   project_ctx is passed around by value, so a dependency that reserved the
   fetcher's own generous buffers would multiply by PROJECT_MAX_DEPS into tens
   of kilobytes of manifest. */
#define PROJECT_MAX_DEPS 32
#define DEP_NAME_MAX 64
#define DEP_VERSION_MAX 64
#define DEP_REGISTRY_MAX 32
#define DEP_LOCATION_MAX 256
#define DEP_REFERENCE_MAX 128
#define DEP_DIGEST_MAX 72
#define DEP_PREFIX_MAX 128

#define PROJECT_MAX_REGISTRIES 8
#define REGISTRY_NAME_MAX 32
#define REGISTRY_URL_MAX 256

typedef enum {
    dep_source_version, /* the plain-string shorthand, and `version = "…"` */
    dep_source_git,
    dep_source_path,
    dep_source_archive,
} dep_source;

typedef enum {
    dep_resolution_registry, /* a coordinate: ask a registry (resolve_service) */
    dep_resolution_carried,  /* the manifest names the bytes: fetch them directly */
} dep_resolution;

/* Which of branch/tag/rev `spec.reference` holds.

   A tag and a rev name a commit; a branch names whatever it points at today,
   and RFC-0008 has `molto update` re-resolve one and not the others. Which was
   written is not recoverable from the reference itself, so it is kept. It lives
   here rather than in source_spec because RFC-0009's `[source]` has `tag` and
   `rev` only, and putting `branch` there would hand recipes a capability that
   RFC does not give them. */
typedef enum {
    dep_git_ref_none,
    dep_git_ref_branch,
    dep_git_ref_tag,
    dep_git_ref_rev,
} dep_git_ref;

typedef struct {
    char name[DEP_NAME_MAX];
    dep_source source;
    dep_resolution resolution;

    /* The coordinate, when resolution is dep_resolution_registry. Always one
       exact version: manifest_is_exact_version has already refused a range. */
    char version[DEP_VERSION_MAX];
    char registry[DEP_REGISTRY_MAX]; /* "" means the default registry */

    /* Where the bytes are, when resolution is dep_resolution_carried. Spelled
       as the manifest spells it; project_dep_to_source turns it into what the
       fetcher takes, in one place, so nothing is translated twice. */
    char location[DEP_LOCATION_MAX]; /* the git URL, archive URL, or directory */
    char reference[DEP_REFERENCE_MAX];
    char sha256[DEP_DIGEST_MAX];
    char strip_prefix[DEP_PREFIX_MAX];
    dep_git_ref git_ref;
} project_dep;

typedef struct {
    project_dep items[PROJECT_MAX_DEPS];
    size_t count;
} project_deps;

/* The `[registries]` table: a name a dependency can select with
   `registry = "<name>"`, and the base URL it stands for. */
typedef struct {
    char names[PROJECT_MAX_REGISTRIES][REGISTRY_NAME_MAX];
    char urls[PROJECT_MAX_REGISTRIES][REGISTRY_URL_MAX];
    size_t count;
} project_registries;

/* Read the `[deps]` table of a parsed manifest into `out`, in declaration
   order. An absent table is not an error and yields a count of zero.

   Every refusal names the dependency: more than PROJECT_MAX_DEPS entries, no
   source key, two source keys, a version that is a range, a git reference
   without a git URL, two git references, an archive without a digest, a key
   that is not part of the format, and a key that is part of it but not
   supported yet. */
[[nodiscard]] bool project_deps_read(const toml_document *doc, project_deps *out, char *err,
                                     size_t err_size);

/* The same, through doc_view, so a recipe's `[deps]` is read by exactly the
   code that reads a manifest's — whether that recipe arrived as TOML on disk or
   as the JSON a registry serves inside an artifact's metadata.

   A transitive dependency is discovered this way, and it has to be the same
   reader: a graph built from two parsers is a graph whose edges disagree about
   what a dependency is. */
[[nodiscard]] bool project_deps_read_doc(doc_view doc, project_deps *out, char *err,
                                         size_t err_size);

/* Read the `[registries]` table. An absent table is not an error. */
[[nodiscard]] bool project_registries_read(const toml_document *doc, project_registries *out,
                                           char *err, size_t err_size);

/* Refuse a dependency naming a registry that `registries` does not declare.
   Separate from reading because it needs both tables, and a manifest may
   declare them in either order. */
[[nodiscard]] bool project_deps_check_registries(const project_deps *deps,
                                                 const project_registries *registries, char *err,
                                                 size_t err_size);

/* Fill the fetcher's input struct from a dependency that carries its own
   source. False, with a reason in `err`, for a `version` dependency — that one
   has a coordinate and not a source, and a registry has to answer first.

   The one place a manifest's spelling becomes the fetcher's, so a git
   dependency reaches source_fetch through exactly one translation. */
[[nodiscard]] bool project_dep_to_source(const project_dep *dep, source_spec *out, char *err,
                                         size_t err_size);

/* The entry named `name`, or NULL. */
[[nodiscard]] const project_dep *project_deps_find(const project_deps *deps, const char *name);

/* The base URL declared for `name`, or NULL when it is not declared. */
[[nodiscard]] const char *project_registries_url(const project_registries *registries,
                                                 const char *name);

void project_deps_dump(const project_deps *deps, FILE *stream);

#endif /* MOLTO_PROJECT_DEPS_H */
