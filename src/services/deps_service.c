#include <molto/services/deps_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/recipe_service.h>
#include <molto/services/source_discovery.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

/* --- the whole graph --- */

/* Nodes come out sorted by name, so the flags a build receives are the same on
   every machine. That is the only ordering guarantee made here: `-l` entries
   name system libraries, which the platform resolves, and nothing is a built
   library whose link order could matter yet. */
static bool collect_scope(const dep_graph *graph, unsigned wanted, unsigned without,
                          prepared_deps *out, char *err, size_t err_size) {
    for(size_t i = 0; i < dep_graph_count(graph); i++) {
        const dep_node *node = dep_graph_at(graph, i);
        if((node->scope & wanted) == 0 || (node->scope & without) != 0)
            continue;
        char reason[512] = "";
        if(!collect(&node->artifacts, node->root, out, reason, sizeof reason))
            return set_error(err, err_size, "dependency '%s': %s", node->name, reason);
    }
    return true;
}

bool deps_prepare_graph(const dep_graph *graph, prepared_deps *out, char *err, size_t err_size) {
    return collect_scope(graph, dep_scope_runtime, 0, out, err, err_size);
}

bool deps_prepare_dev(const dep_graph *graph, prepared_deps *out, char *err, size_t err_size) {
    /* Only what the test build *adds*. A package required by both tables is
       already in the runtime set, and handing it over twice would put its
       sources on the test link line twice — which is a duplicate symbol, the
       exact failure the one-version rule exists to prevent. */
    return collect_scope(graph, dep_scope_dev, dep_scope_runtime, out, err, err_size);
}

bool deps_prepare(const project_ctx *ctx, prepared_deps *out, char *err, size_t err_size) {
    if(ctx->deps.count == 0)
        return true;

    dep_graph *graph = NULL;
    if(!dep_graph_resolve(ctx, &graph, err, err_size))
        return false;

    const bool ok = deps_prepare_graph(graph, out, err, err_size);
    dep_graph_free(graph);
    return ok;
}
