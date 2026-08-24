#ifndef MOLTO_PROJECT_CTX_H
#define MOLTO_PROJECT_CTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <molto/project/project_deps.h>
#include <molto/services/manifest_service.h>
#include <molto/util/str_list.h>

/* Artifact kinds from RFC-0003 (spec section 9). */
typedef enum {
    artifact_source,
    artifact_static,
    artifact_shared,
} artifact_kind;

#define PROJECT_MAX_LINK 32
#define PROJECT_LINK_NAME_MAX 64
#define PROJECT_MAX_OPTS 16
#define PROJECT_OPT_LEN 96

/* Room for the directory a manifest was read from. */
#define PROJECT_ROOT_MAX 1024
#define PROJECT_MAX_ENV 32
#define PROJECT_ENV_NAME_MAX 64
#define PROJECT_ENV_VALUE_MAX 256

/* Defines Molto adds to [target] itself, on top of whatever the manifest
   declares: the package's name and its version. They sit past the manifest's
   own limit so that a project declaring the full sixteen loses none of them to
   something it did not ask for. */
#define PROJECT_PKG_DEFINES 2
#define PROJECT_PKG_NAME_DEFINE "MOLTO_PKG_NAME"
#define PROJECT_PKG_VERSION_DEFINE "MOLTO_PKG_VERSION"

/* Build settings for each known profile. Access as ctx.profile.release, etc. */
typedef struct {
    manifest_profile debug;
    manifest_profile release;
    manifest_profile bench;
    manifest_profile custom;
} project_profiles;

/* Extra compilation options for a scope ([target] base or a profile).
   defines -> -D, include -> -I, flags -> passed verbatim. */
typedef struct {
    /* Room for the manifest's own entries plus the ones Molto contributes. Only
       [target] ever receives those, but the shape is shared with the profiles. */
    char defines[PROJECT_MAX_OPTS + PROJECT_PKG_DEFINES][PROJECT_OPT_LEN];
    size_t define_count;
    char include[PROJECT_MAX_OPTS][PROJECT_OPT_LEN];
    size_t include_count;
    char flags[PROJECT_MAX_OPTS][PROJECT_OPT_LEN];
    size_t flag_count;
} project_options;

/* Per-profile extra options, added on top of the [target] base. */
typedef struct {
    project_options debug;
    project_options release;
    project_options bench;
    project_options custom;
} project_profile_options;

/* The `[target]` table: toolchain and compilation settings (RFC-0003). */
typedef struct {
    char compiler[16]; /* preferred vendor: "gcc"/"clang"/"msvc"; "" = any */
    char std[16];      /* C standard, e.g. "c23"; "" = compiler default */
    char cpp_std[16];  /* C++ standard, e.g. "c++20"; "" = compiler default */
    char link[PROJECT_MAX_LINK][PROJECT_LINK_NAME_MAX]; /* system libraries */
    size_t link_count;
    /* Compiler features the project needs, proven rather than assumed (see
       pickup). Naming capabilities instead of a binary is what keeps a
       manifest portable between machines. */
    char requires[PROJECT_MAX_OPTS][PROJECT_OPT_LEN];
    size_t requires_count;
    project_options options; /* base defines/include/flags for all profiles */
} project_target;

/* How `molto test` lays out the test executables. */
typedef enum {
    /* One executable per test file, each bringing its own main(). The default,
       and the contract RFC-0002 has always described. */
    test_mode_per_file,
    /* Every test file linked into a single executable. What frameworks that
       register their cases and supply their own main() need. */
    test_mode_single,
} test_mode;

/* The `[test]` table: how tests are built, and what they need beyond the
   project's own sources. */
typedef struct {
    test_mode mode;
    /* Extra sources compiled into the tests only: directories are walked, plain
       files taken as they are. This is how a test framework living outside
       src/ gets compiled in. */
    char sources[PROJECT_MAX_OPTS][PROJECT_OPT_LEN];
    size_t source_count;
    project_options options; /* defines/include/flags applied only to tests */
} project_test;

/* The `[env]` table: variables exported to every process Molto spawns for this
   project — compiler, linker and `molto run` (RFC-0003). */
typedef struct {
    char names[PROJECT_MAX_ENV][PROJECT_ENV_NAME_MAX];
    char values[PROJECT_MAX_ENV][PROJECT_ENV_VALUE_MAX];
    size_t count;
} project_env;

/* The parsed Project.toml as a typed domain model. */
typedef struct {
    char project_name[128];
    char version[64];
    artifact_kind artifact;
    /* The rest of `[package]`: what the package says about itself rather than
       what it needs to build. Nothing here reaches a compile line. */
    manifest_about about;
    project_target target;
    project_test test;
    project_env env;
    project_deps deps;
    /* `[dev-deps]`: resolved alongside the others, but their flags reach only
       the test build. Kept apart from `deps` rather than tagged inside it,
       because "does this ship?" is answered by which list a dependency is in. */
    project_deps dev_deps;
    project_registries registries;
    project_profiles profile;
    project_profile_options profile_options; /* per-profile extra options */
    /* The directory the manifest was read from, absolute or as the caller
       named it. A relative path in a manifest anchors here (RFC-0003), and
       until something knows where "here" is, it cannot.

       `project_load` fills it; `project_parse` leaves it empty, because a
       string has no directory. Empty means the working directory, which is
       what a relative path has always meant to the code that reads one. */
    char root[PROJECT_ROOT_MAX];
} project_ctx;

/* Parse a Project.toml `toml` string into `*out`. Built-in profile defaults are
   seeded first, then overridden by declared values. On failure returns false
   and writes a line-tagged reason into `err`. */
[[nodiscard]] bool project_parse(const char *toml, project_ctx *out, char *err, size_t err_size);

/* Read the file at `path` and delegate to project_parse. */
[[nodiscard]] bool project_load(const char *path, project_ctx *out, char *err, size_t err_size);

/* Print the populated context for debugging. */
void project_ctx_dump(const project_ctx *ctx, FILE *stream);

/* `[test].sources` as a list.
 *
 * The table stores them as a fixed array of fixed strings, which is what a
 * manifest with hard limits wants and what no consumer of it wants. The
 * conversion lives here so that the two callers that need one — the build,
 * which compiles those sources, and the native frontend, which describes them —
 * cannot spell it two ways, and so that neither has to know the shape of the
 * array. */
[[nodiscard]] bool project_test_sources(const project_test *test, str_list *out);

#endif /* MOLTO_PROJECT_CTX_H */
