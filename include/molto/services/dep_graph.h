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
 * One resolved package.
 *
 * It carries both halves on purpose: what a build needs (`root`, `artifacts`)
 * and what a lock file records (`version`, `source`, `checksum`,
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
    /* The names it depends on, sorted, for the lock's `dependencies`. */
    str_list dependencies;
    recipe_artifacts artifacts;
} dep_node;

typedef struct dep_graph dep_graph;

/* Resolve every dependency `ctx` declares, and everything those declare in
   turn, fetching each into the shared cache.

   An empty `[deps]` succeeds with an empty graph and touches no network. A
   conflict, an unreachable registry or a source that brings no recipe fails
   with a reason naming the dependency and who required it. */
[[nodiscard]] bool dep_graph_resolve(const project_ctx *ctx, dep_graph **out, char *err,
                                     size_t err_size);

/* Nodes, sorted by name. The order is the lock file's, and it is sorted rather
   than resolution-ordered so the diff of a lock file is worth reading. */
[[nodiscard]] size_t dep_graph_count(const dep_graph *graph);
[[nodiscard]] const dep_node *dep_graph_at(const dep_graph *graph, size_t index);
[[nodiscard]] const dep_node *dep_graph_find(const dep_graph *graph, const char *name);

void dep_graph_free(dep_graph *graph);

#endif /* MOLTO_DEP_GRAPH_H */
