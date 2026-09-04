#include <molto/services/build_service.h>

#include "build_internal.h"

#include <molto/build/compile_db.h>
#include <molto/build/library.h>
#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/exit_code.h>
#include <molto/services/deps_service.h>
#include <molto/services/frontend_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/host_service.h>
#include <molto/services/ir_transform.h>
#include <molto/services/object_cache.h>
#include <molto/services/source_discovery.h>
#include <molto/services/source_service.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/str_list.h>
#include <molto/workspace/wsdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t project_env_to_vars(const project_env *env, process_env_var *vars, size_t capacity) {
    if(env == NULL)
        return 0;
    size_t count = env->count < capacity ? env->count : capacity;
    for(size_t i = 0; i < count; i++) {
        vars[i].name = env->names[i];
        vars[i].value = env->values[i];
    }
    return count;
}

size_t project_env_fingerprint(const project_env *env, char *out, size_t size) {
    if(size == 0)
        return 0;
    out[0] = '\0';
    if(env == NULL || env->count == 0)
        return 0;

    size_t used = 0;
    for(size_t i = 0; i < env->count; i++) {
        int written = snprintf(out + used, size - used, "%s%s=%s",
                               i > 0 ? OBJECT_CACHE_ENV_MARK : "", env->names[i], env->values[i]);
        if(written < 0 || (size_t)written >= size - used) {
            /* The caller sized the buffer from the same limits read_env
               enforces, so a short write means those two have drifted apart.
               Saying nothing would quietly fingerprint an environment as if it
               were empty; saying half of it would be worse. */
            out[0] = '\0';
            return 0;
        }
        used += (size_t)written;
    }
    return used;
}

/*
 * Resolve the whole graph, reduce it to flags, and write down what was
 * resolved.
 *
 * One walk answers both questions, which is why the lock is written here and
 * not by a command of its own: the graph is in hand exactly once, and going
 * back for it would mean asking the registry the same thing twice.
 *
 * A project with no dependencies writes no lock file. There is nothing to
 * record, and creating one would put a file in every project that has never
 * needed one — molto's own repository included.
 */

/*
 * The sources of one kind of target, as the absolute paths the engine compiles.
 *
 * This is the seam RFC-0015 names. What a build compiles now comes from the
 * document rather than from a walk of the filesystem, which is what makes
 * `Project.toml` reach the engine by the road a plugin's answer will travel —
 * and what makes every `molto build` a test of the frontend.
 *
 * The paths are anchored back at the root on the way through: a document says
 * them relative to it, which is what makes it diffable between machines and
 * what makes it wrong to hand straight to a compiler.
 *
 * Concatenating the test targets in document order reproduces the flat list
 * `molto test` used to walk for itself, because the frontend fills them from
 * that same sorted list — one target per file, in order.
 */
[[nodiscard]] bool build_document_sources(const ir_document *doc, const char *root,
                                          doc_target_set set, str_list *out) {
    for(size_t t = 0; t < doc->target_count; t++) {
        const ir_target *target = &doc->targets[t];
        if(!build_in_set(doc, target, set))
            continue;
        const char *base = build_target_root(doc, target, root);
        for(size_t i = 0; i < target->source_count; i++) {
            const char *relative = target->sources[i].path;
            char path[PATH_BUFFER_SIZE];
            if(!fs_format_path(path, sizeof path, "%s/%s", base, relative))
                return fs_report_long_path(relative);
            if(!str_list_push(out, path))
                return false;
        }
    }
    return true;
}

/* Hold the document to the one path rule, once every transform has spoken.
 *
 * RFC-0013 applies it to every document whatever its origin, and this is the
 * native half of that: `Project.toml` is reviewable, but a path in it still gets
 * to name only somewhere Molto may read from or write to.
 *
 * The fourth bound is what makes the rule fit a real project. A dependency's
 * directory is not the workspace, not the build directory and not always the
 * cache — `{ path = "../greet" }` is a sibling checkout — and it is authorised
 * by the manifest the user wrote. The roots come from `resolve` and never from
 * the document, so nothing a producer writes can widen what it is held to.
 *
 * False with a message in `err`. */
[[nodiscard]] static bool document_is_allowed(const ir_document *doc, const char *root,
                                              build_profile profile, const char *platform,
                                              const project_ctx *ctx, const prepared_deps *deps,
                                              const prepared_deps *dev, char *err,
                                              size_t err_size) {
    char segment[PATH_BUFFER_SIZE];
    char build_dir[PATH_BUFFER_SIZE];
    if(!build_segment(profile, platform, segment, sizeof segment) ||
       !fs_format_path(build_dir, sizeof build_dir, "%s/" DIR_BUILD "/%s", root, segment))
        return fs_report_long_path(root);

    char cache[PATH_BUFFER_SIZE];
    const bool has_cache = source_cache_root(cache, sizeof cache);

    /* Taken from what `resolve` returned and not from `doc->dependencies`, even
       though a transform just wrote the same strings into it. Reading them back
       off the document would let anything that can write a `Dependency` node
       widen the bounds it is then held to, which is a check that checks
       nothing. Here the list cannot be influenced by the document at all. */
    /* A host library's directories are a bound too, and they are resolved here
       rather than read off the document for exactly the reason above: the
       frontend put them there, and a bound taken from what it wrote would be a
       bound it chose. Asked again of the same resolver, from the manifest —
       pkg-config is deterministic within a build, so the two agree, and the
       cost is a process rather than a compromise. */
    host_answer *host = NULL;
    size_t host_dirs = 0;
    if(ctx->target.host_count > 0) {
        host = (host_answer *)calloc(ctx->target.host_count, sizeof *host);
        if(host == NULL) {
            snprintf(err, err_size, "out of memory checking the document's paths");
            return false;
        }
        if(!host_resolve_all(&ctx->target, host, err, err_size)) {
            free(host);
            return false;
        }
        for(size_t i = 0; i < ctx->target.host_count; i++)
            host_dirs += host[i].include_count;
    }

    const size_t count = deps->unit_count + dev->unit_count + host_dirs;
    const char **roots = NULL;
    if(count > 0) {
        roots = (const char **)calloc(count, sizeof *roots);
        if(roots == NULL) {
            snprintf(err, err_size, "out of memory checking the document's paths");
            free(host);
            return false;
        }
        size_t at = 0;
        for(size_t i = 0; i < deps->unit_count; i++)
            roots[at++] = deps->units[i].root;
        for(size_t i = 0; i < dev->unit_count; i++)
            roots[at++] = dev->units[i].root;
        for(size_t i = 0; i < ctx->target.host_count; i++) {
            for(size_t j = 0; j < host[i].include_count; j++)
                roots[at++] = host[i].includes[j];
        }
    }

    const ir_bounds bounds = {.workspace = root,
                              .build_dir = build_dir,
                              .cache = has_cache ? cache : NULL,
                              .roots = roots,
                              .root_count = count};
    const bool ok = ir_validate(doc, &bounds, err, err_size);
    free((void *)roots);
    free(host);
    return ok;
}

/* What a frontend's refusal costs the process.
 *
 * The same mapping `molto ir` applies, and deliberately the same: the two
 * commands ask one question of one service, and a script telling "my manifest
 * is wrong" from "a third-party binary misbehaved" must not depend on which of
 * them it ran. `frontend_none` is an invalid manifest rather than a build
 * failure because what is missing is a description of a project, not code that
 * compiles — RFC-0002 enumerates the codes so that difference survives. */
static int frontend_exit_code(frontend_result answer) {
    switch(answer) {
    case frontend_ok:
        return exit_ok;
    case frontend_none:
    case frontend_bad_manifest:
        return exit_invalid_manifest;
    case frontend_failed:
        return exit_plugin_failure;
    }
    return exit_build_failure;
}

/* Load the manifest, resolve what it depends on, and work out every unit the
   project's own build would compile — without compiling any of them. `objects`
   is caller-initialised and caller-freed; everything the units borrow belongs
   to `plan`. Shared by build_project and build_tests. */
[[nodiscard]] int build_plan_project(const char *root, build_profile profile, const char *platform,
                                     bool refresh_toolchain, wsdb *db, const pass_options *options,
                                     project_ctx *ctx_out, resolved_toolchain *chain_out,
                                     str_list *objects_out, build_plan *plan) {
    int result = build_load_project(root, ctx_out);
    if(result == exit_ok && platform != NULL && ctx_out->target.host_count > 0) {
        /*
         * Refused rather than answered wrongly.
         *
         * `[target].host` asks pkg-config where a library is, and pkg-config
         * answers for the machine it runs on. Cross-compiling, that answer
         * names this host's headers and this host's `-l`, which would be
         * compiled into a binary for somewhere else — a build that succeeds and
         * produces something that cannot link there, or worse, links against
         * the wrong ABI and starts.
         *
         * Answering it properly needs a sysroot with its own `.pc` files and
         * PKG_CONFIG_SYSROOT_DIR pointing at it, which is RFC-0016's business
         * and not a flag's.
         */
        fprintf(stderr,
                "molto: '%s' declares [target].host, and molto cannot resolve a host library "
                "for another platform: pkg-config answers for this machine\n",
                ctx_out->project_name);
        return exit_invalid_manifest;
    }
    if(result != exit_ok)
        return result;

    char src_dir[PATH_BUFFER_SIZE];
    if(!fs_format_path(src_dir, sizeof src_dir, "%s/" DIR_SRC, root)) {
        (void)fs_report_long_path(root);
        return exit_build_failure;
    }
    if(!fs_is_dir(src_dir)) {
        fprintf(stderr, "molto: no " DIR_SRC " directory in '%s'\n", root);
        return exit_build_failure;
    }

    /* Dependencies first: what they contribute has to be in [target] before a
       compile line is built out of it, and their sources have to be in the
       list before the toolchain question is asked — a dependency written in
       C++ decides which driver this build needs as much as the project's own
       code does. */
    char deps_err[512] = "";
    if(!build_prepare_and_lock(root, ctx_out, &plan->deps, &plan->dev, deps_err, sizeof deps_err)) {
        fprintf(stderr, "molto: %s\n", deps_err);
        return exit_dependency_failure;
    }

    /* The frontend describes the project; the engine below consumes what it
       said. The manifest is read twice for now — once here and once by
       build_load_project above, which is still where the compile line's options come
       from — and the second read goes away with `project_ctx` when the options
       are lowered from the document too.
     *
       Asked of the whole selection rather than of the native frontend by name.
       Today that resolves to the same thing — build_load_project above has already
       refused a directory with no Project.toml, and frontend_run prefers the
       native frontend wherever one is — so this changes no build that works
       now. What it changes is where the choice lives: one service decides which
       frontend describes a directory, and `molto build` and `molto ir` cannot
       come to disagree about it. The plugin half of that choice becomes
       reachable when a build can find a root without a manifest. */
    char frontend_err[512] = "";
    const frontend_result described =
        frontend_run(root, profile_name(profile), &plan->doc, frontend_err, sizeof frontend_err);
    if(described != frontend_ok) {
        fprintf(stderr, "molto: %s\n",
                frontend_err[0] != '\0' ? frontend_err : "nothing here describes a project");
        return frontend_exit_code(described);
    }
    if(!build_document_sources(&plan->doc, root, doc_targets_project, &plan->sources))
        return exit_build_failure;

    /* What `resolve` found, said in the document. It runs here and not in the
       frontend because a frontend describes a project and not its graph — and
       because doing it there would make `molto ir` resolve, which means the
       network, for a command whose whole purpose is to show what is already
       known.
     *
       The order is the list RFC-0015 asks for: what the dependencies are, then
       what they export to the targets, then the targets they are themselves.
       The last runs after the fold so that a package is described carrying its
       own recipe and nothing the consumer resolved — which the fold also
       refuses on its own, so the two cannot drift. */
    if(!ir_transform_dependencies(&plan->doc, &plan->deps, &plan->dev, frontend_err,
                                  sizeof frontend_err) ||
       !ir_transform_fold_dependencies(&plan->doc, frontend_err, sizeof frontend_err) ||
       !ir_transform_dependency_targets(&plan->doc, &plan->deps, &plan->dev, ctx_out->target.std,
                                        ctx_out->target.cpp_std, frontend_err,
                                        sizeof frontend_err)) {
        fprintf(stderr, "molto: %s\n", frontend_err);
        return exit_build_failure;
    }

    plan->labels = build_labels_for(&plan->doc);
    if(plan->labels == NULL)
        return exit_build_failure;

    if(!document_is_allowed(&plan->doc, root, profile, platform, ctx_out, &plan->deps, &plan->dev,
                            frontend_err, sizeof frontend_err)) {
        fprintf(stderr, "molto: %s\n", frontend_err);
        return exit_build_failure;
    }

    /* The interface of the dependencies folds into `[target]`, which is what
       the link line still reads. What each of them compiles itself with is a
       target of its own in the document now, so this pass is built the same way
       the project's is. */
    if(!deps_merge_interface(ctx_out, &plan->deps))
        return exit_dependency_failure;
    if(!build_document_sources(&plan->doc, root, doc_targets_runtime_packages,
                               &plan->package_sources))
        return exit_build_failure;
    plan->package_units = build_units_from_document(&plan->doc, doc_targets_runtime_packages,
                                                    &plan->package_sources, plan->labels);
    if(plan->package_units == NULL)
        return exit_build_failure;

    if(str_list_count(&plan->sources) == 0) {
        fprintf(stderr, "molto: no source files found under '%s'\n", src_dir);
        return exit_build_failure;
    }

    /* Which compiler to use is settled once per build, after the sources are
       known: a project with C++ in it needs a toolchain that has a C++ driver,
       and that is part of the question. */
    bool needs_cpp = false;
    for(size_t i = 0; i < str_list_count(&plan->sources); i++)
        needs_cpp = needs_cpp || source_is_cpp(str_list_get(&plan->sources, i));
    for(size_t i = 0; i < str_list_count(&plan->package_sources); i++)
        needs_cpp = needs_cpp || source_is_cpp(str_list_get(&plan->package_sources, i));
    result =
        toolchain_resolve(&ctx_out->target, platform, needs_cpp, db, refresh_toolchain, chain_out);
    if(result != exit_ok)
        return result;

    char segment[PATH_BUFFER_SIZE];
    if(!build_segment(profile, platform, segment, sizeof segment)) {
        (void)fs_report_long_path(root);
        return exit_build_failure;
    }

    const build_pass_env env = {
        .root = root,
        .profile = profile,
        .segment = segment,
        .settings = build_profile_settings(ctx_out, profile),
        .env = &ctx_out->env,
        .chain = chain_out,
        .db = db,
        .options = options,
    };

    /* Dependencies first, and in one pass of their own: each is compiled
       against the language standard and its own recipe, so what reaches the
       compiler is the same in every project that depends on it — which is what
       makes one compiled object worth sharing. */
    result = build_plan_add(plan, &env, plan->package_units, str_list_count(&plan->package_sources),
                            objects_out);
    if(result != exit_ok)
        return result;

    plan->project_units =
        build_units_from_document(&plan->doc, doc_targets_project, &plan->sources, plan->labels);
    if(plan->project_units == NULL)
        return exit_build_failure;
    return build_plan_add(plan, &env, plan->project_units, str_list_count(&plan->sources),
                          objects_out);
}

/* Write out what this build compiled, for whoever parses this code without
   being the build: clangd, clang-tidy, cppcheck (RFC-0007).
 *
 * It is published even when the build failed, because that is when an editor
 * that understands the project is worth the most — and a command line does not
 * become wrong just because the code it describes does not compile. Failing to
 * write it is a warning: nothing about the artifact depends on it. */
void build_publish_compile_db(const compile_db *cdb, const char *root) {
    if(compile_db_count(cdb) == 0)
        return;
    if(!compile_db_write(cdb, root))
        fprintf(stderr, "molto: warning: could not write compile_commands.json; "
                        "editors and static analysers will have to guess\n");
}

int build_project(const char *root, build_profile profile, const char *platform,
                  bool refresh_toolchain, size_t jobs, char *out_binary, size_t out_binary_size) {
    return build_project_with(root, profile, platform, refresh_toolchain, jobs, out_binary,
                              out_binary_size, NULL);
}

int build_project_with(const char *root, build_profile profile, const char *platform,
                       bool refresh_toolchain, size_t jobs, char *out_binary,
                       size_t out_binary_size, build_report *report) {
    wsdb *db = wsdb_open(root);
    if(db == NULL) {
        fprintf(stderr, "molto: could not open the workspace database (locked?)\n");
        return exit_build_failure;
    }

    project_ctx ctx;
    resolved_toolchain chain;
    str_list objects;
    str_list_init(&objects);
    bool any_compiled = false;
    /* The plan resolves development dependencies too — they share the graph and
       the version check — but this build compiles and links none of them. */
    build_plan plan;
    build_plan_init(&plan);
    const pass_options options = {.jobs = jobs, .cdb = compile_db_create()};
    int result = build_plan_project(root, profile, platform, refresh_toolchain, db, &options, &ctx,
                                    &chain, &objects, &plan);
    if(result == exit_ok) {
        build_report_plan(&plan, root, report);
        build_report_begin(report, plan.to_build);
        result = build_run_plan(&plan, report, &any_compiled);
    }
    const bool any_cpp = plan.any_cpp;
    build_publish_compile_db(options.cdb, root);
    compile_db_destroy(options.cdb);

    /* The plan outlives the compiles now, because the link line is read off the
       document it holds. Releasing it where the last object was written would
       take the target node with it. */
    if(result == exit_ok) {
        const ir_target *node = plan.project_units[0].node;

        /* The name is worked out again from the manifest rather than read off
           the document, for the same reason the host bounds are: what a
           producer wrote is what is being checked, and a path taken from it
           would be a path checking itself. For the native frontend the two
           agree by construction, which is what makes this cheap. */
        library_names names;
        char name_err[512] = "";
        char binary[PATH_BUFFER_SIZE];
        char directory[PATH_BUFFER_SIZE];
        char segment[PATH_BUFFER_SIZE];
        if(!library_names_of(ctx.artifact, ctx.project_name, ctx.version, &names, name_err,
                             sizeof name_err) ||
           !build_segment(profile, platform, segment, sizeof segment) ||
           !build_compose_binary_path(root, segment, names.file, node->kind, binary,
                                      sizeof binary) ||
           !fs_format_path(directory, sizeof directory, "%s/" DIR_BUILD "/%s", root, segment)) {
            if(name_err[0] != '\0')
                build_report_message(report, "molto: %s\n", name_err);
            build_plan_free(&plan);
            str_list_free(&objects);
            (void)wsdb_close(db);
            return exit_build_failure;
        }

        /* Whatever the linker had to say has already been framed and printed
           by then; a line here would only repeat it less clearly. */
        bool produced = false;
        if(node->kind == ir_target_static) {
            produced =
                build_archive_project(&objects, binary, &ctx.env, &chain, any_compiled, db, report);
        } else {
            produced = build_link_project(any_cpp, &objects, binary, node, &ctx.env, &chain,
                                          any_compiled, db, root, report);
            if(produced && node->kind == ir_target_shared)
                build_place_shared_links(directory, &names, report);
        }
        if(!produced)
            result = exit_build_failure;
        if(result == exit_ok) {
            /* Prune objects orphaned by removed sources (scoped to src/). */
            char prefix[PATH_BUFFER_SIZE];
            if(fs_format_path(prefix, sizeof prefix, "%s/" DIR_BUILD "/%s/" DIR_OBJ "/" DIR_SRC "/",
                              root, segment))
                wsdb_prune(db, &objects, prefix);
            if(out_binary != NULL && !fs_format_path(out_binary, out_binary_size, "%s", binary)) {
                (void)fs_report_long_path(binary);
                result = exit_build_failure;
            }
        }
    }

    build_plan_free(&plan);
    str_list_free(&objects);
    build_warn_if_not_saved(db);
    return result;
}
