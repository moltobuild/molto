#include <molto/project/project_ctx.h>

#include <molto/services/fs_service.h>
#include <molto/util/toml.h>

#include <stdlib.h>
#include <string.h>

/* Built-in profile defaults (RFC-0003 / spec section 13). Live here because
   they are project-level policy, not tied to the compiler backend. */
static void seed_defaults(project_ctx *ctx) {
    memset(ctx, 0, sizeof *ctx);
    snprintf(ctx->version, sizeof ctx->version, "%s", "0.0.0");
    ctx->artifact = artifact_static;
    ctx->profile.debug = (manifest_profile){ .opt_level = 0, .debug_info = true };
    ctx->profile.release = (manifest_profile){ .opt_level = 3, .debug_info = false };
    ctx->profile.bench = (manifest_profile){ .opt_level = 3, .debug_info = false };
    ctx->profile.custom = (manifest_profile){ .opt_level = 2, .debug_info = true };
}

static bool map_artifact(const char *name, artifact_kind *out) {
    if (strcmp(name, "source") == 0) {
        *out = artifact_source;
        return true;
    }
    if (strcmp(name, "static") == 0) {
        *out = artifact_static;
        return true;
    }
    if (strcmp(name, "shared") == 0) {
        *out = artifact_shared;
        return true;
    }
    return false;
}

/* Accepted values for [target].compiler (empty means autodetect). */
static bool valid_compiler(const char *name) {
    return name[0] == '\0'
        || strcmp(name, "gcc") == 0 || strcmp(name, "g++") == 0
        || strcmp(name, "clang") == 0 || strcmp(name, "llvm") == 0
        || strcmp(name, "msvc") == 0;
}

bool project_parse(const char *toml, project_ctx *out, char *err, size_t err_size) {
    seed_defaults(out);

    toml_document *doc = toml_parse(toml, err, err_size);
    if (doc == NULL)
        return false;

    const toml_field schema[] = {
        TOML_STR(project_ctx, "package", "name", project_name),
        TOML_STR(project_ctx, "package", "version", version),
        TOML_STR(project_ctx, "target", "compiler", target.compiler),
        TOML_STR(project_ctx, "target", "std", target.std),
        TOML_STR(project_ctx, "target", "cpp_std", target.cpp_std),
        TOML_INT(project_ctx, "profile.debug", "opt_level", profile.debug.opt_level),
        TOML_BOOL(project_ctx, "profile.debug", "debug_info", profile.debug.debug_info),
        TOML_INT(project_ctx, "profile.release", "opt_level", profile.release.opt_level),
        TOML_BOOL(project_ctx, "profile.release", "debug_info", profile.release.debug_info),
        TOML_INT(project_ctx, "profile.bench", "opt_level", profile.bench.opt_level),
        TOML_BOOL(project_ctx, "profile.bench", "debug_info", profile.bench.debug_info),
        TOML_INT(project_ctx, "profile.custom", "opt_level", profile.custom.opt_level),
        TOML_BOOL(project_ctx, "profile.custom", "debug_info", profile.custom.debug_info),
    };
    size_t field_count = sizeof schema / sizeof schema[0];
    if (!toml_bind(doc, schema, field_count, out, err, err_size)) {
        toml_free(doc);
        return false;
    }

    /* artifact is an enum expressed as a string: handle it separately. */
    char artifact[16];
    if (toml_get_string(doc, "package", "artifact", artifact, sizeof artifact)) {
        if (!map_artifact(artifact, &out->artifact)) {
            if (err != NULL && err_size > 0)
                snprintf(err, err_size, "unknown artifact kind '%s'", artifact);
            toml_free(doc);
            return false;
        }
    }

    /* target.compiler must be a known toolchain (if given). */
    if (!valid_compiler(out->target.compiler)) {
        if (err != NULL && err_size > 0)
            snprintf(err, err_size, "unknown compiler '%s'", out->target.compiler);
        toml_free(doc);
        return false;
    }

    /* target.link is an array: copy up to PROJECT_MAX_LINK library names. */
    str_list libs;
    str_list_init(&libs);
    if (toml_get_array(doc, "target", "link", &libs)) {
        size_t count = str_list_count(&libs);
        for (size_t i = 0; i < count && out->target.link_count < PROJECT_MAX_LINK; i++)
            snprintf(out->target.link[out->target.link_count++],
                     PROJECT_LINK_NAME_MAX, "%s", str_list_get(&libs, i));
    }
    str_list_free(&libs);

    toml_free(doc);

    if (!manifest_is_valid_name(out->project_name)) {
        if (err != NULL && err_size > 0)
            snprintf(err, err_size, "package name is missing or not snake_case");
        return false;
    }
    return true;
}

bool project_load(const char *path, project_ctx *out, char *err, size_t err_size) {
    char *toml = fs_read_file(path);
    if (toml == NULL) {
        if (err != NULL && err_size > 0)
            snprintf(err, err_size, "could not read '%s'", path);
        return false;
    }
    bool ok = project_parse(toml, out, err, err_size);
    free(toml);
    return ok;
}

void project_ctx_dump(const project_ctx *ctx, FILE *stream) {
    static const char *artifact_names[] = { "source", "static", "shared" };
    fprintf(stream, "project_name = %s\n", ctx->project_name);
    fprintf(stream, "version      = %s\n", ctx->version);
    fprintf(stream, "artifact     = %s\n", artifact_names[ctx->artifact]);
    fprintf(stream, "target.compiler = %s\n",
            ctx->target.compiler[0] != '\0' ? ctx->target.compiler : "(auto)");
    fprintf(stream, "target.std      = %s\n",
            ctx->target.std[0] != '\0' ? ctx->target.std : "(default)");
    fprintf(stream, "target.cpp_std  = %s\n",
            ctx->target.cpp_std[0] != '\0' ? ctx->target.cpp_std : "(default)");
    fprintf(stream, "target.link     = [");
    for (size_t i = 0; i < ctx->target.link_count; i++)
        fprintf(stream, "%s%s", i > 0 ? ", " : "", ctx->target.link[i]);
    fprintf(stream, "]\n");
    fprintf(stream, "profile.debug   = { opt_level = %d, debug_info = %s }\n",
            ctx->profile.debug.opt_level, ctx->profile.debug.debug_info ? "true" : "false");
    fprintf(stream, "profile.release = { opt_level = %d, debug_info = %s }\n",
            ctx->profile.release.opt_level, ctx->profile.release.debug_info ? "true" : "false");
    fprintf(stream, "profile.bench   = { opt_level = %d, debug_info = %s }\n",
            ctx->profile.bench.opt_level, ctx->profile.bench.debug_info ? "true" : "false");
    fprintf(stream, "profile.custom  = { opt_level = %d, debug_info = %s }\n",
            ctx->profile.custom.opt_level, ctx->profile.custom.debug_info ? "true" : "false");
}
