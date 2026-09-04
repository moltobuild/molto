#include "build_internal.h"

#include <molto/exit_code.h>
#include <molto/project/lockfile.h>
#include <molto/project/project_ctx.h>
#include <molto/services/conflict_prompt.h>
#include <molto/services/deps_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/host_service.h>
#include <molto/services/manifest_service.h>
#include <molto/util/progress.h>
#include <molto/workspace/wsdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Everything that has to be true before the first compiler runs: the manifest
 * read, the dependencies resolved, the lock taken and written.
 *
 * It is one file because it is one failure mode. Nothing here compiles
 * anything, and everything here can end the build before it starts — a
 * manifest that does not parse, a version nobody can satisfy, a lock another
 * molto is holding. Reading it end to end is how a person answers "why did
 * this not even begin", and that question used to be answered from four places
 * scattered through two thousand lines.
 */

/* Size of the buffer receiving a manifest parse-error message. */
#define MANIFEST_ERROR_SIZE 256

/* What the spinner says while the registry is being asked. */
#define RESOLVE_LABEL "asking the registry"

/* Load and parse `root/Project.toml` into a project context, reporting the
   detailed parse error to stderr on failure. */
int build_load_project(const char *root, project_ctx *out) {
    char manifest_path[PATH_BUFFER_SIZE];
    if(!fs_format_path(manifest_path, sizeof manifest_path, "%s/" MANIFEST_FILENAME, root)) {
        (void)fs_report_long_path(root);
        return exit_invalid_manifest;
    }
    if(!fs_path_exists(manifest_path)) {
        fprintf(stderr, "molto: no " MANIFEST_FILENAME " in '%s'\n", root);
        return exit_invalid_manifest;
    }
    char err[MANIFEST_ERROR_SIZE] = "";
    if(!project_load(manifest_path, out, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err[0] != '\0' ? err : "invalid manifest");
        return exit_invalid_manifest;
    }
    return exit_ok;
}

/* Close the workspace database, saying so if the incremental state could not be
   persisted. The build itself still stands; the next one just will not be
   incremental, and silence there would look like a mysterious full rebuild. */
void build_warn_if_not_saved(wsdb *db) {
    if(!wsdb_close(db))
        fprintf(stderr, "molto: warning: could not save the workspace database; "
                        "the next build will not be incremental\n");
}

/* On stderr, next to the diagnostics it is interleaved with, and only when a
   person is there to see it. */
static void watch_registry(size_t frame, void *context) {
    progress_line *line = context;
    if(!progress_is_interactive(stderr))
        return;
    spinner_wait(stderr, RESOLVE_LABEL, frame);
    line->drawn = true;
}

/* Resolve, and when two exact versions disagree, look for a way out and ask.
 *
 * At most one retry. Accepting a proposal rewrites `Project.toml`, so the
 * manifest is reloaded and resolved again — and if that resolution conflicts
 * too, it is reported rather than turned into another question: a manifest
 * that can conflict twice is one the user should look at rather than be walked
 * through one prompt at a time.
 */
[[nodiscard]] static bool resolve_or_ask(const char *root, project_ctx *ctx, dep_graph **out,
                                         char *err, size_t err_size) {
    progress_line line = {0};
    const dep_resolve_options options = {
        .propose = true, .watch = watch_registry, .watch_context = &line};

    for(int attempt = 0; attempt < 2; attempt++) {
        dep_conflict conflict = {0};
        const bool resolved = dep_graph_resolve_with(ctx, &options, out, &conflict, err, err_size);
        /* Before anything else is printed: half a spinner in front of a
           message reads as part of the message. */
        progress_line_clear(stderr, &line);
        if(resolved)
            return true;

        /* An empty name means the resolution failed for an ordinary reason —
           an unreachable registry, a recipe that does not parse — and those
           are the caller's to report. */
        if(conflict.name[0] == '\0' || attempt > 0)
            return false;
        if(!conflict_prompt_apply(root, &conflict)) {
            /* The conflict and the proposal are already on stderr; this is the
               one line the caller prints. */
            snprintf(err, err_size,
                     "'%s' is required at two versions and nothing chose between them",
                     conflict.name);
            return false;
        }

        /* The manifest on disk is no longer the one in hand. */
        project_ctx reloaded;
        if(build_load_project(root, &reloaded) != exit_ok) {
            snprintf(err, err_size, "Project.toml could not be read back after the edit");
            return false;
        }
        *ctx = reloaded;
    }
    return false;
}

/* What the host answered, in the shape the lock records (RFC-0016). */
[[nodiscard]] static bool collect_host_locks(const project_ctx *ctx, lock_host **out, size_t *count,
                                             char *err, size_t err_size) {
    *out = NULL;
    *count = 0;
    if(ctx->target.host_count == 0)
        return true;

    host_answer *answers = (host_answer *)calloc(ctx->target.host_count, sizeof *answers);
    lock_host *locks = (lock_host *)calloc(ctx->target.host_count, sizeof *locks);
    if(answers == NULL || locks == NULL) {
        free(answers);
        free(locks);
        snprintf(err, err_size, "out of memory recording what the host answered");
        return false;
    }

    if(!host_resolve_all(&ctx->target, answers, err, err_size)) {
        free(answers);
        free(locks);
        return false;
    }

    for(size_t i = 0; i < ctx->target.host_count; i++) {
        snprintf(locks[i].capability, sizeof locks[i].capability, "%s", ctx->target.host[i]);
        snprintf(locks[i].answered, sizeof locks[i].answered, "%s", HOST_RESOLVER_NAME);
        snprintf(locks[i].version, sizeof locks[i].version, "%s", answers[i].version);
    }
    free(answers);
    *out = locks;
    *count = ctx->target.host_count;
    return true;
}

[[nodiscard]] bool build_prepare_and_lock(const char *root, project_ctx *ctx, prepared_deps *out,
                                          prepared_deps *dev_out, char *err, size_t err_size) {
    if(ctx->deps.count == 0 && ctx->dev_deps.count == 0 && ctx->target.host_count == 0)
        return true;

    /* Read before resolving, so what comes back can be held against it. A
       missing or stale lock is not an error: there is simply nothing to check
       against, and one is written at the end either way. */
    lockfile lock;
    char lock_err[512] = "";
    const bool locked =
        lockfile_read(root, &lock, lock_err, sizeof lock_err) && lockfile_matches(&lock, ctx);

    lock_host *hosts = NULL;
    size_t host_count = 0;
    if(!collect_host_locks(ctx, &hosts, &host_count, err, err_size)) {
        if(lock.packages != NULL || lock.hosts != NULL)
            lockfile_free(&lock);
        return false;
    }
    /* Reported before anything is built and only when the lock still describes
       this manifest: on a stale one the answer is being re-recorded anyway, and
       a note about a file about to be rewritten is noise. */
    if(locked) {
        (void)lockfile_report_host_drift(&lock, hosts, host_count);
        /* And what it recorded is kept rather than replaced with this machine's
           answer. A record that rewrites itself on every build is not a record:
           two developers on different distributions would flip the file back
           and forth in every commit, and the diff that was supposed to make a
           difference visible would become the noise nobody reads. A capability
           the lock has never seen is recorded now; one it has is left alone,
           and the note above is what says this machine disagrees. */
        for(size_t i = 0; i < host_count; i++) {
            for(size_t j = 0; j < lock.host_count; j++) {
                if(strcmp(lock.hosts[j].capability, hosts[i].capability) != 0)
                    continue;
                snprintf(hosts[i].version, sizeof hosts[i].version, "%s", lock.hosts[j].version);
                break;
            }
        }
    }

    dep_graph *graph = NULL;
    if(ctx->deps.count + ctx->dev_deps.count > 0 &&
       !resolve_or_ask(root, ctx, &graph, err, err_size)) {
        if(lock.packages != NULL || lock.hosts != NULL)
            lockfile_free(&lock);
        free(hosts);
        return false;
    }

    bool ok = !locked || lockfile_verify(&lock, graph, err, err_size);
    if(lock.packages != NULL || lock.hosts != NULL)
        lockfile_free(&lock);
    if(!ok) {
        dep_graph_free(graph);
        free(hosts);
        return false;
    }

    ok = deps_prepare_graph(graph, out, err, err_size) &&
         deps_prepare_dev(graph, dev_out, err, err_size);
    /* Failing to record a resolution that succeeded must not fail the build:
       the objects are correct either way, and the cost is that the next build
       resolves again. Silence would be worse — a lock file nobody can write is
       a reproducibility guarantee nobody has. */
    if(ok && !lockfile_write(root, ctx->project_name, graph, hosts, host_count, err, err_size)) {
        fprintf(stderr, "molto: warning: %s\n", err);
        err[0] = '\0';
    }

    free(hosts);
    dep_graph_free(graph);
    return ok;
}
