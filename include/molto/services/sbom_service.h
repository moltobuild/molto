#ifndef MOLTO_SBOM_SERVICE_H
#define MOLTO_SBOM_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/project_ctx.h>
#include <molto/services/dep_graph.h>
#include <molto/util/str_list.h>

/*
 * A resolved graph, reduced to what a bill of materials has to say about it.
 *
 * The graph already carries almost all of it — a name, an exact version, an
 * origin with its revision resolved, a checksum, a scope, the edges, and since
 * the publishing metadata landed, a licence (RFC-0009 `[about]`). What is left
 * is the part worth testing without a formatter in the way: which packages
 * belong in the document at all, what a missing checksum means, and the order
 * they come out in.
 *
 * Nothing here is copied. Every string points into the `project_ctx` and the
 * `dep_graph` it was collected from, and both have to outlive the document —
 * the same arrangement `prepared_unit` documents, and for the same reason: a
 * recipe_artifacts is fifteen kilobytes, and a document that copied one per
 * package would be half a megabyte of duplicated strings.
 */

typedef struct {
    const char *name;
    /* "" for a source that carries no version of its own: a path dependency,
       or a git one named by branch. */
    const char *version;
    /* From the recipe's `[about]`. "" throughout when it stated nothing, which
       is allowed and is not the same as a licence of "none". */
    const char *description;
    const char *license;
    const char *homepage;
    const char *repository;
    /* Lowercase hex, with any `sha256:` prefix already off, so a consumer gets
       the digest and not a spelling of it. "" when there is none. */
    const char *checksum;
    /* The lock file's origin string, verbatim: `registry+…`, `git+…#rev`,
       `path+…`. One string that answers both where it came from and how. */
    const char *source;
    /* Reaches the package's own binary, rather than only its tests. */
    bool ships;
    /* Has a checksum to verify it against. False is a path dependency, whose
       bytes are whatever is on disk — which is the point of one, and which a
       document that stays silent about it would be misrepresenting. */
    bool verified;
    /* The names it depends on, sorted. Borrowed from the node. */
    const str_list *dependencies;
} sbom_component;

typedef struct {
    /* Whether packages reachable only through `[dev-deps]` are included. A
       bill of materials describes what ships, so they are not by default. */
    bool include_dev;
} sbom_options;

typedef struct {
    /* The package the document is about. */
    const char *name;
    const char *version;
    const manifest_about *about;
    /* Its direct dependencies, sorted: the root's own edges. Owned by the
       document, because they are the one thing not already sitting in a
       node. */
    str_list root_dependencies;
    /* Sorted by name, because the graph already is. A document whose diff
       reorders itself between runs is one nobody reads. */
    sbom_component *components;
    size_t count;
} sbom_document;

/* Reduce `graph` to the document describing `ctx`.
 *
 * A package is left out when it does not reach the binary and `include_dev` is
 * false. That test is on the scope *bit* and not on equality: a package
 * required by `[deps]` and `[dev-deps]` alike is one node carrying both, and it
 * ships.
 *
 * False only on allocation failure. An empty graph is a valid document with no
 * components, which is what a package with no dependencies has. */
[[nodiscard]] bool sbom_collect(const project_ctx *ctx, const dep_graph *graph,
                                const sbom_options *options, sbom_document *out);

/* How many components carry no checksum. What a caller warns about: the
   document is still correct, and the part of it that cannot be verified should
   not have to be found by reading it. */
[[nodiscard]] size_t sbom_unverified_count(const sbom_document *document);

void sbom_document_free(sbom_document *document);

#endif /* MOLTO_SBOM_SERVICE_H */
