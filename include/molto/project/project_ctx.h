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

/* Build settings for each known profile. Access as ctx.profile.release, etc. */
typedef struct {
    manifest_profile debug;
    manifest_profile release;
    manifest_profile bench;
    manifest_profile custom;
} project_profiles;

#define PROJECT_MAX_LINK      32
#define PROJECT_LINK_NAME_MAX 64

/* The `[target]` table: toolchain and compilation settings (RFC-0003). */
typedef struct {
    char compiler[16];  /* "gcc"/"g++"/"clang"/"llvm"/"msvc"; "" = autodetect */
    char std[16];       /* C standard, e.g. "c23"; "" = compiler default */
    char cpp_std[16];   /* C++ standard, e.g. "c++20"; "" = compiler default */
    char link[PROJECT_MAX_LINK][PROJECT_LINK_NAME_MAX]; /* system libraries */
    size_t link_count;
} project_target;

/* The parsed Project.toml as a typed domain model. */
typedef struct {
    char project_name[128];
    char version[64];
    artifact_kind artifact;
    project_target target;
    project_profiles profile;
} project_ctx;

/* Parse a Project.toml `toml` string into `*out`. Built-in profile defaults are
   seeded first, then overridden by declared values. On failure returns false
   and writes a line-tagged reason into `err`. */
[[nodiscard]] bool project_parse(const char *toml, project_ctx *out,
                                 char *err, size_t err_size);

/* Read the file at `path` and delegate to project_parse. */
[[nodiscard]] bool project_load(const char *path, project_ctx *out,
                                char *err, size_t err_size);

/* Print the populated context for debugging. */
void project_ctx_dump(const project_ctx *ctx, FILE *stream);

#endif /* MOLTO_PROJECT_CTX_H */
