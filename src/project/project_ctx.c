#include <molto/project/project_ctx.h>

#include <molto/services/fs_service.h>
#include <molto/util/doc.h>
#include <molto/util/toml.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Write a manifest error and return false, so callers can `return set_error(...)`. */
static bool set_error(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool set_error(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
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
    ctx->profile.debug = (manifest_profile){.opt_level = 0, .debug_info = true};
    ctx->profile.release = (manifest_profile){.opt_level = 3, .debug_info = false};
    ctx->profile.bench = (manifest_profile){.opt_level = 3, .debug_info = false};
    ctx->profile.custom = (manifest_profile){.opt_level = 2, .debug_info = true};
}

/*
 * Every key `[package]` defines, and the one table in the manifest that fails
 * closed on the rest.
 *
 * Everywhere else — `[target]`, `[profile.*]`, `[test]` — an unknown key is
 * dropped in silence, and the cost of a typo is a setting that did not take
 * effect on the machine that typed it. These keys are different: they leave the
 * machine. A `licence` misspelled once publishes a package that claims no
 * licence at all, to everyone who ever resolves it, and nothing about the build
 * looks wrong. So this table is read the way `[deps]` is (RFC-0008).
 */
static const char *const PACKAGE_KEYS[] = {
    "name", "version", "artifact", "description", "license", "homepage", "repository", "authors",
};

static bool is_package_key(const char *key) {
    for(size_t i = 0; i < sizeof PACKAGE_KEYS / sizeof PACKAGE_KEYS[0]; i++) {
        if(strcmp(PACKAGE_KEYS[i], key) == 0)
            return true;
    }
    return false;
}

/* Classified before anything is read, so the reason a manifest was refused is
   the key that is wrong and not whatever failed later because of it. */
[[nodiscard]] static bool check_package_keys(const toml_document *doc, char *err, size_t err_size) {
    str_list keys;
    str_list_init(&keys);
    if(!doc_table_members(doc_from_toml(doc), "package", &keys)) {
        str_list_free(&keys);
        return set_error(err, err_size, "could not read the [package] table");
    }

    bool ok = true;
    for(size_t i = 0; ok && i < str_list_count(&keys); i++) {
        const char *key = str_list_get(&keys, i);
        if(!is_package_key(key))
            ok = set_error(err, err_size, "[package]: unknown key '%s'", key);
    }

    str_list_free(&keys);
    return ok;
}

static bool map_test_mode(const char *name, test_mode *out) {
    if(strcmp(name, "per_file") == 0) {
        *out = test_mode_per_file;
        return true;
    }
    if(strcmp(name, "single") == 0) {
        *out = test_mode_single;
        return true;
    }
    return false;
}

static bool map_artifact(const char *name, artifact_kind *out) {
    if(strcmp(name, "source") == 0) {
        *out = artifact_source;
        return true;
    }
    if(strcmp(name, "static") == 0) {
        *out = artifact_static;
        return true;
    }
    if(strcmp(name, "shared") == 0) {
        *out = artifact_shared;
        return true;
    }
    return false;
}

/* Accepted values for [target].compiler (empty means autodetect). */
static bool valid_compiler(const char *name) {
    return name[0] == '\0' || strcmp(name, "gcc") == 0 || strcmp(name, "g++") == 0 ||
           strcmp(name, "clang") == 0 || strcmp(name, "llvm") == 0 || strcmp(name, "msvc") == 0;
}

/* Copy a string array from `doc[section][key]` into a fixed-size destination.
   Overflowing the capacity — or a single value that does not fit — is an error
   rather than a silent truncation: dropping a flag would produce a green build
   that used different options than the manifest asked for.

   The capacity passed is PROJECT_MAX_OPTS and not the destination's own size,
   because `defines` is deliberately larger: the last two slots belong to the
   name and version Molto contributes, and a manifest declaring the full
   sixteen must not take them. */
/* A host capability is a name and never a path (RFC-0016).
 *
 * Refused rather than accepted, because `include = ["/usr/include/gtk-3.0"]` is
 * the mistake this key exists to prevent and it is the one everybody whose
 * machine happens to work will make. The value that belongs here is what
 * pkg-config, vcpkg or a framework lookup is *asked* — `gtk+-3.0` — not what
 * any of them answered on one machine. */
[[nodiscard]] static bool check_host_capabilities(const project_target *target, char *err,
                                                  size_t err_size) {
    for(size_t i = 0; i < target->host_count; i++) {
        if(strchr(target->host[i], '/') == NULL)
            continue;
        return set_error(err, err_size,
                         "[target].host '%s' looks like a path, and a host library is named by "
                         "capability so the same manifest builds on another machine; write the "
                         "name a resolver is asked for, such as \"gtk+-3.0\"",
                         target->host[i]);
    }
    return true;
}

[[nodiscard]] static bool read_option_array(const toml_document *doc, const char *section,
                                            const char *key,
                                            char dest[PROJECT_MAX_OPTS][PROJECT_OPT_LEN],
                                            size_t *count, char *err, size_t err_size) {
    return doc_read_strings(doc_from_toml(doc), section, key, dest[0], PROJECT_MAX_OPTS,
                            PROJECT_OPT_LEN, count, err, err_size);
}

/*
 * Hand the package's own identity to the code being compiled.
 *
 * The name and the version are in the manifest already, and a program that
 * wants to report its version had until now to write it down a second time in
 * a header and keep the two in step by hand -- a binary answering with the
 * version before last looks exactly like one that was never rebuilt.
 *
 * Added after the manifest's own entries and past its limit, so a project
 * declaring the full sixteen defines loses none of them to these. A program
 * that does not use them does not notice they are there.
 */
[[nodiscard]] static bool add_package_defines(project_ctx *out, char *err, size_t err_size) {
    project_options *options = &out->target.options;
    const char *const names[PROJECT_PKG_DEFINES] = {
        PROJECT_PKG_NAME_DEFINE,
        PROJECT_PKG_VERSION_DEFINE,
    };
    const char *const values[PROJECT_PKG_DEFINES] = {
        out->project_name,
        out->version,
    };

    for(size_t i = 0; i < PROJECT_PKG_DEFINES; i++) {
        /* Quoted, because what reaches the compiler is a string literal: the
           preprocessor is handed -DMOLTO_PKG_VERSION="0.3.1" and the program
           sees a char array rather than a bare token it cannot use. */
        if(!fs_format_path(options->defines[options->define_count], PROJECT_OPT_LEN, "%s=\"%s\"",
                           names[i], values[i]))
            return set_error(err, err_size, "package %s is too long to pass to the compiler",
                             i == 0 ? "name" : "version");
        options->define_count++;
    }
    return true;
}

/* Read the [env] table. Its keys are the variable names, so they are discovered
   rather than declared in a schema. Values must be strings. */
[[nodiscard]] static bool read_env(const toml_document *doc, project_env *out, char *err,
                                   size_t err_size) {
    str_list names;
    str_list_init(&names);
    if(!toml_section_keys(doc, "env", &names)) {
        str_list_free(&names);
        return set_error(err, err_size, "could not read the [env] table");
    }
    /* Sorted here, once, rather than by whoever needs it: the order these were
       written in would otherwise reach the build fingerprints, and through them
       the key of a cache shared between projects. Two manifests that name the
       same variables would stop recognising each other's objects over the order
       of two lines. */
    str_list_sort(&names);

    bool ok = true;
    size_t total = str_list_count(&names);
    for(size_t i = 0; ok && i < total; i++) {
        const char *name = str_list_get(&names, i);
        if(out->count >= PROJECT_MAX_ENV) {
            ok = set_error(err, err_size, "[env] has more than %d entries", PROJECT_MAX_ENV);
            break;
        }
        char value[PROJECT_ENV_VALUE_MAX];
        if(!toml_get_string(doc, "env", name, value, sizeof value))
            ok = set_error(err, err_size, "[env].%s must be a string", name);
        else if(!fs_format_path(out->names[out->count], PROJECT_ENV_NAME_MAX, "%s", name))
            ok = set_error(err, err_size, "[env] name '%s' is longer than %d characters", name,
                           PROJECT_ENV_NAME_MAX - 1);
        else if(!fs_format_path(out->values[out->count], PROJECT_ENV_VALUE_MAX, "%s", value))
            ok = set_error(err, err_size, "[env].%s is longer than %d characters", name,
                           PROJECT_ENV_VALUE_MAX - 1);
        else
            out->count++;
    }
    str_list_free(&names);
    return ok;
}

/* Read defines/include/flags of a section into `out`. */
[[nodiscard]] static bool read_options(const toml_document *doc, const char *section,
                                       project_options *out, char *err, size_t err_size) {
    return read_option_array(doc, section, "defines", out->defines, &out->define_count, err,
                             err_size) &&
           read_option_array(doc, section, "include", out->include, &out->include_count, err,
                             err_size) &&
           read_option_array(doc, section, "flags", out->flags, &out->flag_count, err, err_size);
}

bool project_parse(const char *toml, project_ctx *out, char *err, size_t err_size) {
    seed_defaults(out);

    toml_document *doc = toml_parse(toml, err, err_size);
    if(doc == NULL)
        return false;

    if(!check_package_keys(doc, err, err_size)) {
        toml_free(doc);
        return false;
    }

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
    if(!toml_bind(doc, schema, field_count, out, err, err_size)) {
        toml_free(doc);
        return false;
    }

    /* artifact is an enum expressed as a string: handle it separately. */
    char artifact[16];
    if(toml_get_string(doc, "package", "artifact", artifact, sizeof artifact)) {
        if(!map_artifact(artifact, &out->artifact)) {
            toml_free(doc);
            return set_error(err, err_size, "unknown artifact kind '%s'", artifact);
        }
        /* No artifact kind changes what gets built yet: every project links an
           executable, and libraries would need ar / -shared / -fPIC. Accepting
           the key and quietly ignoring it would be a lie, so it is refused
           until it means something. */
        toml_free(doc);
        return set_error(err, err_size,
                         "artifact '%s' is not supported yet "
                         "(this version always builds an executable)",
                         artifact);
    }

    /* target.compiler must be a known toolchain (if given). */
    if(!valid_compiler(out->target.compiler)) {
        toml_free(doc);
        return set_error(err, err_size, "unknown compiler '%s'", out->target.compiler);
    }

    /* target.link is an array of library names, with the same no-silent-loss
       rule as the option arrays above. */
    str_list libs;
    str_list_init(&libs);
    bool ok = true;
    if(toml_get_array(doc, "target", "link", &libs)) {
        size_t count = str_list_count(&libs);
        for(size_t i = 0; ok && i < count; i++) {
            const char *lib = str_list_get(&libs, i);
            if(out->target.link_count >= PROJECT_MAX_LINK)
                ok = set_error(err, err_size, "[target].link has more than %d entries",
                               PROJECT_MAX_LINK);
            else if(!fs_format_path(out->target.link[out->target.link_count], PROJECT_LINK_NAME_MAX,
                                    "%s", lib))
                ok = set_error(err, err_size,
                               "[target].link entry '%s' is longer than %d characters", lib,
                               PROJECT_LINK_NAME_MAX - 1);
            else
                out->target.link_count++;
        }
    }
    str_list_free(&libs);

    /* [test].mode is an enum expressed as a string, like package.artifact. */
    char test_mode_name[16];
    if(ok && toml_get_string(doc, "test", "mode", test_mode_name, sizeof test_mode_name) &&
       !map_test_mode(test_mode_name, &out->test.mode))
        ok =
            set_error(err, err_size, "unknown test mode '%s' (per_file or single)", test_mode_name);

    ok = ok &&
         read_option_array(doc, "test", "sources", out->test.sources, &out->test.source_count, err,
                           err_size) &&
         read_options(doc, "test", &out->test.options, err, err_size);

    /* target.requires: the features the project needs from a compiler. */
    ok = ok && read_option_array(doc, "target", "requires", out->target.requires,
                                 &out->target.requires_count, err, err_size);

    /* target.host: the libraries it needs from the machine it builds on. A
       separate key from `requires` and not an extension of it — that one is a
       question for pickup about a compiler, this one is about a library, and
       one list answered by two resolvers is a list nobody can read. */
    ok = ok &&
         read_option_array(doc, "target", "host", out->target.host, &out->target.host_count, err,
                           err_size) &&
         check_host_capabilities(&out->target, err, err_size);

    /* Base compilation options ([target]), the [env] table, and per-profile
       additions. */
    ok = ok && read_env(doc, &out->env, err, err_size) &&
         read_options(doc, "target", &out->target.options, err, err_size) &&
         read_options(doc, "profile.debug", &out->profile_options.debug, err, err_size) &&
         read_options(doc, "profile.release", &out->profile_options.release, err, err_size) &&
         read_options(doc, "profile.bench", &out->profile_options.bench, err, err_size) &&
         read_options(doc, "profile.custom", &out->profile_options.custom, err, err_size);

    /* The rest of `[package]`. Read with the same code a recipe's `[about]` is
       read with, because RFC-0009 requires the two to say the same thing and
       two readers of one format drift. */
    ok = ok && manifest_read_about(doc_from_toml(doc), "package", &out->about, err, err_size);

    /* Dependencies, and the registries one may name. Checked together after
       both are read, because a manifest may declare them in either order.

       Both tables are read here and kept apart: what separates them is not how
       they are written but where their flags are allowed to land, and that is
       the build's decision (RFC-0008). */
    ok = ok && project_deps_read_doc(doc_from_toml(doc), &out->deps, err, err_size) &&
         project_dev_deps_read_doc(doc_from_toml(doc), &out->dev_deps, err, err_size) &&
         project_registries_read(doc, &out->registries, err, err_size) &&
         project_deps_check_registries(&out->deps, "deps", &out->registries, err, err_size) &&
         project_deps_check_registries(&out->dev_deps, "dev-deps", &out->registries, err, err_size);

    toml_free(doc);
    if(!ok)
        return false;

    if(!manifest_is_valid_name(out->project_name))
        return set_error(err, err_size, "package name is missing or not snake_case");
    return add_package_defines(out, err, err_size);
}

/* The directory `path` sits in, which is the project root. */
static void manifest_dir(const char *path, char *out, size_t out_size) {
    if((size_t)snprintf(out, out_size, "%s", path) >= out_size) {
        out[0] = '\0'; /* too long to anchor against: the working directory it is */
        return;
    }
    char *slash = strrchr(out, '/');
    if(slash == NULL) {
        out[0] = '\0'; /* a bare "Project.toml": the root is where we are */
        return;
    }
    /* "/Project.toml" at the filesystem root leaves the slash rather than "". */
    *(slash == out ? slash + 1 : slash) = '\0';
}

bool project_load(const char *path, project_ctx *out, char *err, size_t err_size) {
    char *toml = fs_read_file(path);
    if(toml == NULL) {
        if(err != NULL && err_size > 0)
            snprintf(err, err_size, "could not read '%s'", path);
        return false;
    }
    bool ok = project_parse(toml, out, err, err_size);
    free(toml);
    /* After the parse, which zeroes the whole struct. */
    if(ok)
        manifest_dir(path, out->root, sizeof out->root);
    return ok;
}

void project_ctx_dump(const project_ctx *ctx, FILE *stream) {
    static const char *artifact_names[] = {"source", "static", "shared"};
    fprintf(stream, "project_name = %s\n", ctx->project_name);
    fprintf(stream, "version      = %s\n", ctx->version);
    fprintf(stream, "artifact     = %s\n", artifact_names[ctx->artifact]);
    fprintf(stream, "license      = %s\n",
            ctx->about.license[0] != '\0' ? ctx->about.license : "(none stated)");
    fprintf(stream, "target.compiler = %s\n",
            ctx->target.compiler[0] != '\0' ? ctx->target.compiler : "(auto)");
    fprintf(stream, "target.std      = %s\n",
            ctx->target.std[0] != '\0' ? ctx->target.std : "(default)");
    fprintf(stream, "target.cpp_std  = %s\n",
            ctx->target.cpp_std[0] != '\0' ? ctx->target.cpp_std : "(default)");
    fprintf(stream, "target.link     = [");
    for(size_t i = 0; i < ctx->target.link_count; i++)
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
    project_deps_dump(&ctx->deps, stream);
}

bool project_test_sources(const project_test *test, str_list *out) {
    for(size_t i = 0; i < test->source_count; i++) {
        if(!str_list_push(out, test->sources[i]))
            return false;
    }
    return true;
}
