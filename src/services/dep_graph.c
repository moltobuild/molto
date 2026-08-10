#include <molto/services/dep_graph.h>

#include <molto/services/credentials_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/registry_service.h>
#include <molto/services/resolve_service.h>
#include <molto/util/str_map.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The file a dependency that carries its own source has to bring. `[deps]` can
   say where bytes are but not what to compile out of them, so the source
   describes itself. */
#define CARRIED_RECIPE "recipe.toml"

/* Where a source with no version of its own is cached: it is not
   platform-specific, and its cache key is a digest or a commit id. */
#define CARRIED_TARGET "any"

/* A walk wider than this is a mistake rather than a project. The bound exists
   because the queue is a fixed array and because a runaway graph should stop
   with a message instead of with an allocator. */
#define DEP_GRAPH_MAX_NODES 256

struct dep_graph {
    dep_node **nodes; /* owned, sorted by name once the walk is done */
    size_t count;
    size_t capacity;
    str_map *index; /* name -> dep_node*, borrowed; `nodes` owns them */
};

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

/* --- the graph as a container --- */

static void node_free(dep_node *node) {
    if(node == NULL)
        return;
    str_list_free(&node->dependencies);
    free(node);
}

static bool graph_push(dep_graph *graph, dep_node *node, char *err, size_t err_size) {
    if(graph->count == graph->capacity) {
        const size_t grown = graph->capacity == 0 ? 8 : graph->capacity * 2;
        dep_node **items = (dep_node **)realloc((void *)graph->nodes, grown * sizeof *items);
        if(items == NULL)
            return set_error(err, err_size, "out of memory resolving dependencies");
        graph->nodes = items;
        graph->capacity = grown;
    }
    if(!str_map_put(graph->index, node->name, node))
        return set_error(err, err_size, "out of memory resolving dependencies");
    graph->nodes[graph->count++] = node;
    return true;
}

static int by_name(const void *left, const void *right) {
    const dep_node *const *a = (const dep_node *const *)left;
    const dep_node *const *b = (const dep_node *const *)right;
    return strcmp((*a)->name, (*b)->name);
}

size_t dep_graph_count(const dep_graph *graph) { return graph == NULL ? 0 : graph->count; }

const dep_node *dep_graph_at(const dep_graph *graph, size_t index) {
    if(graph == NULL || index >= graph->count)
        return NULL;
    return graph->nodes[index];
}

const dep_node *dep_graph_find(const dep_graph *graph, const char *name) {
    if(graph == NULL)
        return NULL;
    return str_map_get(graph->index, name);
}

void dep_graph_free(dep_graph *graph) {
    if(graph == NULL)
        return;
    for(size_t i = 0; i < graph->count; i++)
        node_free(graph->nodes[i]);
    free((void *)graph->nodes);
    /* The map borrows the nodes the array owns, so it frees no values. */
    str_map_destroy(graph->index);
    free(graph);
}

/* --- the lock file's `source` string --- */

/* One string that says both where a package came from and which fetcher goes
   back for it (RFC-0008). Composed here, once, so the lock writer has nothing
   to decide. */
static bool compose_source(const project_dep *dep, const char *registry_url, char *out,
                           size_t out_size, char *err, size_t err_size) {
    int written = 0;
    switch(dep->source) {
    case dep_source_version:
        written = snprintf(out, out_size, "registry+%s", registry_url);
        break;
    case dep_source_git:
        written = dep->reference[0] == '\0'
                      ? snprintf(out, out_size, "git+%s", dep->location)
                      : snprintf(out, out_size, "git+%s#%s", dep->location, dep->reference);
        break;
    case dep_source_path:
        written = snprintf(out, out_size, "path+%s", dep->location);
        break;
    case dep_source_archive:
        written = snprintf(out, out_size, "archive+%s", dep->location);
        break;
    }
    if(written < 0 || (size_t)written >= out_size)
        return set_error(err, err_size, "the source of '%s' is too long to record", dep->name);
    return true;
}

/* --- which registry answers for a dependency --- */

/* `[registries]` if the dependency named one, then whatever `molto login`
   stored, then the official one. */
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

/* --- reading the recipe a fetched source brings --- */

/* Both halves of a carried recipe come out of one parse: what to compile, and
   what it depends on in turn. Read through the same doc_view the registry's
   answer goes through, so the two cannot come to disagree. */
static bool read_carried_recipe(const char *root, const char *name, recipe_artifacts *artifacts,
                                project_deps *deps, char *err, size_t err_size) {
    char path[DEP_GRAPH_PATH_MAX];
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

    const doc_view view = doc_from_toml(doc);
    const bool ok = recipe_read_artifacts(view, artifacts, err, err_size) &&
                    project_deps_read_doc(view, deps, err, err_size);
    toml_free(doc);
    return ok;
}

/* --- visiting one dependency --- */

/* What a visit produces: everything the node keeps, plus the dependencies to
   walk next. Kept off the stack of the walk itself — a resolved_dep carries a
   whole recipe, and one per queue entry would be tens of kilobytes deep. */
typedef struct {
    char version[DEP_VERSION_MAX];
    char root[DEP_GRAPH_PATH_MAX];
    char checksum[SOURCE_DIGEST_MAX];
    recipe_artifacts artifacts;
    project_deps deps;
} visited;

static bool visit_registry(const project_ctx *ctx, const project_dep *dep, const credentials *creds,
                           visited *out, char *err, size_t err_size) {
    resolved_dep *resolved = calloc(1, sizeof *resolved);
    if(resolved == NULL)
        return set_error(err, err_size, "out of memory resolving dependencies");

    bool ok = resolve_version(registry_for(ctx, dep, creds), dep->name, dep->version, resolved, err,
                              err_size);
    if(ok && resolved->coordinate.form != recipe_form_source)
        ok = set_error(err, err_size,
                       "%s %s is published as a prebuilt artifact, and molto cannot consume one "
                       "yet",
                       dep->name, dep->version);
    if(ok)
        ok = source_fetch(&resolved->source, dep->name, dep->version, resolved->coordinate.target,
                          out->root, sizeof out->root, err, err_size);
    if(ok) {
        snprintf(out->version, sizeof out->version, "%s", dep->version);
        snprintf(out->checksum, sizeof out->checksum, "%s", resolved->source.sha256);
        out->artifacts = resolved->artifacts;
        out->deps = resolved->deps;
    }

    free(resolved);
    return ok;
}

static bool visit_carried(const project_dep *dep, visited *out, char *err, size_t err_size) {
    source_spec spec;
    if(!project_dep_to_source(dep, &spec, err, err_size))
        return false;

    /* A path dependency is used where it is, so it has no cache key and its
       directory is the source. */
    if(spec.origin == source_origin_path) {
        if(!source_fetch(&spec, dep->name, "", CARRIED_TARGET, out->root, sizeof out->root, err,
                         err_size))
            return false;
    } else {
        char key[SOURCE_DIGEST_MAX];
        if(!source_cache_key(&spec, key, sizeof key, err, err_size))
            return false;
        if(!source_fetch(&spec, dep->name, key, CARRIED_TARGET, out->root, sizeof out->root, err,
                         err_size))
            return false;
        snprintf(out->checksum, sizeof out->checksum, "%s", spec.sha256);
    }

    return read_carried_recipe(out->root, dep->name, &out->artifacts, &out->deps, err, err_size);
}

/* --- the walk --- */

/* Who required something, for a message. The root package has no name here
   because it is the one reading the message. */
static const char *requirer(const char *required_by) {
    return required_by[0] == '\0' ? "this project" : required_by;
}

/* Which version, or — for a source that has none — where it came from. A path
   dependency is identified by its directory and a git one by its URL, and
   saying "two versions" about two directories would name neither. */
static const char *identify(const char *version, const char *source) {
    return version[0] != '\0' ? version : source;
}

/* Two dependents named the same package and did not mean the same thing.
   There is no range to widen and no highest version to pick: RFC-0008 makes
   this the user's decision, so the message's whole job is to say who asked for
   what. */
static bool report_conflict(const dep_node *seen, const project_dep *dep, const char *source,
                            const char *required_by, char *err, size_t err_size) {
    return set_error(err, err_size,
                     "'%s' is required twice and not as the same thing: %s by %s, and %s by %s. "
                     "One package is one version in a build, so one of them has to change",
                     dep->name, identify(seen->version, seen->source), requirer(seen->required_by),
                     identify(dep->version, source), requirer(required_by));
}

/* One entry of the queue: a dependency still to visit, and who named it. The
   dependency is copied rather than pointed at, because the recipe it came out
   of is freed as soon as its parent has been visited. */
typedef struct {
    project_dep dep;
    char required_by[DEP_NAME_MAX];
} pending;

typedef struct {
    pending *items;
    size_t count;
    size_t head;
    size_t capacity;
} queue;

static bool queue_push(queue *q, const project_dep *dep, const char *required_by, char *err,
                       size_t err_size) {
    if(q->count == q->capacity) {
        const size_t grown = q->capacity == 0 ? 16 : q->capacity * 2;
        pending *items = realloc(q->items, grown * sizeof *items);
        if(items == NULL)
            return set_error(err, err_size, "out of memory resolving dependencies");
        q->items = items;
        q->capacity = grown;
    }
    q->items[q->count].dep = *dep;
    snprintf(q->items[q->count].required_by, DEP_NAME_MAX, "%s", required_by);
    q->count++;
    return true;
}

static bool enqueue_all(queue *q, const project_deps *deps, const char *required_by, char *err,
                        size_t err_size) {
    for(size_t i = 0; i < deps->count; i++) {
        if(!queue_push(q, &deps->items[i], required_by, err, err_size))
            return false;
    }
    return true;
}

/* A node's own edges, sorted, so the lock file it ends up in has a stable
   diff whatever order the recipe listed them in. */
static bool record_edges(dep_node *node, const project_deps *deps, char *err, size_t err_size) {
    for(size_t i = 0; i < deps->count; i++) {
        if(!str_list_push(&node->dependencies, deps->items[i].name))
            return set_error(err, err_size, "out of memory resolving dependencies");
    }
    str_list_sort(&node->dependencies);
    return true;
}

static bool visit_one(const project_ctx *ctx, const pending *entry, const credentials *creds,
                      dep_graph *graph, queue *q, char *err, size_t err_size) {
    const project_dep *dep = &entry->dep;

    char source[DEP_GRAPH_SOURCE_MAX];
    if(!compose_source(dep, registry_for(ctx, dep, creds), source, sizeof source, err, err_size))
        return false;

    /* A name already in the graph is the same package. Either it agrees, and
       the second arrival stops here — which is also what terminates a cycle —
       or it does not, and no build can contain both.

       Agreement is on the version *and* on the origin. Comparing versions
       alone would let two dependents point one name at two different
       directories and unify them silently, which is the same duplicate-symbol
       problem wearing a different hat — and the version of a path dependency
       is empty, so it would compare equal to anything. */
    const dep_node *seen = dep_graph_find(graph, dep->name);
    if(seen != NULL) {
        if(strcmp(seen->version, dep->version) != 0 || strcmp(seen->source, source) != 0)
            return report_conflict(seen, dep, source, entry->required_by, err, err_size);
        return true;
    }

    if(graph->count >= DEP_GRAPH_MAX_NODES)
        return set_error(err, err_size, "the dependency graph has more than %d packages",
                         DEP_GRAPH_MAX_NODES);

    visited *found = calloc(1, sizeof *found);
    if(found == NULL)
        return set_error(err, err_size, "out of memory resolving dependencies");

    char reason[512] = "";
    bool ok = dep->resolution == dep_resolution_registry
                  ? visit_registry(ctx, dep, creds, found, reason, sizeof reason)
                  : visit_carried(dep, found, reason, sizeof reason);
    if(!ok) {
        free(found);
        return entry->required_by[0] == '\0'
                   ? set_error(err, err_size, "dependency '%s': %s", dep->name, reason)
                   : set_error(err, err_size, "dependency '%s', required by '%s': %s", dep->name,
                               entry->required_by, reason);
    }

    dep_node *node = calloc(1, sizeof *node);
    if(node == NULL) {
        free(found);
        return set_error(err, err_size, "out of memory resolving dependencies");
    }
    str_list_init(&node->dependencies);
    snprintf(node->name, sizeof node->name, "%s", dep->name);
    snprintf(node->version, sizeof node->version, "%s", found->version);
    snprintf(node->root, sizeof node->root, "%s", found->root);
    snprintf(node->checksum, sizeof node->checksum, "%s", found->checksum);
    snprintf(node->required_by, sizeof node->required_by, "%s", entry->required_by);
    snprintf(node->source, sizeof node->source, "%s", source);
    node->artifacts = found->artifacts;

    ok = record_edges(node, &found->deps, err, err_size) &&
         enqueue_all(q, &found->deps, dep->name, err, err_size);
    if(ok)
        ok = graph_push(graph, node, err, err_size);
    if(!ok)
        node_free(node);

    free(found);
    return ok;
}

bool dep_graph_resolve(const project_ctx *ctx, dep_graph **out, char *err, size_t err_size) {
    *out = NULL;

    dep_graph *graph = calloc(1, sizeof *graph);
    if(graph == NULL)
        return set_error(err, err_size, "out of memory resolving dependencies");
    /* The array owns the nodes; the index only points at them. */
    graph->index = str_map_create(NULL);
    if(graph->index == NULL) {
        free(graph);
        return set_error(err, err_size, "out of memory resolving dependencies");
    }

    /* Reads need no token, so a machine that never ran `molto login` resolves
       against the official registry like any other. */
    credentials creds = {0};
    (void)credentials_load(&creds, NULL, 0);

    queue q = {0};
    bool ok = enqueue_all(&q, &ctx->deps, "", err, err_size);
    for(; ok && q.head < q.count; q.head++)
        ok = visit_one(ctx, &q.items[q.head], &creds, graph, &q, err, err_size);
    free(q.items);

    if(!ok) {
        dep_graph_free(graph);
        return false;
    }

    qsort((void *)graph->nodes, graph->count, sizeof *graph->nodes, by_name);
    *out = graph;
    return true;
}
