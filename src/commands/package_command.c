#include <molto/services/package_service.h>

#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACKAGE_PATH_MAX 4096
#define TRACKED_OUTPUT_MAX (4 * 1024 * 1024)

static bool copy_tracked(const char *root, const char *destination, const str_list *kept,
                         uint64_t *bytes) {
    *bytes = 0;
    for(size_t i = 0; i < str_list_count(kept); i++) {
        char from[PACKAGE_PATH_MAX], to[PACKAGE_PATH_MAX], parent[PACKAGE_PATH_MAX];
        const char *file = str_list_get(kept, i);
        if(!fs_format_path(from, sizeof from, "%s/%s", root, file) ||
           !fs_format_path(to, sizeof to, "%s/%s", destination, file))
            return false;
        snprintf(parent, sizeof parent, "%s", to);
        char *slash = strrchr(parent, '/');
        if(!slash)
            return false;
        *slash = '\0';
        uint64_t size = 0;
        if(!fs_stamp(from, NULL, &size) || !fs_make_dirs(parent) || !fs_copy_file(from, to))
            return false;
        *bytes += size;
    }
    return true;
}

/* A tiny real consumer exercises the ordinary dependency build path. Nothing
   from the author's env, profiles, tests or plugins enters this workspace. */
static int build_consumer(const char *root, const char *package, const project_ctx *ctx) {
    char consumer[PACKAGE_PATH_MAX], path[PACKAGE_PATH_MAX], text[PACKAGE_PATH_MAX * 2];
    if(!fs_format_path(consumer, sizeof consumer, "%s/build/package/.consumer-%s-%s", root,
                       ctx->project_name, ctx->version) ||
       !fs_remove_tree(consumer) || !fs_format_path(path, sizeof path, "%s/src", consumer) ||
       !fs_make_dirs(path))
        return exit_build_failure;
    if(!fs_format_path(path, sizeof path, "%s/src/main.c", consumer) ||
       !fs_write_file(path, "int main(void) { return 0; }\n"))
        return exit_build_failure;
    /* TOML basic strings need escaping on unusual checkout paths. */
    char escaped[PACKAGE_PATH_MAX * 2];
    size_t used = 0;
    for(const char *p = package; *p; p++) {
        if(used + 2 >= sizeof escaped)
            return exit_build_failure;
        if(*p == '"' || *p == '\\')
            escaped[used++] = '\\';
        if(*p == '\n' || *p == '\r')
            return exit_invalid_manifest;
        escaped[used++] = *p;
    }
    escaped[used] = '\0';
    if(!fs_format_path(text, sizeof text,
                       "[package]\nname = \"package_check\"\nversion = \"0.1.0\"\n"
                       "[deps]\n%s = { path = \"%s\" }\n",
                       ctx->project_name, escaped) ||
       !fs_format_path(path, sizeof path, "%s/Project.toml", consumer) ||
       !fs_write_file(path, text))
        return exit_build_failure;
    return build_project(consumer, profile_debug, NULL, false, 0, NULL, 0);
}

int package_command_run(bool list_only) {
    char root[PACKAGE_PATH_MAX], err[2048] = "", destination[PACKAGE_PATH_MAX];
    if(!workspace_find_root(root, sizeof root))
        return exit_invalid_manifest;
    project_ctx *ctx = calloc(1, sizeof *ctx);
    recipe_artifacts *artifacts = calloc(1, sizeof *artifacts);
    if(!ctx || !artifacts) {
        free(ctx);
        free(artifacts);
        return exit_build_failure;
    }
    bool ok = package_read(root, NULL, ctx, artifacts, err, sizeof err);
    if(!ok) {
        fprintf(stderr, "molto: %s\n", err);
        free(ctx);
        free(artifacts);
        return exit_invalid_manifest;
    }
    fprintf(stderr, "molto: %s %s passes the dependency checklist\n", ctx->project_name,
            ctx->version);
    str_list files, kept;
    str_list_init(&files);
    str_list_init(&kept);
    char *output = calloc(1, TRACKED_OUTPUT_MAX);
    if(!output) {
        free(ctx);
        free(artifacts);
        return exit_build_failure;
    }
    const char *tracked[] = {"git", "-C", root, "ls-files", "-z", "--cached", NULL};
    process_spec spec = {.stdout_to = process_stream_capture,
                         .stderr_to = process_stream_inherit,
                         .capture = output,
                         .capture_size = TRACKED_OUTPUT_MAX};
    ok = process_execute(tracked, &spec) == 0 && !spec.truncated;
    for(size_t offset = 0; ok && offset < TRACKED_OUTPUT_MAX && output[offset];) {
        ok = str_list_push(&files, output + offset);
        offset += strlen(output + offset) + 1;
    }
    free(output);
    if(!ok)
        snprintf(err, sizeof err,
                 "could not inventory git-tracked files (or the inventory is too large)");
    if(ok)
        ok = package_select(ctx, &files, &kept, err, sizeof err);
    char changes[512] = "";
    const char *status[] = {"git", "-C", root, "status", "--porcelain", "--untracked-files=no",
                            NULL};
    if(ok && process_capture(status, changes, sizeof changes) == 0 && *changes)
        fprintf(stderr, "molto: warning: tracked files have uncommitted changes; a tag will not "
                        "contain them\n");
    uint64_t total = 0, bytes = 0;
    for(size_t i = 0; ok && i < str_list_count(&files); i++) {
        char path[PACKAGE_PATH_MAX];
        uint64_t size = 0;
        ok = fs_format_path(path, sizeof path, "%s/%s", root, str_list_get(&files, i)) &&
             fs_stamp(path, NULL, &size);
        total += size;
    }
    if(ok)
        ok = fs_format_path(destination, sizeof destination, "%s/build/package/%s-%s", root,
                            ctx->project_name, ctx->version) &&
             fs_remove_tree(destination) && fs_make_dirs(destination) &&
             copy_tracked(root, destination, &kept, &bytes);
    /* Validate the assembled copy too: ignored headers and untracked includes
       must fail before --list can claim the package is complete. */
    if(ok)
        ok = package_read(destination, NULL, ctx, artifacts, err, sizeof err);
    int result = ok ? exit_ok : exit_invalid_manifest;
    if(ok && list_only) {
        for(size_t i = 0; i < str_list_count(&kept); i++)
            puts(str_list_get(&kept, i));
    } else if(ok) {
        result = build_consumer(root, destination, ctx);
    }
    if(!ok)
        fprintf(stderr, "molto: %s\n", *err ? err : "could not assemble tracked package files");
    if(result == exit_ok) {
        fprintf(stderr,
                "molto: %s %s is ready to be a dependency\n"
                "  kept %zu files (%llu bytes), pruned %zu (%llu bytes)\n",
                ctx->project_name, ctx->version, str_list_count(&kept), (unsigned long long)bytes,
                str_list_count(&files) - str_list_count(&kept),
                (unsigned long long)(total - bytes));
        if(*ctx->about.repository)
            fprintf(stderr, "  consumers add: %s = { git = \"%s\", tag = \"v%s\" }\n",
                    ctx->project_name, ctx->about.repository, ctx->version);
    }
    str_list_free(&files);
    str_list_free(&kept);
    free(ctx);
    free(artifacts);
    return result;
}
