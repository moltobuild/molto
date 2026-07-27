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

/* The parsed Project.toml as a typed domain model. */
typedef struct {
    char project_name[128];
    char version[64];
    artifact_kind artifact;
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
