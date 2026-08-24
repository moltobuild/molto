#include <molto/commands/ir_command.h>

#include <molto/exit_code.h>
#include <molto/services/frontend_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/ir_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define IR_COMMAND_PATH_MAX 4096
#define DEFAULT_PROFILE "debug"

/* The directory this was run for.
 *
 * A project with a manifest is found by walking up, exactly as every other
 * command does (RFC-0004). A directory a plugin frontend understands has no
 * manifest to walk up to, so the working directory is the answer — and it is
 * only consulted after the walk fails, so a subdirectory of a molto project
 * still describes the project rather than the subdirectory. */
static bool locate(char *out, size_t size) {
    if(workspace_find_root(out, size))
        return true;
    return getcwd(out, size) != NULL;
}

/* Where the document goes. A file is written whole or not at all: a half-written
   document is not one, and leaving one behind would be a fixture that passes
   until somebody looks. */
static int write_document(const ir_document *doc, const char *output_path) {
    if(output_path == NULL)
        return ir_write(doc, stdout) ? exit_ok : exit_build_failure;

    FILE *file = fopen(output_path, "w");
    if(file == NULL) {
        fprintf(stderr, "molto: cannot write '%s'\n", output_path);
        return exit_build_failure;
    }
    const bool ok = ir_write(doc, file);
    const bool closed = fclose(file) == 0;
    if(!ok || !closed) {
        fprintf(stderr, "molto: the document could not be written to '%s'\n", output_path);
        (void)remove(output_path);
        return exit_build_failure;
    }
    return exit_ok;
}

int ir_command_run(const char *output_path, const char *profile) {
    char root[IR_COMMAND_PATH_MAX];
    if(!locate(root, sizeof root)) {
        fprintf(stderr, "molto: cannot tell which directory to describe\n");
        return exit_invalid_manifest;
    }

    const char *which = profile == NULL || profile[0] == '\0' ? DEFAULT_PROFILE : profile;

    ir_document doc;
    char err[1024] = "";
    const frontend_result result = frontend_run(root, which, &doc, err, sizeof err);

    switch(result) {
    case frontend_ok:
        break;
    case frontend_none:
        /* Not a plugin that broke: a directory nothing understands. Reported as
           an invalid manifest because that is what is missing — a description
           of a project — and a script telling "nothing here" from "a plugin
           misbehaved" is the whole reason exit codes are enumerated. */
        fprintf(stderr, "molto: %s\n", err[0] != '\0' ? err : "nothing here describes a project");
        ir_document_free(&doc);
        return exit_invalid_manifest;
    case frontend_bad_manifest:
        /* The native frontend read a manifest and refused it. That is the
           manifest's exit code and not a plugin's — nothing third-party ran. */
        fprintf(stderr, "molto: %s\n", err[0] != '\0' ? err : "the manifest is not valid");
        ir_document_free(&doc);
        return exit_invalid_manifest;
    case frontend_failed:
        fprintf(stderr, "molto: %s\n", err[0] != '\0' ? err : "the frontend failed");
        ir_document_free(&doc);
        return exit_plugin_failure;
    }

    const int code = write_document(&doc, output_path);
    ir_document_free(&doc);
    return code;
}
