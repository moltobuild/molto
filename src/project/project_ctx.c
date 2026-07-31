#include <molto/project/project_ctx.h>

#include <molto/services/fs_service.h>
#include <molto/util/toml.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Write a manifest error and return false, so callers can `return set_error(...)`. */
static bool set_error(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool set_error(char *err, size_t err_size, const char *format, ...) {
    if (err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

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

/* Copy a string array from `doc[section][key]` into a fixed-size destination.
   Overflowing the capacity — or a single value that does not fit — is an error
   rather than a silent truncation: dropping a flag would produce a green build
   that used different options than the manifest asked for. */
[[nodiscard]] static bool read_option_array(const toml_document *doc, const char *section,
                                            const char *key,
                                            char dest[PROJECT_MAX_OPTS][PROJECT_OPT_LEN],
                                            size_t *count, char *err, size_t err_size) {
    str_list values;
    str_list_init(&values);
    bool ok = true;
    if (toml_get_array(doc, section, key, &values)) {
        size_t total = str_list_count(&values);
        for (size_t i = 0; ok && i < total; i++) {
            const char *value = str_list_get(&values, i);
            if (*count >= PROJECT_MAX_OPTS)
                ok = set_error(err, err_size, "[%s].%s has more than %d entries",
                               section, key, PROJECT_MAX_OPTS);
            else if (!fs_format_path(dest[*count], PROJECT_OPT_LEN, "%s", value))
                ok = set_error(err, err_size,
                               "[%s].%s entry '%s' is longer than %d characters",
                               section, key, value, PROJECT_OPT_LEN - 1);
            else
                (*count)++;
        }
    }
    str_list_free(&values);
    return ok;
}

/* Read defines/include/flags of a section into `out`. */
[[nodiscard]] static bool read_options(const toml_document *doc, const char *section,
                                       project_options *out, char *err, size_t err_size) {
    return read_option_array(doc, section, "defines", out->defines,
                             &out->define_count, err, err_size)
        && read_option_array(doc, section, "include", out->include,
                             &out->include_count, err, err_size)
        && read_option_array(doc, section, "flags", out->flags,
                             &out->flag_count, err, err_size);
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
            toml_free(doc);
            return set_error(err, err_size, "unknown artifact kind '%s'", artifact);
        }
    }

    /* target.compiler must be a known toolchain (if given). */
    if (!valid_compiler(out->target.compiler)) {
        toml_free(doc);
        return set_error(err, err_size, "unknown compiler '%s'", out->target.compiler);
    }

    /* target.link is an array of library names, with the same no-silent-loss
       rule as the option arrays above. */
    str_list libs;
    str_list_init(&libs);
    bool ok = true;
    if (toml_get_array(doc, "target", "link", &libs)) {
        size_t count = str_list_count(&libs);
        for (size_t i = 0; ok && i < count; i++) {
            const char *lib = str_list_get(&libs, i);
            if (out->target.link_count >= PROJECT_MAX_LINK)
                ok = set_error(err, err_size, "[target].link has more than %d entries",
                               PROJECT_MAX_LINK);
            else if (!fs_format_path(out->target.link[out->target.link_count],
                                     PROJECT_LINK_NAME_MAX, "%s", lib))
                ok = set_error(err, err_size,
                               "[target].link entry '%s' is longer than %d characters",
                               lib, PROJECT_LINK_NAME_MAX - 1);
            else
                out->target.link_count++;
        }
    }
    str_list_free(&libs);

    /* Base compilation options ([target]) and per-profile additions. */
    ok = ok && read_options(doc, "target", &out->target.options, err, err_size)
            && read_options(doc, "profile.debug", &out->profile_options.debug,
                            err, err_size)
            && read_options(doc, "profile.release", &out->profile_options.release,
                            err, err_size)
            && read_options(doc, "profile.bench", &out->profile_options.bench,
                            err, err_size)
            && read_options(doc, "profile.custom", &out->profile_options.custom,
                            err, err_size);

    toml_free(doc);
    if (!ok)
        return false;

    if (!manifest_is_valid_name(out->project_name))
        return set_error(err, err_size, "package name is missing or not snake_case");
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
