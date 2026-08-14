#ifndef MOLTO_DEP_GRAPH_H
#define MOLTO_DEP_GRAPH_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/project_ctx.h>
#include <molto/services/recipe_service.h>
#include <molto/util/str_list.h>

/*
 * The whole dependency graph, not just what the manifest wrote down.
 *
 * A manifest names its direct dependencies; each of those names its own, in the
 * recipe it was published with. Walking that is what turns four lines of
 * `[deps]` into the set of things a build actually compiles — and it is where
 * the two rules of RFC-0008 are enforced, because neither can be checked one
 * dependency at a time.
 *
 * **One version per name.** The C linker has one flat namespace, so two copies
 * of a library in one link are duplicate symbols or a silent ODR violation.
 * Versions are exact (RFC-0008), so unification is equality: either every
 * dependent named the same version or the graph is refused, naming both.
 *
 * **A name is visited once.** A package reached twice is the same package, and
 * the second arrival stops there. That also settles cycles without a special
 * case: `a` depending on `b` depending on `a` terminates, and it is not an
 * error — with `type = "source"` everything lands in one binary, so a cycle
 * between two drops describes a build that works.
 */

/* Longest path to a fetched source this walker composes. */
#define DEP_GRAPH_PATH_MAX 1024

/* A lock file's `source` string: a scheme prefix and an origin (RFC-0008). */
#define DEP_GRAPH_SOURCE_MAX (SOURCE_URL_MAX + 64)

/*
 * Which build a package belongs to.
 *
 * A flag set rather than a choice: the same package can be required by
 * `[deps]` and by `[dev-deps]`, and then it is both — one node, one version,
 * reachable from either build.
 */
typedef enum {
    dep_scope_runtime = 1 << 0, /* linked into the package's own binary */
    dep_scope_dev = 1 << 1,     /* only ever reaches the test build */
} dep_scope;

/*
 * One resolved package.
 *
 * It carries both halves on purpose: what a build needs (`root`, `artifacts`)
 * and what a lock file records (`version`, `source`, `checksum`, `scope`,
 * `dependencies`). Splitting them would mean walking the graph twice, and the
 * second walk is the one that goes to the network.
 */
typedef struct {
    char name[DEP_NAME_MAX];
    /* The exact version, or "" for a source that carries no version of its
       own: a path dependency, or a git one named by branch. */
    char version[DEP_VERSION_MAX];
    /* Where the fetched source lives, absolute. */
    char root[DEP_GRAPH_PATH_MAX];
    /* Prefixed origin, ready for the lock: "registry+…", "git+…#rev",
       "path+…", "archive+…". */
    char source[DEP_GRAPH_SOURCE_MAX];
    /* "" when the origin cannot be digested — a path dependency's bytes are
       whatever is on disk, which is the point of one. */
    char checksum[SOURCE_DIGEST_MAX];
    /* Who pulled it in, for the message a conflict prints. "" when the root
       package named it directly. */
    char required_by[DEP_NAME_MAX];
    /* One or both of dep_scope. A package required by both tables carries
       both, and is compiled once. */
    unsigned scope;
    /* The names it depends on, sorted, for the lock's `dependencies`. */
    str_list dependencies;
    recipe_artifacts artifacts;
} dep_node;

typedef struct dep_graph dep_graph;

/* Resolve every dependency `ctx` declares in `[deps]` and `[dev-deps]`, and
   everything those declare in turn, fetching each into the shared cache.

   The two tables share one graph and one version per name. They have to: a
   test binary links the objects of `src/`, compiled against the runtime
   version, together with the objects of `tests/`. Two versions of one library
   in that link are duplicate symbols or a silent ODR violation, which is the
   same reason `[deps]` alone allows only one (RFC-0008).

   Development dependencies are **not transitive**: only the root package's are
   walked, and a dependency's own `[dev-deps]` is never read. Otherwise adding
   one small library would drag in its test framework and its mocks, none of
   which will ever be compiled.

   Empty tables succeed with an empty graph and touch no network. A conflict, an
   unreachable registry or a source that brings no recipe fails with a reason
   naming the dependency and who required it. */
[[nodiscard]] bool dep_graph_resolve(const project_ctx *ctx, dep_graph **out, char *err,
                                     size_t err_size);

/*
 * A name two dependents wanted at two versions, and the way out if there is
 * one.
 *
 * There is no interval to intersect here and no highest version to take:
 * exact versions either agree or they do not (RFC-0008). What replaces the
 * arithmetic is a search and then a question, and this struct is what the
 * question is asked from.
 */
typedef struct {
    /* The package in conflict. Empty when there is no conflict, which is how a
       caller tells one apart from an unreachable registry. */
    char name[DEP_NAME_MAX];
    /* The two claims, and who made each. `required_by` is "" when the root
       package named it directly. */
    char version[DEP_VERSION_MAX];
    char required_by[DEP_NAME_MAX];
    char other_version[DEP_VERSION_MAX];
    char other_required_by[DEP_NAME_MAX];

    /* Filled by the search when it found a change the user can write down. */
    bool has_proposal;
    /* The root dependency to move, and where to. */
    char change_name[DEP_NAME_MAX];
    char change_from[DEP_VERSION_MAX];
    char change_to[DEP_VERSION_MAX];
    /* Which table it is declared in: "deps" or "dev-deps". */
    char change_table[16];
    /* The version the conflicting package settles on once it is applied. */
    char settles_on[DEP_VERSION_MAX];
} dep_conflict;

/* How much noise the resolution is allowed to make, and how hard it tries. */
typedef struct {
    /* Search for a version that removes a conflict, instead of only reporting
       it. Costs requests, never downloads. */
    bool propose;
    /* Drawn once per registry request, so a search that takes a while says so.
       NULL draws nothing, which is what a test and a pipe both want. */
    void (*watch)(size_t frame, void *context);
    void *watch_context;
} dep_resolve_options;

/* As `dep_graph_resolve`, with a say in how hard it tries and how loudly.
 *
 * `conflict` may be NULL. When it is not and the resolution failed, a
 * non-empty `conflict->name` means the failure was a conflict rather than a
 * broken registry — and `has_proposal` says whether there is something to
 * offer the user. `err` is filled either way, so a caller that only prints
 * needs no extra branch. */
[[nodiscard]] bool dep_graph_resolve_with(const project_ctx *ctx,
                                          const dep_resolve_options *options, dep_graph **out,
                                          dep_conflict *conflict, char *err, size_t err_size);

/* Nodes, sorted by name. The order is the lock file's, and it is sorted rather
   than resolution-ordered so the diff of a lock file is worth reading. */
[[nodiscard]] size_t dep_graph_count(const dep_graph *graph);
[[nodiscard]] const dep_node *dep_graph_at(const dep_graph *graph, size_t index);
[[nodiscard]] const dep_node *dep_graph_find(const dep_graph *graph, const char *name);

void dep_graph_free(dep_graph *graph);

#endif /* MOLTO_DEP_GRAPH_H */
