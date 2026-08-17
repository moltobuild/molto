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

/* A fetch the walk decided on and did not perform.
 *
 * RFC-0008 requires the whole graph to be walked over metadata before a byte
 * is downloaded, so that a conflict is a question asked during resolution
 * rather than an error discovered after paying for it. A registry dependency
 * makes that possible: the recipe carries its own `[deps]`, so what a version
 * needs is knowable without its sources.
 *
 * `node` is an index rather than a pointer because the array it points into is
 * grown while the walk runs.
 */
typedef struct {
    size_t node;
    source_spec spec;
    char name[DEP_NAME_MAX];
    char version[DEP_VERSION_MAX];
    char target[RECIPE_COORDINATE_MAX];
} deferred_fetch;

struct dep_graph {
    dep_node **nodes; /* owned, sorted by name once the walk is done */
    size_t count;
    size_t capacity;
    str_map *index; /* name -> dep_node*, borrowed; `nodes` owns them */
    /* What to fetch once the graph is known to be conflict-free. */
    deferred_fetch *fetches;
    size_t fetch_count;
    size_t fetch_capacity;
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

static bool listed(const str_list *list, const char *value) {
    for(size_t i = 0; i < str_list_count(list); i++) {
        if(strcmp(str_list_get(list, i), value) == 0)
            return true;
    }
    return false;
}

/* Append what `node` depends on and `out` has not seen, never `root` itself:
   a cycle back to the package being asked about is not part of its closure. */
static bool push_unseen(const dep_node *node, const char *root, str_list *out) {
    for(size_t i = 0; i < str_list_count(&node->dependencies); i++) {
        const char *name = str_list_get(&node->dependencies, i);
        if(strcmp(name, root) == 0 || listed(out, name))
            continue;
        if(!str_list_push(out, name))
            return false;
    }
    return true;
}

bool dep_graph_closure(const dep_graph *graph, const char *name, str_list *out) {
    const dep_node *root = dep_graph_find(graph, name);
    if(root == NULL)
        return true;

    /* `out` is the answer and the queue at once: everything below the cursor
       has been expanded, everything above it is waiting. A name is pushed once,
       which is what makes a cycle stop here instead of recursing. */
    const size_t base = str_list_count(out);
    if(!push_unseen(root, name, out))
        return false;
    for(size_t cursor = base; cursor < str_list_count(out); cursor++) {
        const dep_node *reached = dep_graph_find(graph, str_list_get(out, cursor));
        /* A name the graph does not carry cannot be expanded, and a partial
           graph is a resolution failure someone else already reported. */
        if(reached != NULL && !push_unseen(reached, name, out))
            return false;
    }
    return true;
}

void dep_graph_free(dep_graph *graph) {
    if(graph == NULL)
        return;
    for(size_t i = 0; i < graph->count; i++)
        node_free(graph->nodes[i]);
    free((void *)graph->nodes);
    free(graph->fetches);
    /* The map borrows the nodes the array owns, so it frees no values. */
    str_map_destroy(graph->index);
    free(graph);
}

/* --- the lock file's `source` string --- */

/* The scheme each kind of origin is written under. Named here rather than
   spelled into the format strings below, because reading one back is the same
   question as writing it and two spellings of it could disagree. */
#define SOURCE_SCHEME_REGISTRY "registry+"
#define SOURCE_SCHEME_GIT "git+"
#define SOURCE_SCHEME_PATH "path+"
#define SOURCE_SCHEME_ARCHIVE "archive+"

/* One string that says both where a package came from and which fetcher goes
   back for it (RFC-0008). Composed here, once, so the lock writer has nothing
   to decide. */
static bool compose_source(const project_dep *dep, const char *registry_url, char *out,
                           size_t out_size, char *err, size_t err_size) {
    int written = 0;
    switch(dep->source) {
    case dep_source_version:
        written = snprintf(out, out_size, SOURCE_SCHEME_REGISTRY "%s", registry_url);
        break;
    case dep_source_git:
        written =
            dep->reference[0] == '\0'
                ? snprintf(out, out_size, SOURCE_SCHEME_GIT "%s", dep->location)
                : snprintf(out, out_size, SOURCE_SCHEME_GIT "%s#%s", dep->location, dep->reference);
        break;
    case dep_source_path:
        written = snprintf(out, out_size, SOURCE_SCHEME_PATH "%s", dep->location);
        break;
    case dep_source_archive:
        written = snprintf(out, out_size, SOURCE_SCHEME_ARCHIVE "%s", dep->location);
        break;
    }
    if(written < 0 || (size_t)written >= out_size)
        return set_error(err, err_size, "the source of '%s' is too long to record", dep->name);
    return true;
}

dep_source dep_graph_source_kind(const char *source) {
    static const struct {
        const char *scheme;
        dep_source kind;
    } schemes[] = {
        {SOURCE_SCHEME_REGISTRY, dep_source_version},
        {SOURCE_SCHEME_GIT, dep_source_git},
        {SOURCE_SCHEME_PATH, dep_source_path},
        {SOURCE_SCHEME_ARCHIVE, dep_source_archive},
    };
    if(source != NULL) {
        for(size_t i = 0; i < sizeof schemes / sizeof schemes[0]; i++) {
            if(strncmp(source, schemes[i].scheme, strlen(schemes[i].scheme)) == 0)
                return schemes[i].kind;
        }
    }
    /* Anything else is treated as bytes nobody can go back for, which is what a
       path dependency is. Reporting it as a registry package would be the one
       wrong answer: that is the kind whose version and checksum are claims. */
    return dep_source_path;
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
                                project_deps *deps, manifest_about *about, char *err,
                                size_t err_size) {
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
                    project_deps_read_doc(view, deps, err, err_size) &&
                    manifest_read_about(view, "about", about, err, err_size);
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
    manifest_about about;
    /* Set for a registry dependency, whose bytes are not fetched during the
       walk. `root` is empty until they are. */
    bool deferred;
    source_spec spec;
    char target[RECIPE_COORDINATE_MAX];
} visited;

/* Everything a registry dependency contributes to the graph, and not one byte
   of its sources: the recipe already says what it depends on, so the walk can
   finish — and find its conflicts — before any of this is downloaded. */
static bool visit_registry(const project_ctx *ctx, const project_dep *dep, const credentials *creds,
                           visited *out, char *err, size_t err_size) {
    resolved_dep *resolved = calloc(1, sizeof *resolved);
    if(resolved == NULL)
        return set_error(err, err_size, "out of memory resolving dependencies");

    /* What the registry said last time, if it is still on disk. A published
       coordinate never changes (RFC-0010) and the manifest names an exact
       version, so there is nothing a fresh request could tell us that the
       remembered answer does not — and without this, every build of every
       project asks every registry to repeat itself. */
    bool ok = resolve_remembered(dep->name, dep->version, resolved) ||
              resolve_version(registry_for(ctx, dep, creds), dep->name, dep->version, resolved, err,
                              err_size);
    if(ok && resolved->coordinate.form != recipe_form_source)
        ok = set_error(err, err_size,
                       "%s %s is published as a prebuilt artifact, and molto cannot consume one "
                       "yet",
                       dep->name, dep->version);
    if(ok) {
        /* Kept now rather than after the fetch: the releases tree is not the
           directory a fetch replaces, so there is nothing to wait for. */
        if(resolved->body[0] != '\0')
            resolve_remember(dep->name, dep->version, resolved->body);
        snprintf(out->version, sizeof out->version, "%s", dep->version);
        snprintf(out->checksum, sizeof out->checksum, "%s", resolved->source.sha256);
        snprintf(out->target, sizeof out->target, "%s", resolved->coordinate.target);
        out->artifacts = resolved->artifacts;
        out->deps = resolved->deps;
        out->about = resolved->about;
        out->spec = resolved->source;
        out->deferred = true;
    }

    free(resolved);
    return ok;
}

/* A source that carries its own recipe has to be fetched to be read: its
   recipe *is* its bytes, so there is no metadata to walk ahead of them. This
   is the one place the walk still downloads, and the reason a conflict between
   two path or git dependencies is reported rather than searched. */
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

    return read_carried_recipe(out->root, dep->name, &out->artifacts, &out->deps, &out->about, err,
                               err_size);
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
                            const char *required_by, dep_conflict *conflict, char *err,
                            size_t err_size) {
    if(conflict != NULL) {
        snprintf(conflict->name, sizeof conflict->name, "%s", dep->name);
        snprintf(conflict->version, sizeof conflict->version, "%s",
                 identify(seen->version, seen->source));
        snprintf(conflict->required_by, sizeof conflict->required_by, "%s", seen->required_by);
        snprintf(conflict->other_version, sizeof conflict->other_version, "%s",
                 identify(dep->version, source));
        snprintf(conflict->other_required_by, sizeof conflict->other_required_by, "%s",
                 required_by);
    }
    return set_error(err, err_size,
                     "'%s' is required twice and not as the same thing: %s by %s, and %s by %s. "
                     "One package is one version in a build, so one of them has to change",
                     dep->name, identify(seen->version, seen->source), requirer(seen->required_by),
                     identify(dep->version, source), requirer(required_by));
}

/* Give `node` and everything under it a scope it did not have.
 *
 * Reachable by name alone, because the pass that created these nodes has
 * already finished: development dependencies are walked only after every
 * runtime one is in the graph, so a node found here has all of its own
 * children in the graph too. The scope bit doubles as the visited mark, which
 * is what stops a cycle.
 */
static void widen_scope(dep_graph *graph, dep_node *node, unsigned scope) {
    if((node->scope & scope) == scope)
        return;
    node->scope |= scope;
    for(size_t i = 0; i < str_list_count(&node->dependencies); i++) {
        dep_node *child = str_map_get(graph->index, str_list_get(&node->dependencies, i));
        if(child != NULL)
            widen_scope(graph, child, scope);
    }
}

/* One entry of the queue: a dependency still to visit, and who named it. The
   dependency is copied rather than pointed at, because the recipe it came out
   of is freed as soon as its parent has been visited. */
typedef struct {
    project_dep dep;
    char required_by[DEP_NAME_MAX];
    /* Inherited by everything this entry pulls in: a package reached only
       through a development dependency is itself only ever compiled into the
       test build. */
    unsigned scope;
} pending;

typedef struct {
    pending *items;
    size_t count;
    size_t head;
    size_t capacity;
} queue;

static bool queue_push(queue *q, const project_dep *dep, const char *required_by, unsigned scope,
                       char *err, size_t err_size) {
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
    q->items[q->count].scope = scope;
    q->count++;
    return true;
}

static bool enqueue_all(queue *q, const project_deps *deps, const char *required_by, unsigned scope,
                        char *err, size_t err_size) {
    for(size_t i = 0; i < deps->count; i++) {
        if(!queue_push(q, &deps->items[i], required_by, scope, err, err_size))
            return false;
    }
    return true;
}

/* --- the fetches the walk put off --- */

static bool defer_fetch(dep_graph *graph, size_t node, const char *name, const visited *found,
                        char *err, size_t err_size) {
    if(graph->fetch_count == graph->fetch_capacity) {
        const size_t grown = graph->fetch_capacity == 0 ? 8 : graph->fetch_capacity * 2;
        deferred_fetch *items = realloc(graph->fetches, grown * sizeof *items);
        if(items == NULL)
            return set_error(err, err_size, "out of memory resolving dependencies");
        graph->fetches = items;
        graph->fetch_capacity = grown;
    }

    deferred_fetch *entry = &graph->fetches[graph->fetch_count++];
    entry->node = node;
    entry->spec = found->spec;
    snprintf(entry->name, sizeof entry->name, "%s", name);
    snprintf(entry->version, sizeof entry->version, "%s", found->version);
    snprintf(entry->target, sizeof entry->target, "%s", found->target);
    return true;
}

/* Download what the resolution settled on.
 *
 * Runs after the whole graph is known and known to be conflict-free, which is
 * the point of putting it off: nothing is on disk for a version the user is
 * about to be asked to change. */
static bool materialize(dep_graph *graph, char *err, size_t err_size) {
    for(size_t i = 0; i < graph->fetch_count; i++) {
        const deferred_fetch *entry = &graph->fetches[i];
        dep_node *node = graph->nodes[entry->node];
        char reason[512] = "";

        if(!source_fetch(&entry->spec, entry->name, entry->version, entry->target, node->root,
                         sizeof node->root, reason, sizeof reason))
            return set_error(err, err_size, "dependency '%s': %s", entry->name, reason);
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
                      dep_graph *graph, queue *q, dep_conflict *conflict, char *err,
                      size_t err_size) {
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
    dep_node *seen = str_map_get(graph->index, dep->name);
    if(seen != NULL) {
        if(strcmp(seen->version, dep->version) != 0 || strcmp(seen->source, source) != 0)
            return report_conflict(seen, dep, source, entry->required_by, conflict, err, err_size);
        /* Required by both tables: one node, compiled once, reachable from both
           builds. Everything below it inherits the wider scope too, which is
           why this walks rather than just setting a flag. */
        widen_scope(graph, seen, entry->scope);
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
    node->scope = entry->scope;
    node->artifacts = found->artifacts;
    node->about = found->about;

    ok = record_edges(node, &found->deps, err, err_size) &&
         enqueue_all(q, &found->deps, dep->name, entry->scope, err, err_size);
    if(ok)
        ok = graph_push(graph, node, err, err_size);
    if(ok && found->deferred)
        ok = defer_fetch(graph, graph->count - 1, dep->name, found, err, err_size);
    else if(!ok)
        node_free(node);

    free(found);
    return ok;
}

/* --- the walk, over metadata --- */

static dep_graph *graph_create(char *err, size_t err_size) {
    dep_graph *graph = calloc(1, sizeof *graph);
    if(graph == NULL) {
        (void)set_error(err, err_size, "out of memory resolving dependencies");
        return NULL;
    }
    /* The array owns the nodes; the index only points at them. */
    graph->index = str_map_create(NULL);
    if(graph->index == NULL) {
        free(graph);
        (void)set_error(err, err_size, "out of memory resolving dependencies");
        return NULL;
    }
    return graph;
}

/* The root package's tables, with a proposal applied.
 *
 * Only the root's, and only for a registry dependency: what a search varies is
 * what the user could write down, and a version pinned anywhere else would be
 * a resolution nobody could reproduce by editing their manifest. */
static void apply_pin(project_deps *deps, const char *name, const char *version) {
    if(name == NULL || name[0] == '\0')
        return;
    for(size_t i = 0; i < deps->count; i++) {
        if(deps->items[i].resolution == dep_resolution_registry &&
           strcmp(deps->items[i].name, name) == 0)
            snprintf(deps->items[i].version, sizeof deps->items[i].version, "%s", version);
    }
}

/* Walk both tables and produce a graph, downloading nothing that a registry
   describes. `pin_name` moves one root dependency, for the search. */
static bool walk(const project_ctx *ctx, const credentials *creds, const char *pin_name,
                 const char *pin_version, dep_graph **out, dep_conflict *conflict, char *err,
                 size_t err_size) {
    *out = NULL;
    dep_graph *graph = graph_create(err, err_size);
    if(graph == NULL)
        return false;

    project_deps deps = ctx->deps;
    project_deps dev_deps = ctx->dev_deps;
    apply_pin(&deps, pin_name, pin_version);
    apply_pin(&dev_deps, pin_name, pin_version);

    /* Runtime first, so a package required by both is created as a runtime one
       and then widened. Development dependencies are only ever taken from the
       root package: a dependency's own are read and ignored, which is what
       keeps a library's test framework out of its consumer's build. */
    queue q = {0};
    bool ok = enqueue_all(&q, &deps, "", dep_scope_runtime, err, err_size);
    for(; ok && q.head < q.count; q.head++)
        ok = visit_one(ctx, &q.items[q.head], creds, graph, &q, conflict, err, err_size);

    /* Only now, with every runtime package in the graph, are the development
       ones walked. Doing them in one pass would let a node be created under the
       narrower scope while its children were still queued, and widening it
       later would not reach them. */
    ok = ok && enqueue_all(&q, &dev_deps, "", dep_scope_dev, err, err_size);
    for(; ok && q.head < q.count; q.head++)
        ok = visit_one(ctx, &q.items[q.head], creds, graph, &q, conflict, err, err_size);
    free(q.items);

    if(!ok) {
        dep_graph_free(graph);
        return false;
    }
    *out = graph;
    return true;
}

/* --- the search for a way out --- */

/* What the search is allowed to spend. The bounds are small on purpose: this
   runs while somebody waits, every step of it is a request, and a search that
   has to look at more than a handful of releases is one whose answer nobody
   would trust anyway. */
#define CONFLICT_MAX_PIVOTS 3
#define CONFLICT_MAX_CANDIDATES 8

/* The root dependency a name belongs to, so a proposal names something the
   manifest actually declares. Answers which table it was found in. */
static const project_dep *root_dep(const project_ctx *ctx, const char *name, const char **table) {
    for(size_t i = 0; i < ctx->deps.count; i++) {
        if(strcmp(ctx->deps.items[i].name, name) == 0) {
            *table = "deps";
            return &ctx->deps.items[i];
        }
    }
    for(size_t i = 0; i < ctx->dev_deps.count; i++) {
        if(strcmp(ctx->dev_deps.items[i].name, name) == 0) {
            *table = "dev-deps";
            return &ctx->dev_deps.items[i];
        }
    }
    return NULL;
}

/* Does moving `pivot` to `candidate` produce a graph with no conflict at all?
   Answered by walking, which costs requests and no downloads. */
static bool candidate_settles_it(const project_ctx *ctx, const credentials *creds,
                                 const char *pivot, const char *candidate, const char *conflicting,
                                 char *settles_on, size_t settles_size) {
    dep_graph *trial = NULL;
    char ignored[512] = "";
    if(!walk(ctx, creds, pivot, candidate, &trial, NULL, ignored, sizeof ignored))
        return false;

    const dep_node *node = dep_graph_find(trial, conflicting);
    const bool ok = node != NULL;
    if(ok)
        snprintf(settles_on, settles_size, "%s", node->version);
    dep_graph_free(trial);
    return ok;
}

/* Try every release of one root dependency, newest first, and stop at the
   first that removes the conflict.

   Never a downgrade: taking away a version somebody chose is not a proposal
   they can accept without thinking, and RFC-0008 asks for the newest
   combination that works. */
static bool search_pivot(const project_ctx *ctx, const credentials *creds, const char *pivot,
                         const dep_conflict *conflict, dep_conflict *out,
                         const dep_resolve_options *options, size_t *frame) {
    const char *table = NULL;
    const project_dep *declared = root_dep(ctx, pivot, &table);
    if(declared == NULL || declared->resolution != dep_resolution_registry)
        return false;

    str_list releases;
    str_list_init(&releases);
    char ignored[512] = "";
    if(options->watch != NULL)
        options->watch((*frame)++, options->watch_context);
    if(!resolve_versions(registry_for(ctx, declared, creds), pivot, &releases, ignored,
                         sizeof ignored)) {
        str_list_free(&releases);
        return false;
    }

    bool found = false;
    size_t tried = 0;
    for(size_t i = 0; i < str_list_count(&releases) && tried < CONFLICT_MAX_CANDIDATES && !found;
        i++) {
        const char *candidate = str_list_get(&releases, i);
        if(strcmp(candidate, declared->version) == 0)
            break; /* ordered newest first, so everything below is a downgrade */
        tried++;
        if(options->watch != NULL)
            options->watch((*frame)++, options->watch_context);

        char settles_on[DEP_VERSION_MAX] = "";
        if(!candidate_settles_it(ctx, creds, pivot, candidate, conflict->name, settles_on,
                                 sizeof settles_on))
            continue;

        found = true;
        out->has_proposal = true;
        snprintf(out->change_name, sizeof out->change_name, "%s", pivot);
        snprintf(out->change_from, sizeof out->change_from, "%s", declared->version);
        snprintf(out->change_to, sizeof out->change_to, "%s", candidate);
        snprintf(out->change_table, sizeof out->change_table, "%s", table);
        snprintf(out->settles_on, sizeof out->settles_on, "%s", settles_on);
    }

    str_list_free(&releases);
    return found;
}

/* Look for a version the user can write into Project.toml that removes the
   conflict. Not finding one is not an error: the message is the same minus the
   proposal, and there is always something the user can do by hand. */
static void search_proposal(const project_ctx *ctx, const credentials *creds,
                            dep_conflict *conflict, const dep_resolve_options *options) {
    /* Only what the manifest declares can be varied, so the pivots are the
       package itself and the two dependents that disagreed about it. */
    const char *pivots[CONFLICT_MAX_PIVOTS] = {conflict->name, conflict->required_by,
                                               conflict->other_required_by};
    size_t frame = 0;

    for(size_t i = 0; i < CONFLICT_MAX_PIVOTS; i++) {
        if(pivots[i] == NULL || pivots[i][0] == '\0')
            continue;
        bool repeated = false;
        for(size_t seen = 0; seen < i; seen++)
            repeated = repeated || strcmp(pivots[seen], pivots[i]) == 0;
        if(repeated)
            continue;
        if(search_pivot(ctx, creds, pivots[i], conflict, conflict, options, &frame))
            return;
    }
}

bool dep_graph_resolve_with(const project_ctx *ctx, const dep_resolve_options *options,
                            dep_graph **out, dep_conflict *conflict, char *err, size_t err_size) {
    static const dep_resolve_options none = {0};
    const dep_resolve_options *opts = options == NULL ? &none : options;
    *out = NULL;

    /* Reads need no token, so a machine that never ran `molto login` resolves
       against the official registry like any other. */
    credentials creds = {0};
    (void)credentials_load(&creds, NULL, 0);

    dep_graph *graph = NULL;
    if(!walk(ctx, &creds, NULL, NULL, &graph, conflict, err, err_size)) {
        if(opts->propose && conflict != NULL && conflict->name[0] != '\0')
            search_proposal(ctx, &creds, conflict, opts);
        return false;
    }

    /* Only now, with every version settled and nothing in conflict, are the
       sources brought down. Before the sort, because the deferred fetches name
       nodes by their position in the array. */
    if(!materialize(graph, err, err_size)) {
        dep_graph_free(graph);
        return false;
    }

    qsort((void *)graph->nodes, graph->count, sizeof *graph->nodes, by_name);
    *out = graph;
    return true;
}

bool dep_graph_resolve(const project_ctx *ctx, dep_graph **out, char *err, size_t err_size) {
    return dep_graph_resolve_with(ctx, NULL, out, NULL, err, err_size);
}
