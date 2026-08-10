#include <molto/services/deps_service.h>

#include <molto/services/credentials_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/recipe_service.h>
#include <molto/services/registry_service.h>
#include <molto/services/resolve_service.h>
#include <molto/services/source_discovery.h>
#include <molto/services/source_service.h>
#include <molto/util/toml.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The file a dependency that carries its own source has to bring. `[deps]` can
   say where bytes are but not what to compile out of them, so the source
   describes itself — the same way a git dependency in Cargo brings its own
   Cargo.toml. */
#define CARRIED_RECIPE "recipe.toml"

/* Where a source with no version of its own is cached: it is not
   platform-specific, and its cache key is a digest or a commit id. */
#define CARRIED_TARGET "any"

#define DEPS_PATH_MAX 1024

static bool set_error(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool set_error(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

void prepared_deps_init(prepared_deps *out) {
    str_list_init(&out->sources);
    str_list_init(&out->includes);
    str_list_init(&out->defines);
    str_list_init(&out->flags);
    str_list_init(&out->links);
}

void prepared_deps_free(prepared_deps *out) {
    str_list_free(&out->sources);
    str_list_free(&out->includes);
    str_list_free(&out->defines);
    str_list_free(&out->flags);
    str_list_free(&out->links);
}

/* --- which registry answers for a dependency --- */

/* `[registries]` if the dependency named one, then whatever `molto login`
   stored, then the official one. A dependency naming a registry the manifest
   does not declare was already refused when the manifest was read. */
static const char *registry_for(const project_ctx *ctx, const project_dep *dep,
                                const credentials *creds) {
    if(dep->registry[0] != '\0') {
        const char *url = project_registries_url(&ctx->registries, dep->registry);
        if(url != NULL)
            return url;
    }
    if(creds->registry[0] != '\0')
        return creds->registry;
    return REGISTRY_DEFAULT_URL;
}

/* --- collecting what a dependency contributes --- */

/* A path inside the fetched source, absolute, because the cache is not under
   the project root and the build anchors a relative path there. */
static bool push_rooted(str_list *out, const char *root, const char *relative, char *err,
                        size_t err_size) {
    char path[DEPS_PATH_MAX];
    if(!fs_format_path(path, sizeof path, "%s/%s", root, relative))
        return set_error(err, err_size, "the path '%s/%s' is too long", root, relative);
    if(!str_list_push(out, path))
        return set_error(err, err_size, "out of memory collecting dependencies");
    return true;
}

static bool push_all(str_list *out, const char list[][PROJECT_OPT_LEN], size_t count, char *err,
                     size_t err_size) {
    for(size_t i = 0; i < count; i++) {
        if(!str_list_push(out, list[i]))
            return set_error(err, err_size, "out of memory collecting dependencies");
    }
    return true;
}

/* Every source under `root` the recipe wants compiled.

   A recipe that names its sources is taken at its word. One that names none is
   asking for everything the drop contains, which has to be discovered — and
   filtered, because `exclude` exists precisely for the file that must not be
   compiled. */
static bool collect_sources(const recipe_artifacts *artifacts, const char *root, str_list *out,
                            char *err, size_t err_size) {
    if(artifacts->source_count > 0) {
        for(size_t i = 0; i < artifacts->source_count; i++) {
            if(!recipe_artifacts_wants(artifacts, artifacts->sources[i]))
                continue;
            char path[DEPS_PATH_MAX];
            if(!fs_format_path(path, sizeof path, "%s/%s", root, artifacts->sources[i]))
                return set_error(err, err_size, "the source path for '%s' is too long",
                                 artifacts->sources[i]);
            if(!fs_path_exists(path))
                return set_error(err, err_size,
                                 "the recipe names '%s', which the source does not "
                                 "contain",
                                 artifacts->sources[i]);
            if(!str_list_push(out, path))
                return set_error(err, err_size, "out of memory collecting dependencies");
        }
        return true;
    }

    str_list found;
    str_list_init(&found);
    if(!source_discovery_collect(root, &found)) {
        str_list_free(&found);
        return set_error(err, err_size, "could not read the fetched source at '%s'", root);
    }

    bool ok = true;
    for(size_t i = 0; ok && i < str_list_count(&found); i++) {
        const char *path = str_list_get(&found, i);
        const char *name = path + strlen(root) + 1; /* what `exclude` names */
        if(recipe_artifacts_wants(artifacts, name) && !str_list_push(out, path))
            ok = set_error(err, err_size, "out of memory collecting dependencies");
    }
    str_list_free(&found);
    return ok;
}

static bool collect(const recipe_artifacts *artifacts, const char *root, prepared_deps *out,
                    char *err, size_t err_size) {
    /* Only a source drop contributes translation units. A static or shared
       artifact is already built, and molto cannot consume one yet. */
    if(artifacts->type != recipe_artifact_source)
        return set_error(err, err_size,
                         "this dependency is published as a built library, and molto can only "
                         "consume [artifacts] type = \"source\" yet");

    if(!collect_sources(artifacts, root, &out->sources, err, err_size))
        return false;

    for(size_t i = 0; i < artifacts->options.include_count; i++) {
        const char *directory = artifacts->options.include[i];
        if(!push_rooted(&out->includes, root, directory, err, err_size))
            return false;
    }
    for(size_t i = 0; i < artifacts->link_count; i++) {
        if(!str_list_push(&out->links, artifacts->link[i]))
            return set_error(err, err_size, "out of memory collecting dependencies");
    }
    return push_all(&out->defines, artifacts->options.defines, artifacts->options.define_count, err,
                    err_size) &&
           push_all(&out->flags, artifacts->options.flags, artifacts->options.flag_count, err,
                    err_size);
}

/* --- one dependency --- */

static bool prepare_from_registry(const project_ctx *ctx, const project_dep *dep,
                                  const credentials *creds, prepared_deps *out, char *err,
                                  size_t err_size) {
    resolved_dep resolved;
    if(!resolve_version(registry_for(ctx, dep, creds), dep->name, dep->version, &resolved, err,
                        err_size))
        return false;
    if(resolved.coordinate.form != recipe_form_source)
        return set_error(err, err_size,
                         "%s %s is published as a prebuilt artifact, and molto cannot consume one "
                         "yet",
                         dep->name, dep->version);

    char root[DEPS_PATH_MAX];
    if(!source_fetch(&resolved.source, dep->name, dep->version, resolved.coordinate.target, root,
                     sizeof root, err, err_size))
        return false;

    return collect(&resolved.artifacts, root, out, err, err_size);
}

/* The recipe a fetched source brings with it. Read through the same doc_view
   the registry's answer goes through, so the two cannot come to disagree. */
static bool read_carried_recipe(const char *root, const char *name, recipe_artifacts *out,
                                char *err, size_t err_size) {
    char path[DEPS_PATH_MAX];
    if(!fs_format_path(path, sizeof path, "%s/" CARRIED_RECIPE, root))
        return set_error(err, err_size, "the recipe path for '%s' is too long", name);
    if(!fs_path_exists(path))
        return set_error(err, err_size,
                         "'%s' brings no " CARRIED_RECIPE " at the root of its source, and [deps] "
                         "has nowhere to say what to compile",
                         name);

    char *text = fs_read_file(path);
    if(text == NULL)
        return set_error(err, err_size, "could not read %s", path);

    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    free(text);
    if(doc == NULL)
        return set_error(err, err_size, "%s is not valid TOML: %s", path, parse_err);

    const bool ok = recipe_read_artifacts(doc_from_toml(doc), out, err, err_size);
    toml_free(doc);
    return ok;
}

static bool prepare_carried(const project_dep *dep, prepared_deps *out, char *err,
                            size_t err_size) {
    source_spec spec;
    if(!project_dep_to_source(dep, &spec, err, err_size))
        return false;

    /* A path dependency is used where it is, so it has no cache key and its
       directory is the source. */
    char root[DEPS_PATH_MAX];
    if(spec.origin == source_origin_path) {
        if(!source_fetch(&spec, dep->name, "", CARRIED_TARGET, root, sizeof root, err, err_size))
            return false;
    } else {
        char key[SOURCE_DIGEST_MAX];
        if(!source_cache_key(&spec, key, sizeof key, err, err_size))
            return false;
        if(!source_fetch(&spec, dep->name, key, CARRIED_TARGET, root, sizeof root, err, err_size))
            return false;
    }

    recipe_artifacts artifacts;
    if(!read_carried_recipe(root, dep->name, &artifacts, err, err_size))
        return false;
    return collect(&artifacts, root, out, err, err_size);
}

bool deps_prepare(const project_ctx *ctx, prepared_deps *out, char *err, size_t err_size) {
    if(ctx->deps.count == 0)
        return true;

    /* Reads need no token, so a machine that never ran `molto login` resolves
       against the official registry like any other. */
    credentials creds = {0};
    (void)credentials_load(&creds, NULL, 0);

    for(size_t i = 0; i < ctx->deps.count; i++) {
        const project_dep *dep = &ctx->deps.items[i];
        char reason[512] = "";
        const bool ok = dep->resolution == dep_resolution_registry
                            ? prepare_from_registry(ctx, dep, &creds, out, reason, sizeof reason)
                            : prepare_carried(dep, out, reason, sizeof reason);
        if(!ok)
            return set_error(err, err_size, "dependency '%s': %s", dep->name, reason);
    }
    return true;
}
