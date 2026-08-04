#ifndef MOLTO_PROJECT_CTX_H
#define MOLTO_PROJECT_CTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <molto/services/manifest_service.h>

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
#define PROJECT_MAX_ENV 32
#define PROJECT_ENV_NAME_MAX 64
#define PROJECT_ENV_VALUE_MAX 256

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
    char defines[PROJECT_MAX_OPTS][PROJECT_OPT_LEN];
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
    char
        requires[
            PROJECT_MAX_OPTS][PROJECT_OPT_LEN];
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
    project_target target;
    project_test test;
    project_env env;
    project_profiles profile;
    project_profile_options profile_options; /* per-profile extra options */
} project_ctx;

/* Parse a Project.toml `toml` string into `*out`. Built-in profile defaults are
   seeded first, then overridden by declared values. On failure returns false
   and writes a line-tagged reason into `err`. */
[[nodiscard]] bool project_parse(const char *toml, project_ctx *out, char *err, size_t err_size);

/* Read the file at `path` and delegate to project_parse. */
[[nodiscard]] bool project_load(const char *path, project_ctx *out, char *err, size_t err_size);

/* Print the populated context for debugging. */
void project_ctx_dump(const project_ctx *ctx, FILE *stream);

#endif /* MOLTO_PROJECT_CTX_H */
