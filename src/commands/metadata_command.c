#include <molto/commands/metadata_command.h>

#include <molto/build/sbom_cyclonedx.h>
#include <molto/cli.h>
#include <molto/exit_code.h>
#include <molto/project/project_ctx.h>
#include <molto/services/dep_graph.h>
#include <molto/services/fs_service.h>
#include <molto/services/sbom_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>

#define MANIFEST_FILENAME "Project.toml"

/* Size of the buffers holding the workspace root and the manifest path. */
#define PATH_BUFFER_SIZE 4096

/* Load the manifest of the project this was run from. Every command finds its
   project by walking up (RFC-0004), so this works from any subdirectory. */
static int load_project(project_ctx *out) {
    char root[PATH_BUFFER_SIZE];
    if(!workspace_find_root(root, sizeof root)) {
        fprintf(stderr, "molto: no " MANIFEST_FILENAME " found in this directory or above\n");
        return exit_invalid_manifest;
    }

    char manifest[PATH_BUFFER_SIZE];
    if(!fs_format_path(manifest, sizeof manifest, "%s/" MANIFEST_FILENAME, root)) {
        fprintf(stderr, "molto: the path to " MANIFEST_FILENAME " is too long\n");
        return exit_invalid_manifest;
    }

    char err[512] = "";
    if(!project_load(manifest, out, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_invalid_manifest;
    }
    return exit_ok;
}

/* Say what could not be verified, on stderr, where it does not become part of
   the document. A path dependency has no checksum because its bytes are
   whatever is on disk; the document records that per component, and this is so
   nobody has to read the document to find out. */
static void warn_about_unverified(const sbom_document *document) {
    const size_t unverified = sbom_unverified_count(document);
    if(unverified == 0)
        return;
    fprintf(stderr,
            "molto: warning: %zu of %zu components have no checksum to verify them against "
            "(a path dependency's bytes are whatever is on disk)\n",
            unverified, document->count);
}

static int write_document(const sbom_document *document, const char *output_path) {
    if(output_path == NULL) {
        sbom_write_cyclonedx(stdout, document, cli_version());
        return exit_ok;
    }

    FILE *file = fopen(output_path, "w");
    if(file == NULL) {
        fprintf(stderr, "molto: could not write '%s'\n", output_path);
        return exit_build_failure;
    }
    sbom_write_cyclonedx(file, document, cli_version());
    /* Checked, because a document truncated by a full disk is a document that
       still parses and describes a build that never happened. */
    if(fclose(file) != 0) {
        fprintf(stderr, "molto: could not finish writing '%s'\n", output_path);
        return exit_build_failure;
    }
    return exit_ok;
}

int metadata_command_run(const char *output_path, bool include_dev) {
    project_ctx ctx;
    const int loaded = load_project(&ctx);
    if(loaded != exit_ok)
        return loaded;

    /* The graph, not the lock file: the lock records versions, origins and
       checksums, and deliberately not licences (a fact that already lives in
       each recipe). Resolving is what reads them. */
    dep_graph *graph = NULL;
    char err[512] = "";
    if(!dep_graph_resolve(&ctx, &graph, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_dependency_failure;
    }

    sbom_document document;
    const sbom_options options = {.include_dev = include_dev};
    if(!sbom_collect(&ctx, graph, &options, &document)) {
        fprintf(stderr, "molto: out of memory collecting the bill of materials\n");
        dep_graph_free(graph);
        return exit_build_failure;
    }

    warn_about_unverified(&document);
    const int written = write_document(&document, output_path);

    sbom_document_free(&document);
    dep_graph_free(graph);
    return written;
}
