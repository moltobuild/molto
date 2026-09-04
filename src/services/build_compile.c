#include <molto/services/build_service.h>

#include "build_internal.h"

#include <molto/build/compile_flags.h>
#include <molto/build/depfile.h>
#include <molto/build/diagnostic.h>
#include <molto/build/diagnostic_view.h>
#include <molto/build/report.h>
#include <molto/services/fs_service.h>
#include <molto/services/object_cache.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/util/str_list.h>
#include <molto/util/task_pool.h>
#include <molto/workspace/wsdb.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Compiling: the command line for one unit, the worker that runs it, what the
 * compiler said about it, and the plan that decides how many of them there are.
 *
 * This is the one part of the build that could not be cut smaller, and the call
 * graph is why: composing a command line, deciding whether a unit is stale,
 * running the compiler, and reading what it wrote are four steps of one
 * question, and every one of them is on the fingerprint. The command decides
 * staleness, staleness decides whether the worker runs, the worker's output
 * decides whether the result may be recorded, and what is recorded is the
 * command. Splitting that ring anywhere puts half a decision in another file.
 *
 * The plan sits here for the same reason rather than beside the orchestration
 * it serves: `plan_add` is `plan_pass` for one pass and `run_plan` is `run_pass`
 * for each of them, and a plan that lived elsewhere would be a name for a
 * loop.
 */

/* The fingerprint of a command: the argv it will run, and after the mark the
   environment it will run in. Heap string, caller frees; NULL on failure.

   The two are one string because two consumers ask the same question of it —
   the workspace database compares it, the shared object cache hashes it — and
   one string is what stops their answers from disagreeing. Nothing is appended
   when there is no [env], which is what leaves the databases and cache entries
   already on disk valid. */
char *build_command_fingerprint(const str_list *argv, const project_env *env) {
    char environment[PROJECT_ENV_FINGERPRINT_MAX];
    size_t env_length = project_env_fingerprint(env, environment, sizeof environment);
    char *command = build_join_args(argv);
    if(command == NULL || env_length == 0)
        return command;

    size_t length = strlen(command);
    size_t total = length + strlen(OBJECT_CACHE_ENV_MARK) + env_length + 1;
    char *joined = realloc(command, total);
    if(joined == NULL) {
        free(command);
        return NULL;
    }
    snprintf(joined + length, total - length, OBJECT_CACHE_ENV_MARK "%s", environment);
    return joined;
}

/* Run a command held in a str_list argv (adds the NULL terminator), exporting
   the project's [env] variables to the child.

   `capture` is where everything the child writes to either stream is kept, or
   NULL to let it inherit Molto's own and write straight to the terminal. A
   compile is captured, because a diagnostic has to be read and framed before
   it is shown, and because a compiler writing beside a progress bar lands in
   the middle of it. A link is captured for the first of those reasons. */
int build_run_str_argv(const str_list *argv, const project_env *env, char *capture,
                       size_t capture_size, bool *truncated) {
    size_t count = str_list_count(argv);
    const char **cargv = (const char **)malloc((count + 1) * sizeof(char *));
    if(cargv == NULL)
        return -1;
    for(size_t i = 0; i < count; i++)
        cargv[i] = str_list_get(argv, i);
    cargv[count] = NULL;

    process_env_var vars[PROJECT_MAX_ENV];
    size_t var_count = project_env_to_vars(env, vars, PROJECT_MAX_ENV);
    int status = capture != NULL
                     ? process_capture_all(cargv, vars, var_count, capture, capture_size, truncated)
                     : process_run_env(cargv, vars, var_count);
    free((void *)cargv);
    return status;
}

/* Build the full compile command for one unit into `argv` (a str_list):
   driver, -c, source, -o, object, -O<n>, [-g], [-std], the unit's defines/
   includes/flags, -MMD -MF depfile, and its include flags. */
/* One scope's options and then its includes, which is the only order a document
   can express: it does not distinguish a define from a flag, because a define
   already *is* `-DFOO=1` by the time it is a `CompileOption`.
 *
 * The three scopes reach the line in the order RFC-0013 fixes — target, then
 * profile, then unit — and that order is contract rather than detail. A
 * compiler takes the last of two contradictory flags, so the most specific
 * statement about a unit has to be the one it sees last. It is why `-std` from
 * `[target].std` now wins over one written by hand into `flags`, which is the
 * accident being corrected rather than a rule being bent. */
static bool push_scope(str_list *argv, const char *root, const ir_target *node,
                       const ir_source *unit, ir_scope scope) {
    bool ok = true;
    for(size_t i = 0; ok && i < node->option_count; i++) {
        if(node->options[i].scope == scope)
            ok = str_list_push(argv, node->options[i].value);
    }
    for(size_t i = 0; ok && unit != NULL && i < unit->option_count; i++) {
        if(unit->options[i].scope == scope)
            ok = str_list_push(argv, unit->options[i].value);
    }
    for(size_t i = 0; ok && i < node->include_count; i++) {
        if(node->includes[i].scope == scope)
            ok = compile_flags_push_include(argv, root, node->includes[i].value);
    }
    return ok;
}

/* The compile line a document describes. */
static bool push_document(str_list *argv, const char *root, const compile_unit *unit) {
    return push_scope(argv, root, unit->node, unit->unit, ir_scope_target) &&
           push_scope(argv, root, unit->node, unit->unit, ir_scope_profile) &&
           push_scope(argv, root, unit->node, unit->unit, ir_scope_unit);
}

static bool build_compile_argv(str_list *argv, const char *root, const compile_unit *unit,
                               const char *object, const manifest_profile *settings,
                               const char *depfile, const resolved_toolchain *chain) {
    const char *source = unit->source;
    bool is_cpp = source_is_cpp(source);
    const char *driver = compile_flags_driver(chain, is_cpp);
    if(driver == NULL) {
        fprintf(stderr, "molto: '%s' needs a C++ compiler and none was resolved\n", source);
        return false;
    }

    char opt_flag[OPT_FLAG_SIZE];
    snprintf(opt_flag, sizeof opt_flag, OPT_FLAG_FORMAT, settings->opt_level);

    bool ok = str_list_push(argv, driver) && str_list_push(argv, ARG_COMPILE) &&
              str_list_push(argv, source) && str_list_push(argv, ARG_OUTPUT) &&
              str_list_push(argv, object) && str_list_push(argv, opt_flag);
    if(ok && settings->debug_info)
        ok = str_list_push(argv, ARG_DEBUG);
    if(ok)
        ok = push_document(argv, root, unit);
    if(ok)
        ok = str_list_push(argv, ARG_DEPFILE_GEN) && str_list_push(argv, ARG_DEPFILE_OUT) &&
             str_list_push(argv, depfile);
    return ok;
}

/* The compile command for one unit, depfile path included. The three callers
   below all need the same argv — to run it, to fingerprint it, and to write it
   into the compilation database — and a fourth spelling of it would be one
   that could disagree with what is executed. */
[[nodiscard]] static bool unit_argv(str_list *argv, const char *root, const compile_unit *unit,
                                    const char *object, const manifest_profile *settings,
                                    const resolved_toolchain *chain) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    return build_depfile_path_for(object, depfile, sizeof depfile) &&
           build_compile_argv(argv, root, unit, object, settings, depfile, chain);
}

/* The same compile line, as a tool that is not the build should read it: minus
   `-MMD` and `-MF <path>`.
 *
 * They are the one part of the line that says nothing about the translation.
 * They exist so the build learns which headers a unit read, and no consumer of
 * the compilation database wants them — Clang's own tooling strips them before
 * parsing, and the tools that instead *run* the line, like
 * include-what-you-use, would write a depfile into `build/` on Molto's behalf.
 *
 * The line that is executed and the line that is fingerprinted both keep them.
 * Only the description drops them, so nothing about freshness moves. */
[[nodiscard]] static bool described_argv(str_list *out, const str_list *argv) {
    for(size_t i = 0; i < str_list_count(argv); i++) {
        const char *argument = str_list_get(argv, i);
        if(strcmp(argument, ARG_DEPFILE_GEN) == 0)
            continue;
        if(strcmp(argument, ARG_DEPFILE_OUT) == 0) {
            i++; /* and the path it names */
            continue;
        }
        if(!str_list_push(out, argument))
            return false;
    }
    return true;
}

/* Compile a single translation unit to `object`, keeping what the compiler
   said about it in `output`. gcc writes the header dependency file
   (`-MMD -MF <object>.d`) as a side effect; it is absorbed into the WSDB
   afterwards, on the main thread.

   Returns the compiler's exit code, so a caller can tell a unit that failed
   loudly from one that failed without a word. */
static int compile_one(const char *root, const compile_unit *unit, const char *object,
                       const manifest_profile *settings, const project_env *env,
                       const resolved_toolchain *chain, char *output, size_t output_size,
                       bool *truncated) {
    str_list argv;
    str_list_init(&argv);
    if(!unit_argv(&argv, root, unit, object, settings, chain)) {
        str_list_free(&argv);
        return -1;
    }
    const int status = build_run_str_argv(&argv, env, output, output_size, truncated);
    str_list_free(&argv);
    return status;
}

/* Record a freshly compiled object into the WSDB: read the prerequisites from
   gcc's depfile (falling back to just the source), store {command, prereqs},
   then delete the now-absorbed depfile. Runs on the main thread. Returns false
   if the object could not be recorded, which only costs a rebuild next time. */
[[nodiscard]] static bool wsdb_absorb_object(wsdb *db, const char *source, const char *object,
                                             const char *command) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    if(!build_depfile_path_for(object, depfile, sizeof depfile))
        return false;
    str_list prereqs;
    str_list_init(&prereqs);
    if(!depfile_read(depfile, &prereqs) || str_list_count(&prereqs) == 0) {
        if(!str_list_push(&prereqs, source)) {
            str_list_free(&prereqs);
            return false;
        }
    }
    bool ok = wsdb_record_object(db, object, command, &prereqs);
    str_list_free(&prereqs);
    remove(depfile);
    return ok;
}

/* Drop the depfile left behind by a unit that failed to compile: nothing will
   absorb it, and a stale one would outlive the source it describes. */
static void discard_depfile(const char *object) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    if(build_depfile_path_for(object, depfile, sizeof depfile))
        remove(depfile);
}

/* --- naming a unit --- */

/* The directory a unit's sources are named relative to: its own package's, or
   the project's for the project's own code. A dependency lives in the shared
   cache, and naming its sources relative to the project root would print the
   whole cache path on every line. */
static const char *naming_root(const compile_unit *unit, const char *root) {
    if(unit->label != NULL && unit->label->source != NULL && unit->label->source[0] != '\0')
        return unit->label->source;
    return root;
}

/* How a source is named on a line: relative to the directory it was discovered
   in, so `src/net/http.c` reads as `net/http.c` and the column stays about the
   file rather than about where the project happens to live.

   Not the base name, which would print two different files as one line. A
   source that is under neither directory — a framework a manifest pointed
   `[test].sources` at — keeps its path from the project root. */
static const char *display_source(const char *root, const char *source) {
    const char *relative = build_relative_to_root(root, source);
    if(strncmp(relative, DIR_SRC "/", sizeof(DIR_SRC "/") - 1) == 0)
        return relative + sizeof(DIR_SRC "/") - 1;
    if(strncmp(relative, DIR_TESTS "/", sizeof(DIR_TESTS "/") - 1) == 0)
        return relative + sizeof(DIR_TESTS "/") - 1;
    return relative;
}

/* Where a package's sources are, but only when that is somewhere the reader
   can go and look. A dependency fetched into the shared cache is named by its
   coordinate on the line above, and its cache path is eighty columns that say
   no more than the coordinate already did. */
static const char *shown_source(const build_unit_label *label, const char *root) {
    if(label == NULL || label->source == NULL || label->source[0] == '\0')
        return NULL;
    /* Already relative, which is how a path dependency is kept: the manifest
       named it the way the reader would type it, and there is nothing to
       shorten. */
    if(!fs_path_is_absolute(label->source))
        return label->source;
    const char *relative = fs_relative_to(label->source, root);
    return relative != label->source ? relative : NULL;
}

/* --- what the compiler said --- */

/* A diagnostic Molto wrote itself, for what a tool left unsaid. */
void build_push_own(diagnostic_list *found, const char *source, diagnostic_severity severity,
                    const char *message) {
    diagnostic item = {.severity = severity};
    snprintf(item.file, sizeof item.file, "%s", source);
    snprintf(item.message, sizeof item.message, "%s", message);
    (void)diagnostic_list_push(found, &item);
}

/* One parallel compilation task: compile a planned unit, recording a shared
   failure flag. Runs on a task_pool worker. */
typedef struct {
    const build_pass_env *env;
    const planned_unit *planned;
    atomic_bool *failed;
    build_report *report;
    bool succeeded;     /* written only by the worker owning this task */
    uint64_t signature; /* what the source was when this compilation began */
} compile_task;

/* Everything the compiler had to say about one unit, framed and written as a
   single act — one call, so it is atomic against the bar and against the other
   workers, and with the text as an argument rather than as a format, because a
   compiler message is full of per-cent signs.
 *
 * Called whether or not the unit compiled: capturing the compiler's output and
 * then printing it only on failure would make every warning in every green
 * build disappear. */
static void report_diagnostics(const compile_task *task, const char *output, bool truncated,
                               int status) {
    const build_pass_env *env = task->env;
    const compile_unit *unit = task->planned->unit;

    diagnostic_list found;
    diagnostic_list_init(&found);
    if(!diagnostic_parse(output, &found)) {
        diagnostic_list_free(&found);
        return;
    }
    /* Said once for the whole unit: one compiler produced all of it. */
    diagnostic_list_set_columns(&found, diagnostic_columns_of_vendor(env->chain->vendor));
    if(truncated)
        build_push_own(&found, unit->source, diagnostic_severity_note,
                       "there was more of this than Molto kept");
    if(status != 0 && diagnostic_count_severity(&found, diagnostic_severity_error) == 0)
        build_push_own(&found, unit->source, diagnostic_severity_error,
                       status > SIGNAL_EXIT_BASE
                           ? "the compiler was killed while compiling this file"
                           : "the compiler failed with nothing to say about this "
                             "file");

    char compiler[TOOLCHAIN_DESCRIPTION_MAX];
    build_describe_compiler(env->chain, source_is_cpp(unit->source), compiler, sizeof compiler);
    const diagnostic_context ctx = {
        .unit = display_source(naming_root(unit, env->root), unit->source),
        .package = unit->label != NULL ? unit->label->name : NULL,
        .version = unit->label != NULL ? unit->label->version : NULL,
        .source = shown_source(unit->label, env->root),
        .compiler = compiler,
        .root = env->root,
    };
    char *block = diagnostic_view_render(&found, &ctx, build_report_wants_colour(task->report));
    diagnostic_list_free(&found);
    if(block == NULL)
        return;
    build_report_message(task->report, "%s\n", block);
    free(block);
}

static void compile_task_run(void *arg) {
    compile_task *task = arg;
    const build_pass_env *env = task->env;
    const compile_unit *unit = task->planned->unit;

    /* Named here rather than when the pass was planned: the region says what
       is being compiled at this instant, and the planning happened before any
       compiler of this build had run. The token is a local because its whole
       life is this function. */
    const build_report_slot slot = build_report_unit_started(
        task->report, unit->label, display_source(naming_root(unit, env->root), unit->source));

    /* One buffer per worker, held only while the compiler runs. The whole of
       what gcc says about a broken translation unit fits in it many times
       over, and what does not is reported as having been cut. */
    char *output = malloc(BUILD_OUTPUT_SIZE);
    bool truncated = false;
    const int status =
        compile_one(env->root, unit, task->planned->object, &env->settings, env->env, env->chain,
                    output, output != NULL ? BUILD_OUTPUT_SIZE : 0, &truncated);
    task->succeeded = status == 0;

    if(output != NULL)
        report_diagnostics(task, output, truncated, status);
    else if(!task->succeeded)
        build_report_message(task->report, "molto: failed to compile '%s'\n", unit->source);
    free(output);

    if(!task->succeeded)
        atomic_store(task->failed, true);
    build_report_unit_done(task->report, slot);
}

/* Take a dependency's object out of the shared cache, and record it as if it
   had just been compiled — because as far as anything downstream can tell, it
   was. False when there is nothing to take. */
[[nodiscard]] static bool take_from_object_cache(wsdb *db, const char *source, const char *object,
                                                 const char *command) {
    char cached[OBJECT_CACHE_PATH_MAX];
    if(!object_cache_path(source, command, cached, sizeof cached))
        return false;
    if(!object_cache_take(cached, object))
        return false;

    /* Without a depfile there are no headers to watch, so the source stands in
       for them. That is sound here and nowhere else: the tree a dependency was
       fetched into is immutable, so its headers cannot change without the
       coordinate changing with them. */
    str_list prereqs;
    str_list_init(&prereqs);
    const bool recorded =
        str_list_push(&prereqs, source) && wsdb_record_object(db, object, command, &prereqs);
    str_list_free(&prereqs);
    if(!recorded)
        fprintf(stderr, "molto: warning: could not record the cached object for '%s'\n", source);
    return true;
}

/* Offer a freshly compiled dependency object to the next project that would
   compile it the same way. */
static void share_in_object_cache(const char *source, const char *object, const char *command) {
    char cached[OBJECT_CACHE_PATH_MAX];
    if(object_cache_path(source, command, cached, sizeof cached))
        object_cache_put(object, cached);
}

/* Every source of every target of one set, as units the passes compile.
 *
 * It walks the document in the same order `build_document_sources` did, so the path
 * at index i of `sources` is the source at index i here — which is what lets a
 * unit keep borrowing the arena the plan already owns while pointing at the
 * node that describes it.
 *
 * `labels` is indexed by the target's position in the document, so a package's
 * units are named after the package and the project's after the project,
 * without a unit having to carry a copy of either.
 *
 * The units borrow the document and the arena, so both have to outlive them.
 * Caller frees. NULL means the allocation failed. */
[[nodiscard]] compile_unit *build_units_from_document(const ir_document *doc, doc_target_set set,
                                                      const str_list *sources,
                                                      const build_unit_label *labels) {
    const size_t total = str_list_count(sources);
    compile_unit *units = calloc(total, sizeof *units);
    if(units == NULL)
        return NULL;

    size_t at = 0;
    for(size_t t = 0; t < doc->target_count; t++) {
        const ir_target *node = &doc->targets[t];
        if(!build_in_set(doc, node, set))
            continue;
        for(size_t i = 0; i < node->source_count && at < total; i++, at++) {
            units[at] = (compile_unit){
                .source = str_list_get(sources, at),
                .node = node,
                .unit = &node->sources[i],
                .label = &labels[t],
            };
        }
    }
    return units;
}

/*
 * Phase 1, and now a pass of its own: work out what this pass would compile
 * without compiling any of it.
 *
 * It is separate because the report needs a number nobody can give it
 * otherwise. A bar has to know its denominator before the first unit starts,
 * and a build makes up to four passes — so every one of them is planned, and
 * only then does anything run. The question each unit is asked is unchanged;
 * what changed is that the answers are kept instead of acted on immediately.
 */
[[nodiscard]] static int plan_pass(compile_pass *pass, const build_pass_env *env,
                                   const compile_unit *units, size_t count, str_list *objects,
                                   bool *any_cpp) {
    pass->env = *env;
    pass->units = calloc(count, sizeof *pass->units);
    if(pass->units == NULL)
        return exit_build_failure;
    pass->count = count;

    for(size_t i = 0; i < count; i++) {
        const char *source = units[i].source;
        if(source_is_cpp(source))
            *any_cpp = true;
        char object[PATH_BUFFER_SIZE];
        if(!build_object_path_for(env->root, env->segment, source, object, sizeof object))
            return exit_build_failure;
        if(!build_make_parent_dirs(object)) {
            fprintf(stderr, "molto: could not create output directory for '%s'\n", object);
            return exit_build_failure;
        }
        /* `objects` accumulates across every pass a build makes, and each unit
           keeps the pointer its own entry was pushed as. That stays valid
           however much the list grows afterwards: str_list reallocates the
           array of pointers and never the strings they point at. */
        if(!str_list_push(objects, object))
            return exit_build_failure;

        planned_unit *planned = &pass->units[i];
        planned->unit = &units[i];
        planned->object = str_list_get(objects, str_list_count(objects) - 1);

        /* One argv answers both questions asked here: whether this unit is
           stale, and what it compiles as. The second is recorded for every
           unit and not only the stale ones — an editor asks what a file
           compiles as, and "it was already up to date" is not an answer. */
        str_list argv;
        str_list_init(&argv);
        char *command = NULL;
        if(unit_argv(&argv, env->root, &units[i], planned->object, &env->settings, env->chain)) {
            command = build_command_fingerprint(&argv, env->env);
            str_list described;
            str_list_init(&described);
            if(!described_argv(&described, &argv) ||
               !compile_db_add(env->options->cdb, source, planned->object, &described))
                fprintf(stderr, "molto: warning: could not describe '%s' for the editor\n", source);
            str_list_free(&described);
        }
        str_list_free(&argv);
        planned->needs_compile =
            command == NULL || !wsdb_object_fresh(env->db, planned->object, command);

        /* A stale object that another project already compiled the same way is
           not compiled again: it is copied out of the shared cache and
           recorded as if it had been. Only a dependency qualifies, because
           only a dependency's tree is immutable enough for a coordinate to
           answer for its contents. */
        if(planned->needs_compile && command != NULL)
            planned->needs_compile =
                !take_from_object_cache(env->db, source, planned->object, command);

        /* Kept only where it has something left to say. A unit nothing will
           compile has already spent its fingerprint on the one question it
           was built to answer. */
        if(planned->needs_compile) {
            planned->command = command;
        } else {
            free(command);
        }
        pass->to_build += planned->needs_compile ? 1 : 0;
    }
    return exit_ok;
}

/* Phases 2 and 3: compile what the plan marked stale, in parallel, and record
   what actually got built. Reports whether anything was compiled at all, which
   is what decides whether the link has to run again. */
static int run_pass(const compile_pass *pass, build_report *report, bool *any_compiled) {
    if(pass->to_build == 0)
        return exit_ok;

    compile_task *tasks = calloc(pass->to_build, sizeof *tasks);
    task_pool *pool = task_pool_create(pass->env.options->jobs);
    if(tasks == NULL || pool == NULL) {
        free(tasks);
        task_pool_destroy(pool);
        return exit_build_failure;
    }

    atomic_bool failed = false;
    int result = exit_ok;
    size_t queued = 0;
    for(size_t i = 0; i < pass->count && result == exit_ok; i++) {
        if(!pass->units[i].needs_compile)
            continue;
        tasks[queued] = (compile_task){
            .env = &pass->env,
            .planned = &pass->units[i],
            .failed = &failed,
            .report = report,
            /* Sampled here rather than when the pass was planned: it stands
               for what the compiler is about to read, and planning happened
               before every other pass of this build ran. */
            .signature = fs_signature(pass->units[i].unit->source),
        };
        if(!task_pool_submit(pool, compile_task_run, &tasks[queued]))
            result = exit_build_failure;
        queued++;
    }
    task_pool_wait(pool);
    task_pool_destroy(pool);

    if(result == exit_ok && atomic_load(&failed))
        result = exit_build_failure;

    /* Phase 3: record what was actually built (single-threaded). This runs even
       when a unit failed, so the units that did compile are not thrown away and
       recompiled on the next run. */
    for(size_t i = 0; i < queued; i++) {
        const compile_task *task = &tasks[i];
        const planned_unit *planned = task->planned;
        if(!task->succeeded) {
            discard_depfile(planned->object);
            continue;
        }
        /* A source edited while it was being compiled would otherwise be
           recorded under the signature of content the object does not contain,
           and nothing would rebuild it afterwards: the stale object simply gets
           linked. Leaving it unrecorded costs one recompilation. */
        const char *source = planned->unit->source;
        if(fs_signature(source) != task->signature) {
            discard_depfile(planned->object);
            continue;
        }
        if(planned->command == NULL ||
           !wsdb_absorb_object(pass->env.db, source, planned->object, planned->command))
            build_report_message(report, "molto: warning: could not record '%s' as up to date\n",
                                 source);
        if(planned->command != NULL)
            share_in_object_cache(source, planned->object, planned->command);
    }

    *any_compiled = true;
    free(tasks);
    return result;
}

void build_plan_init(build_plan *plan) {
    memset(plan, 0, sizeof *plan);
    ir_document_init(&plan->doc);
    prepared_deps_init(&plan->deps);
    prepared_deps_init(&plan->dev);
    str_list_init(&plan->sources);
    str_list_init(&plan->test_sources);
    str_list_init(&plan->package_sources);
    str_list_init(&plan->dev_package_sources);
}

void build_plan_free(build_plan *plan) {
    for(size_t i = 0; i < plan->pass_count; i++) {
        for(size_t j = 0; j < plan->passes[i].count; j++)
            free(plan->passes[i].units[j].command);
        free(plan->passes[i].units);
    }
    free(plan->dev_package_units);
    free(plan->package_units);
    free(plan->project_units);
    free(plan->test_units);
    free(plan->labels);
    str_list_free(&plan->dev_package_sources);
    str_list_free(&plan->package_sources);
    str_list_free(&plan->test_sources);
    str_list_free(&plan->sources);
    prepared_deps_free(&plan->dev);
    prepared_deps_free(&plan->deps);
    ir_document_free(&plan->doc);
    memset(plan, 0, sizeof *plan);
}

/* One more pass, planned onto the end. Nothing to compile is not a pass: a
   dependency-free project would otherwise carry an empty one. */
[[nodiscard]] int build_plan_add(build_plan *plan, const build_pass_env *env,
                                 const compile_unit *units, size_t count, str_list *objects) {
    if(count == 0)
        return exit_ok;
    if(plan->pass_count >= BUILD_MAX_PASSES)
        return exit_build_failure;
    compile_pass *pass = &plan->passes[plan->pass_count++];
    const int result = plan_pass(pass, env, units, count, objects, &plan->any_cpp);
    plan->to_build += pass->to_build;
    return result;
}

/* Compile the plan, pass by pass, stopping at the first one that failed — so a
   dependency that would not build still prevents the code that includes it
   from being compiled against it. */
int build_run_plan(const build_plan *plan, build_report *report, bool *any_compiled) {
    int result = exit_ok;
    for(size_t i = 0; i < plan->pass_count && result == exit_ok; i++)
        result = run_pass(&plan->passes[i], report, any_compiled);
    return result;
}

/* Tell the report what the build is about to do: the work, unit by unit, and
   a count of everything that turned out not to be work at all. */
void build_report_plan(const build_plan *plan, const char *root, build_report *report) {
    for(size_t p = 0; p < plan->pass_count; p++) {
        const compile_pass *pass = &plan->passes[p];
        for(size_t i = 0; i < pass->count; i++) {
            const planned_unit *planned = &pass->units[i];
            if(planned->needs_compile)
                build_report_will_compile(
                    report, planned->unit->label,
                    display_source(naming_root(planned->unit, root), planned->unit->source));
            else
                build_report_skipped(report);
        }
    }
}
