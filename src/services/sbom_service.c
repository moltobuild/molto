#include <molto/services/sbom_service.h>

#include <stdlib.h>
#include <string.h>

/* What a digest may be written with in front of it. RFC-0008 spells a lock
   file's checksum `sha256:<64 hex>`, and a source_spec carries the bare hex; a
   document that served one form sometimes and the other form otherwise would
   make every consumer strip it themselves. */
#define DIGEST_PREFIX "sha256:"

/* The digest without whatever names the algorithm, borrowed rather than
   copied: the tail of a string is a pointer into it. */
static const char *bare_digest(const char *checksum) {
    const size_t length = strlen(DIGEST_PREFIX);
    if(strncmp(checksum, DIGEST_PREFIX, length) == 0)
        return checksum + length;
    return checksum;
}

/* Whether the package reaches the binary rather than only the tests.
 *
 * On the bit, never on equality. A package required by `[deps]` and
 * `[dev-deps]` alike is one node carrying both scopes (RFC-0008), and reading
 * that as "it is a dev dependency" would drop a library the binary genuinely
 * links. */
static bool ships(const dep_node *node) { return (node->scope & dep_scope_runtime) != 0; }

static void fill(sbom_component *out, const dep_node *node) {
    out->name = node->name;
    out->version = node->version;
    out->description = node->about.description;
    out->license = node->about.license;
    out->homepage = node->about.homepage;
    out->repository = node->about.repository;
    out->checksum = bare_digest(node->checksum);
    out->source = node->source;
    out->ships = ships(node);
    /* A path dependency has none, because its bytes are whatever is on disk.
       The document says so rather than implying a verification that did not
       happen. */
    out->verified = node->checksum[0] != '\0';
    out->dependencies = &node->dependencies;
}

/* The names the manifest itself declared, sorted.
 *
 * Taken from the manifest rather than from the graph because they are the only
 * edges no node records: every other package's dependents are written down in
 * the node that required them, and the root package has no node. */
static bool collect_root_edges(const project_ctx *ctx, const sbom_options *options, str_list *out) {
    for(size_t i = 0; i < ctx->deps.count; i++) {
        if(!str_list_push(out, ctx->deps.items[i].name))
            return false;
    }
    if(options->include_dev) {
        for(size_t i = 0; i < ctx->dev_deps.count; i++) {
            if(!str_list_push(out, ctx->dev_deps.items[i].name))
                return false;
        }
    }
    str_list_sort(out);
    return true;
}

bool sbom_collect(const project_ctx *ctx, const dep_graph *graph, const sbom_options *options,
                  sbom_document *out) {
    memset(out, 0, sizeof *out);
    out->name = ctx->project_name;
    out->version = ctx->version;
    out->about = &ctx->about;
    str_list_init(&out->root_dependencies);

    if(!collect_root_edges(ctx, options, &out->root_dependencies)) {
        sbom_document_free(out);
        return false;
    }

    const size_t total = dep_graph_count(graph);
    if(total > 0) {
        out->components = calloc(total, sizeof *out->components);
        if(out->components == NULL) {
            sbom_document_free(out);
            return false;
        }
    }

    /* In graph order, which is sorted by name, so the document's diff is worth
       reading for the same reason the lock file's is. */
    for(size_t i = 0; i < total; i++) {
        const dep_node *node = dep_graph_at(graph, i);
        if(!options->include_dev && !ships(node))
            continue;
        fill(&out->components[out->count++], node);
    }
    return true;
}

size_t sbom_unverified_count(const sbom_document *document) {
    size_t total = 0;
    for(size_t i = 0; i < document->count; i++)
        total += document->components[i].verified ? 0 : 1;
    return total;
}

void sbom_document_free(sbom_document *document) {
    str_list_free(&document->root_dependencies);
    free(document->components);
    document->components = NULL;
    document->count = 0;
}
