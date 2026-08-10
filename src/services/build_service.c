#include <molto/services/build_service.h>

#include <molto/build/compile_flags.h>
#include <molto/build/depfile.h>
#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/project/lockfile.h>
#include <molto/project/project_ctx.h>
#include <molto/services/deps_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/manifest_service.h>
#include <molto/services/object_cache.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/str_list.h>
#include <molto/util/task_pool.h>
#include <molto/workspace/wsdb.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compiler command-line arguments. */
#define ARG_COMPILE "-c"           /* compile only, do not link */
#define ARG_OUTPUT "-o"            /* next argument is the output path */
#define ARG_DEBUG "-g"             /* emit debug symbols */
#define ARG_DEPFILE_GEN "-MMD"     /* also write a header-dependency file */
#define ARG_DEPFILE_OUT "-MF"      /* next argument is the dependency file path */
#define OPT_FLAG_FORMAT "-O%d"     /* optimisation level, e.g. -O2 */
#define INCLUDE_FLAG_FORMAT "-I%s" /* add an include search directory */
#define LINK_FLAG_PREFIX "-l"      /* prepended to a system library name */

/* On-disk layout of a Molto project. */
#define MANIFEST_FILENAME "Project.toml"
#define DIR_BUILD "build"          /* output root, e.g. build/<profile>/ */
#define DIR_OBJ "obj"              /* compiled objects under the build root */
#define DIR_SRC "src"              /* where sources are discovered */
#define DIR_TESTS "tests"          /* where test sources are discovered */
#define OBJECT_SUFFIX ".o"         /* appended to a source path for its object */
#define DEPFILE_SUFFIX ".d"        /* appended to an object path for its depfile */
#define TEST_SUITE_SUFFIX "_tests" /* appended to the package name in single mode */

/* Size of the stack buffers used to compose filesystem paths. */
#define PATH_BUFFER_SIZE 4096

/* Size of the buffer receiving a manifest parse-error message. */
#define MANIFEST_ERROR_SIZE 256

/* Size of the small buffer holding the "-O<n>" flag. */
#define OPT_FLAG_SIZE 16

/* Compose the output executable path for a package. */
[[nodiscard]] static bool compose_binary_path(const char *root, build_profile profile,
                                              const char *name, char *out, size_t out_size) {
    return fs_format_path(out, out_size, "%s/" DIR_BUILD "/%s/%s", root, profile_name(profile),
                          name) ||
           fs_report_long_path(name);
}

/* Select the settings for `profile` from a parsed project context. */
static manifest_profile profile_settings(const project_ctx *ctx, build_profile profile) {
    switch(profile) {
    case profile_release:
        return ctx->profile.release;
    case profile_bench:
        return ctx->profile.bench;
    case profile_custom:
        return ctx->profile.custom;
    case profile_debug:
    default:
        return ctx->profile.debug;
    }
}

/* Select the per-profile extra options for `profile`. */
static const project_options *profile_options_for(const project_ctx *ctx, build_profile profile) {
    switch(profile) {
    case profile_release:
        return &ctx->profile_options.release;
    case profile_bench:
        return &ctx->profile_options.bench;
    case profile_custom:
        return &ctx->profile_options.custom;
    case profile_debug:
    default:
        return &ctx->profile_options.debug;
    }
}

/* Map a source path to its object path, mirroring the source tree under
   `root/build/<profile_dir>/obj`. */
[[nodiscard]] static bool object_path_for(const char *root, const char *profile_dir,
                                          const char *source, char *out, size_t out_size) {
    size_t root_len = strlen(root);
    const char *relative = source;
    if(strncmp(source, root, root_len) == 0 && source[root_len] == '/')
        relative = source + root_len + 1;
    return fs_format_path(out, out_size, "%s/" DIR_BUILD "/%s/" DIR_OBJ "/%s" OBJECT_SUFFIX, root,
                          profile_dir, relative) ||
           fs_report_long_path(source);
}

/* The dependency file gcc writes next to an object: "<object>.d". Used both to
   tell the compiler where to write it and to read it back when deciding whether
   to rebuild, so the two paths always match. */
[[nodiscard]] static bool depfile_path_for(const char *object, char *out, size_t out_size) {
    return fs_format_path(out, out_size, "%s" DEPFILE_SUFFIX, object) ||
           fs_report_long_path(object);
}

/* Create the parent directory chain for `path`. */
static bool make_parent_dirs(const char *path) {
    char directory[PATH_BUFFER_SIZE];
    if(!fs_format_path(directory, sizeof directory, "%s", path))
        return fs_report_long_path(path);
    char *slash = strrchr(directory, '/');
    if(slash == NULL)
        return true;
    *slash = '\0';
    return fs_make_dirs(directory);
}

/* Join argv items into one space-separated heap string (caller frees). The
   buffer is sized from the same strings, so a short write means something went
   wrong and the fingerprint would be wrong too: report it as a failure. */
static char *join_args(const str_list *argv) {
    size_t total = 1;
    for(size_t i = 0; i < str_list_count(argv); i++)
        total += strlen(str_list_get(argv, i)) + 1;
    char *out = malloc(total);
    if(out == NULL)
        return NULL;
    size_t pos = 0;
    for(size_t i = 0; i < str_list_count(argv); i++) {
        int written =
            snprintf(out + pos, total - pos, "%s%s", i > 0 ? " " : "", str_list_get(argv, i));
        if(written < 0 || (size_t)written >= total - pos) {
            free(out);
            return NULL;
        }
        pos += (size_t)written;
    }
    return out;
}

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

/* Run a command held in a str_list argv (adds the NULL terminator), exporting
   the project's [env] variables to the child. */
static int run_str_argv(const str_list *argv, const project_env *env) {
    size_t count = str_list_count(argv);
    const char **cargv = (const char **)malloc((count + 1) * sizeof(char *));
    if(cargv == NULL)
        return -1;
    for(size_t i = 0; i < count; i++)
        cargv[i] = str_list_get(argv, i);
    cargv[count] = NULL;

    process_env_var vars[PROJECT_MAX_ENV];
    size_t var_count = project_env_to_vars(env, vars, PROJECT_MAX_ENV);
    int status = process_run_env(cargv, vars, var_count);
    free((void *)cargv);
    return status;
}

/* Build the full compile command for one source into `argv` (a str_list):
   driver, -c, source, -o, object, -O<n>, [-g], [-std], base+profile defines/
   includes/flags, -MMD -MF depfile, and the src include flag. */
static bool build_compile_argv(str_list *argv, const char *root, const char *source,
                               const char *object, const manifest_profile *settings,
                               const project_target *target, const project_options *profile_opts,
                               const project_options *extra_opts, const str_list *include_flags,
                               const char *depfile, const resolved_toolchain *chain) {
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
        ok = compile_flags_push_std(argv, target, is_cpp);
    if(ok)
        ok = compile_flags_push_options(argv, root, &target->options);
    /* Absent for a dependency, which is compiled against the language standard
       and its own recipe and nothing else the project chose. */
    if(ok && profile_opts != NULL)
        ok = compile_flags_push_options(argv, root, profile_opts);
    /* Extra scope, used by tests: applied last so it can add to the rest. */
    if(ok && extra_opts != NULL)
        ok = compile_flags_push_options(argv, root, extra_opts);
    if(ok)
        ok = str_list_push(argv, ARG_DEPFILE_GEN) && str_list_push(argv, ARG_DEPFILE_OUT) &&
             str_list_push(argv, depfile);
    /* Pre-composed "-I" flags: the project's own src/, then one per include
       directory a dependency exports. They are composed by the caller rather
       than stored in project_options because a dependency's is an absolute
       path into the cache, and a manifest option is sized for "-DFOO=1". */
    for(size_t i = 0; ok && i < str_list_count(include_flags); i++)
        ok = str_list_push(argv, str_list_get(include_flags, i));
    return ok;
}

/* Build the current compile command for a source as a heap string, for the
   fingerprint comparison (caller frees). NULL on allocation failure. */
static char *compile_command_string(const char *root, const char *source, const char *object,
                                    const manifest_profile *settings, const project_target *target,
                                    const project_options *profile_opts,
                                    const project_options *extra_opts,
                                    const str_list *include_flags,
                                    const resolved_toolchain *chain) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    if(!depfile_path_for(object, depfile, sizeof depfile))
        return NULL;
    str_list argv;
    str_list_init(&argv);
    char *command = NULL;
    if(build_compile_argv(&argv, root, source, object, settings, target, profile_opts, extra_opts,
                          include_flags, depfile, chain))
        command = join_args(&argv);
    str_list_free(&argv);
    return command;
}

/* Compile a single translation unit to `object`. gcc writes the header
   dependency file (`-MMD -MF <object>.d`) as a side effect; it is absorbed into
   the WSDB afterwards, on the main thread. */
static bool compile_one(const char *root, const char *source, const char *object,
                        const manifest_profile *settings, const project_target *target,
                        const project_options *profile_opts, const project_options *extra_opts,
                        const str_list *include_flags, const project_env *env,
                        const resolved_toolchain *chain) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    if(!depfile_path_for(object, depfile, sizeof depfile))
        return false;
    str_list argv;
    str_list_init(&argv);
    if(!build_compile_argv(&argv, root, source, object, settings, target, profile_opts, extra_opts,
                           include_flags, depfile, chain)) {
        str_list_free(&argv);
        return false;
    }
    bool ok = run_str_argv(&argv, env) == 0;
    str_list_free(&argv);
    return ok;
}

/* Record a freshly compiled object into the WSDB: read the prerequisites from
   gcc's depfile (falling back to just the source), store {command, prereqs},
   then delete the now-absorbed depfile. Runs on the main thread. Returns false
   if the object could not be recorded, which only costs a rebuild next time. */
[[nodiscard]] static bool wsdb_absorb_object(wsdb *db, const char *source, const char *object,
                                             const char *command) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    if(!depfile_path_for(object, depfile, sizeof depfile))
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
    if(depfile_path_for(object, depfile, sizeof depfile))
        remove(depfile);
}

/* Load and parse `root/Project.toml` into a project context, reporting the
   detailed parse error to stderr on failure. */
static int load_project(const char *root, project_ctx *out) {
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
static void warn_if_not_saved(wsdb *db) {
    if(!wsdb_close(db))
        fprintf(stderr, "molto: warning: could not save the workspace database; "
                        "the next build will not be incremental\n");
}

/* One parallel compilation task: compile `source` into `object`, recording a
   shared failure flag. Runs on a task_pool worker. */
typedef struct {
    const char *root;
    const char *source;
    const char *object;
    const manifest_profile *settings;
    const str_list *include_flags;
    const project_target *target;
    const project_options *profile_opts;
    const project_options *extra_opts;
    const project_env *env;
    const resolved_toolchain *chain;
    atomic_bool *failed;
    bool succeeded;     /* written only by the worker owning this task */
    uint64_t signature; /* what the source was when this compilation began */
} compile_task;

static void compile_task_run(void *arg) {
    compile_task *task = arg;
    task->succeeded = compile_one(task->root, task->source, task->object, task->settings,
                                  task->target, task->profile_opts, task->extra_opts,
                                  task->include_flags, task->env, task->chain);
    if(!task->succeeded) {
        fprintf(stderr, "molto: failed to compile '%s'\n", task->source);
        atomic_store(task->failed, true);
    }
}

/* Compile every source. Phase 1 (sequential) resolves object paths and asks the
   WSDB which units are stale; phase 2 compiles them in parallel; phase 3 (back
   on this thread) records the results into the WSDB. Reports whether C++ is
   present and whether anything was recompiled. */
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

static int compile_sources(const char *root, build_profile profile,
                           const manifest_profile *settings, const str_list *include_flags,
                           const project_target *target, const project_options *profile_opts,
                           const project_options *extra_opts, const project_env *env,
                           const resolved_toolchain *chain, wsdb *db, const str_list *sources,
                           str_list *objects, bool *any_cpp, bool *any_compiled) {
    size_t count = str_list_count(sources);
    /* `objects` accumulates across the passes a build makes — dependencies
       first, then the project — so this call's entries begin where the list
       already stood. Indexing it from zero would hand one source another's
       object, and the second pass would overwrite what the first produced. */
    const size_t objects_base = str_list_count(objects);
    bool *needs = calloc(count, sizeof(bool));
    if(needs == NULL)
        return exit_build_failure;

    /* Phase 1: resolve object paths and ask the WSDB what is stale. Finishing
       all str_list_push here keeps the object pointers stable for phase 2. */
    for(size_t i = 0; i < count; i++) {
        const char *source = str_list_get(sources, i);
        if(source_is_cpp(source))
            *any_cpp = true;
        char object[PATH_BUFFER_SIZE];
        if(!object_path_for(root, profile_name(profile), source, object, sizeof object)) {
            free(needs);
            return exit_build_failure;
        }
        if(!make_parent_dirs(object)) {
            fprintf(stderr, "molto: could not create output directory for '%s'\n", object);
            free(needs);
            return exit_build_failure;
        }
        if(!str_list_push(objects, object)) {
            free(needs);
            return exit_build_failure;
        }
        char *command = compile_command_string(root, source, object, settings, target, profile_opts,
                                               extra_opts, include_flags, chain);
        needs[i] = command == NULL || !wsdb_object_fresh(db, object, command);

        /* A stale object that another project already compiled the same way is
           not compiled again: it is copied out of the shared cache and
           recorded as if it had been. Only a dependency qualifies, because
           only a dependency's tree is immutable enough for a coordinate to
           answer for its contents. */
        if(needs[i] && command != NULL)
            needs[i] = !take_from_object_cache(db, source, object, command);
        free(command);
    }

    size_t to_build = 0;
    for(size_t i = 0; i < count; i++)
        to_build += needs[i] ? 1 : 0;
    if(to_build == 0) {
        *any_compiled = false;
        free(needs);
        return exit_ok;
    }

    /* Phase 2: compile the stale units concurrently (no WSDB access here). */
    compile_task *tasks = calloc(to_build, sizeof(compile_task));
    task_pool *pool = task_pool_create(0);
    if(tasks == NULL || pool == NULL) {
        free(tasks);
        task_pool_destroy(pool);
        free(needs);
        return exit_build_failure;
    }

    atomic_bool failed = false;
    int result = exit_ok;
    size_t queued = 0;
    for(size_t i = 0; i < count && result == exit_ok; i++) {
        if(!needs[i])
            continue;
        tasks[queued] = (compile_task){
            .root = root,
            .source = str_list_get(sources, i),
            .object = str_list_get(objects, objects_base + i),
            .settings = settings,
            .include_flags = include_flags,
            .target = target,
            .profile_opts = profile_opts,
            .extra_opts = extra_opts,
            .env = env,
            .chain = chain,
            .failed = &failed,
            .signature = fs_signature(str_list_get(sources, i)),
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
        if(!task->succeeded) {
            discard_depfile(task->object);
            continue;
        }
        /* A source edited while it was being compiled would otherwise be
           recorded under the signature of content the object does not contain,
           and nothing would rebuild it afterwards: the stale object simply gets
           linked. Leaving it unrecorded costs one recompilation. */
        if(fs_signature(task->source) != task->signature) {
            discard_depfile(task->object);
            continue;
        }
        char *command = compile_command_string(root, task->source, task->object, settings, target,
                                               profile_opts, extra_opts, include_flags, chain);
        if(command == NULL || !wsdb_absorb_object(db, task->source, task->object, command))
            fprintf(stderr, "molto: warning: could not record '%s' as up to date\n", task->source);
        if(command != NULL)
            share_in_object_cache(task->source, task->object, command);
        free(command);
    }

    *any_compiled = true;
    free(tasks);
    free(needs);
    return result;
}

/* Return true if the executable must be re-linked: it is missing or older
   than at least one object file. */
static bool link_needed(const str_list *objects, const char *binary) {
    for(size_t i = 0; i < str_list_count(objects); i++) {
        if(fs_source_newer(str_list_get(objects, i), binary))
            return true;
    }
    return false;
}

/* Build the link command into `argv`: linker, objects, raw flags (base +
   profile, so -flto/-fsanitize reach the linker too), -o binary, and the
   system libraries (-l<lib>). */
static bool build_link_argv(str_list *argv, bool any_cpp, const str_list *objects,
                            const char *binary, const project_target *target,
                            const project_options *profile_opts, const resolved_toolchain *chain) {
    const char *driver = compile_flags_driver(chain, any_cpp);
    if(driver == NULL) {
        fprintf(stderr, "molto: '%s' needs a C++ compiler and none was resolved\n", binary);
        return false;
    }
    bool ok = str_list_push(argv, driver);
    for(size_t i = 0; ok && i < str_list_count(objects); i++)
        ok = str_list_push(argv, str_list_get(objects, i));
    for(size_t i = 0; ok && i < target->options.flag_count; i++)
        ok = str_list_push(argv, target->options.flags[i]);
    for(size_t i = 0; ok && i < profile_opts->flag_count; i++)
        ok = str_list_push(argv, profile_opts->flags[i]);
    if(ok)
        ok = str_list_push(argv, ARG_OUTPUT) && str_list_push(argv, binary);
    for(size_t i = 0; ok && i < target->link_count; i++)
        ok = compile_flags_push_prefixed(argv, LINK_FLAG_PREFIX, target->link[i]);
    return ok;
}

/* Link `objects` into `binary` when needed — `force` (something recompiled),
   a stale/missing binary, or a changed link command (per the WSDB). Records the
   link command in the WSDB. Returns false only if a needed link failed. */
static bool link_project(bool any_cpp, const str_list *objects, const char *binary,
                         const project_target *target, const project_options *profile_opts,
                         const project_env *env, const resolved_toolchain *chain, bool force,
                         wsdb *db) {
    str_list argv;
    str_list_init(&argv);
    if(!build_link_argv(&argv, any_cpp, objects, binary, target, profile_opts, chain)) {
        str_list_free(&argv);
        return false;
    }
    char *command = join_args(&argv);

    bool ok = true;
    if(force || command == NULL || !wsdb_binary_fresh(db, binary, command) ||
       link_needed(objects, binary)) {
        ok = run_str_argv(&argv, env) == 0;
        if(ok && (command == NULL || !wsdb_record_binary(db, binary, command)))
            fprintf(stderr, "molto: warning: could not record '%s' as up to date\n", binary);
    }
    free(command);
    str_list_free(&argv);
    return ok;
}

/* The "-I" flags a compile line carries: the project's own src/, then one per
   include directory a dependency exports.

   Composed into a list rather than merged into project_options because a
   dependency's include is an absolute path into the shared cache, and a
   manifest option is sized for "-DFOO=1" — RFC-0003 caps one at 95 characters,
   which a real cache path exceeds. */
[[nodiscard]] static bool compose_include_flags(const char *src_dir, const str_list *dep_includes,
                                                str_list *out) {
    char flag[PATH_BUFFER_SIZE + 4];
    if(!fs_format_path(flag, sizeof flag, INCLUDE_FLAG_FORMAT, src_dir))
        return fs_report_long_path(src_dir);
    if(!str_list_push(out, flag))
        return false;

    for(size_t i = 0; dep_includes != NULL && i < str_list_count(dep_includes); i++) {
        const char *directory = str_list_get(dep_includes, i);
        if(!fs_format_path(flag, sizeof flag, INCLUDE_FLAG_FORMAT, directory))
            return fs_report_long_path(directory);
        if(!str_list_push(out, flag))
            return false;
    }
    return true;
}

/* Append one entry to a fixed-size option array, refusing to overflow it: a
   dropped define or library produces a green build of something else. */
[[nodiscard]] static bool append_option(char dest[][PROJECT_OPT_LEN], size_t *count,
                                        size_t capacity, const char *value, const char *what) {
    if(*count >= capacity) {
        fprintf(stderr, "molto: %s has more than %zu entries once dependencies are added\n", what,
                capacity);
        return false;
    }
    if(!fs_format_path(dest[*count], PROJECT_OPT_LEN, "%s", value))
        return fs_report_long_path(value);
    (*count)++;
    return true;
}

/* What a dependency is compiled with: its own defines and flags, and its own
   include directories as pre-composed "-I" flags. Deliberately not the
   project's: see the call site. */
[[nodiscard]] static bool collect_dep_options(const prepared_deps *deps, project_options *options,
                                              str_list *include_flags) {
    for(size_t i = 0; i < str_list_count(&deps->defines); i++) {
        if(!append_option(options->defines, &options->define_count, PROJECT_MAX_OPTS,
                          str_list_get(&deps->defines, i), "a dependency's defines"))
            return false;
    }
    for(size_t i = 0; i < str_list_count(&deps->flags); i++) {
        if(!append_option(options->flags, &options->flag_count, PROJECT_MAX_OPTS,
                          str_list_get(&deps->flags, i), "a dependency's flags"))
            return false;
    }
    char flag[PATH_BUFFER_SIZE + 4];
    for(size_t i = 0; i < str_list_count(&deps->includes); i++) {
        const char *directory = str_list_get(&deps->includes, i);
        if(!fs_format_path(flag, sizeof flag, INCLUDE_FLAG_FORMAT, directory))
            return fs_report_long_path(directory);
        if(!str_list_push(include_flags, flag))
            return false;
    }
    return true;
}

/* Fold what the dependencies contribute into `[target]`.
 *
 * A dependency's include directories, defines, flags and libraries are exactly
 * the things `[target]` already carries, and everything downstream — the
 * compile line, the link line, `molto lint`, the test build — reads them from
 * there. Merging here means none of those has to learn what a dependency is.
 */
[[nodiscard]] static bool merge_deps(project_ctx *ctx, const prepared_deps *deps) {
    /* The package name and version live past the manifest's own limit, so a
       dependency may not take their slots. */
    for(size_t i = 0; i < str_list_count(&deps->defines); i++) {
        if(!append_option(ctx->target.options.defines, &ctx->target.options.define_count,
                          PROJECT_MAX_OPTS + PROJECT_PKG_DEFINES, str_list_get(&deps->defines, i),
                          "[target].defines"))
            return false;
    }
    for(size_t i = 0; i < str_list_count(&deps->flags); i++) {
        if(!append_option(ctx->target.options.flags, &ctx->target.options.flag_count,
                          PROJECT_MAX_OPTS, str_list_get(&deps->flags, i), "[target].flags"))
            return false;
    }
    for(size_t i = 0; i < str_list_count(&deps->links); i++) {
        const char *library = str_list_get(&deps->links, i);
        if(ctx->target.link_count >= PROJECT_MAX_LINK) {
            fprintf(stderr,
                    "molto: [target].link has more than %d entries once dependencies are "
                    "added\n",
                    PROJECT_MAX_LINK);
            return false;
        }
        if(!fs_format_path(ctx->target.link[ctx->target.link_count], PROJECT_LINK_NAME_MAX, "%s",
                           library))
            return fs_report_long_path(library);
        ctx->target.link_count++;
    }
    return true;
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
[[nodiscard]] static bool prepare_and_lock(const char *root, project_ctx *ctx, prepared_deps *out,
                                           char *err, size_t err_size) {
    if(ctx->deps.count == 0)
        return true;

    dep_graph *graph = NULL;
    if(!dep_graph_resolve(ctx, &graph, err, err_size))
        return false;

    bool ok = deps_prepare_graph(graph, out, err, err_size);
    /* Failing to record a resolution that succeeded must not fail the build:
       the objects are correct either way, and the cost is that the next build
       resolves again. Silence would be worse — a lock file nobody can write is
       a reproducibility guarantee nobody has. */
    if(ok && !lockfile_write(root, ctx->project_name, graph, err, err_size)) {
        fprintf(stderr, "molto: warning: %s\n", err);
        err[0] = '\0';
    }

    dep_graph_free(graph);
    return ok;
}

/* Load the manifest and compile every source under `root/src` into `objects`
   (caller-initialised, caller-freed). Reports whether C++ is present and whether
   anything was recompiled. Shared by build_project and build_tests. */
static int compile_project(const char *root, build_profile profile, wsdb *db,
                           bool refresh_toolchain, project_ctx *ctx_out,
                           resolved_toolchain *chain_out, str_list *objects_out,
                           str_list *include_flags_out, bool *any_cpp_out, bool *any_compiled_out) {
    int result = load_project(root, ctx_out);
    if(result != exit_ok)
        return result;

    manifest_profile settings = profile_settings(ctx_out, profile);

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
    prepared_deps deps;
    prepared_deps_init(&deps);
    char deps_err[512] = "";
    if(!prepare_and_lock(root, ctx_out, &deps, deps_err, sizeof deps_err)) {
        fprintf(stderr, "molto: %s\n", deps_err);
        prepared_deps_free(&deps);
        return exit_dependency_failure;
    }

    str_list sources;
    str_list_init(&sources);
    if(!source_discovery_collect(src_dir, &sources)) {
        fprintf(stderr, "molto: could not read the sources under '%s'\n", src_dir);
        str_list_free(&sources);
        prepared_deps_free(&deps);
        return exit_build_failure;
    }

    /* The dependencies' own compile settings, kept apart from the project's.
       They are what a dependency is compiled with, and merging the two would
       hand it the consumer's defines and put the consumer's src/ on its
       include path — where a dependency's `#include "config.h"` would find
       the application's. It also makes the same dependency compile
       identically in every project, which is what lets one object be shared. */
    project_options dep_options;
    memset(&dep_options, 0, sizeof dep_options);
    str_list dep_include_flags;
    str_list_init(&dep_include_flags);

    bool merged = collect_dep_options(&deps, &dep_options, &dep_include_flags) &&
                  merge_deps(ctx_out, &deps) &&
                  compose_include_flags(src_dir, &deps.includes, include_flags_out);
    str_list dep_sources;
    str_list_init(&dep_sources);
    for(size_t i = 0; merged && i < str_list_count(&deps.sources); i++)
        merged = str_list_push(&dep_sources, str_list_get(&deps.sources, i));
    prepared_deps_free(&deps);
    if(!merged) {
        str_list_free(&sources);
        str_list_free(&dep_sources);
        str_list_free(&dep_include_flags);
        return exit_dependency_failure;
    }
    if(str_list_count(&sources) == 0) {
        fprintf(stderr, "molto: no source files found under '%s'\n", src_dir);
        str_list_free(&sources);
        str_list_free(&dep_sources);
        str_list_free(&dep_include_flags);
        return exit_build_failure;
    }

    /* Which compiler to use is settled once per build, after the sources are
       known: a project with C++ in it needs a toolchain that has a C++ driver,
       and that is part of the question. */
    bool needs_cpp = false;
    for(size_t i = 0; i < str_list_count(&sources); i++)
        needs_cpp = needs_cpp || source_is_cpp(str_list_get(&sources, i));
    for(size_t i = 0; i < str_list_count(&dep_sources); i++)
        needs_cpp = needs_cpp || source_is_cpp(str_list_get(&dep_sources, i));
    result = toolchain_resolve(&ctx_out->target, needs_cpp, db, refresh_toolchain, chain_out);
    if(result != exit_ok) {
        str_list_free(&sources);
        str_list_free(&dep_sources);
        str_list_free(&dep_include_flags);
        return result;
    }

    *any_cpp_out = false;
    *any_compiled_out = false;

    /* Dependencies first, and in their own pass: the target they are compiled
       against carries the language standard and nothing of the project's, so
       what reaches the compiler is the same in every project that depends on
       them — which is what makes one compiled object worth sharing. */
    if(str_list_count(&dep_sources) > 0) {
        project_target dep_target = ctx_out->target;
        memset(&dep_target.options, 0, sizeof dep_target.options);
        dep_target.link_count = 0;
        bool dep_cpp = false;
        bool dep_compiled = false;
        result = compile_sources(root, profile, &settings, &dep_include_flags, &dep_target, NULL,
                                 &dep_options, &ctx_out->env, chain_out, db, &dep_sources,
                                 objects_out, &dep_cpp, &dep_compiled);
        *any_cpp_out = *any_cpp_out || dep_cpp;
        *any_compiled_out = *any_compiled_out || dep_compiled;
    }
    str_list_free(&dep_sources);
    str_list_free(&dep_include_flags);

    if(result == exit_ok) {
        bool project_cpp = false;
        bool project_compiled = false;
        result =
            compile_sources(root, profile, &settings, include_flags_out, &ctx_out->target,
                            profile_options_for(ctx_out, profile), NULL, &ctx_out->env, chain_out,
                            db, &sources, objects_out, &project_cpp, &project_compiled);
        *any_cpp_out = *any_cpp_out || project_cpp;
        *any_compiled_out = *any_compiled_out || project_compiled;
    }
    str_list_free(&sources);
    return result;
}

int build_project(const char *root, build_profile profile, bool refresh_toolchain, char *out_binary,
                  size_t out_binary_size) {
    wsdb *db = wsdb_open(root);
    if(db == NULL) {
        fprintf(stderr, "molto: could not open the workspace database (locked?)\n");
        return exit_build_failure;
    }

    project_ctx ctx;
    resolved_toolchain chain;
    str_list objects;
    str_list include_flags;
    str_list_init(&objects);
    str_list_init(&include_flags);
    bool any_cpp = false;
    bool any_compiled = false;
    int result = compile_project(root, profile, db, refresh_toolchain, &ctx, &chain, &objects,
                                 &include_flags, &any_cpp, &any_compiled);

    if(result == exit_ok) {
        char binary[PATH_BUFFER_SIZE];
        if(!compose_binary_path(root, profile, ctx.project_name, binary, sizeof binary)) {
            str_list_free(&objects);
            str_list_free(&include_flags);
            (void)wsdb_close(db);
            return exit_build_failure;
        }
        if(!link_project(any_cpp, &objects, binary, &ctx.target, profile_options_for(&ctx, profile),
                         &ctx.env, &chain, any_compiled, db)) {
            fprintf(stderr, "molto: failed to link '%s'\n", binary);
            result = exit_build_failure;
        }
        if(result == exit_ok) {
            /* Prune objects orphaned by removed sources (scoped to src/). */
            char prefix[PATH_BUFFER_SIZE];
            if(fs_format_path(prefix, sizeof prefix, "%s/" DIR_BUILD "/%s/" DIR_OBJ "/" DIR_SRC "/",
                              root, profile_name(profile)))
                wsdb_prune(db, &objects, prefix);
            if(out_binary != NULL && !fs_format_path(out_binary, out_binary_size, "%s", binary)) {
                (void)fs_report_long_path(binary);
                result = exit_build_failure;
            }
        }
    }

    str_list_free(&objects);
    str_list_free(&include_flags);
    warn_if_not_saved(db);
    return result;
}

/* Portion of `path` relative to `root` (drops a leading "root/"), or `path`
   unchanged if it is not under root. */
static const char *relative_to_root(const char *root, const char *path) {
    size_t root_len = strlen(root);
    if(strncmp(path, root, root_len) == 0 && path[root_len] == '/')
        return path + root_len + 1;
    return path;
}

/* Output path of a test executable: build/<profile>/tests/<name>, mirroring the
   test source's path under tests/ with its extension stripped. */
[[nodiscard]] static bool test_binary_path(const char *root, const char *profile_dir,
                                           const char *test_source, char *out, size_t out_size) {
    char stem[PATH_BUFFER_SIZE];
    if(!fs_format_path(stem, sizeof stem, "%s", relative_to_root(root, test_source)))
        return fs_report_long_path(test_source);
    char *dot = strrchr(stem, '.');
    char *slash = strrchr(stem, '/');
    if(dot != NULL && (slash == NULL || dot > slash))
        *dot = '\0';
    return fs_format_path(out, out_size, "%s/" DIR_BUILD "/%s/%s", root, profile_dir, stem) ||
           fs_report_long_path(test_source);
}

/* Collect what the tests are built from: everything under tests/, plus the
   extra sources the manifest lists. A listed directory is walked; a listed
   file is taken as it is. This is how a framework living outside src/ — with
   the main() the tests do not have — gets compiled in. */
static bool collect_test_sources(const char *root, const project_ctx *ctx, const char *tests_dir,
                                 str_list *out) {
    /* A missing or empty tests/ is not an error: there is simply nothing. */
    if(fs_is_dir(tests_dir) && !source_discovery_collect(tests_dir, out)) {
        fprintf(stderr, "molto: could not read the tests under '%s'\n", tests_dir);
        return false;
    }

    for(size_t i = 0; i < ctx->test.source_count; i++) {
        const char *entry = ctx->test.sources[i];
        char path[PATH_BUFFER_SIZE];
        bool composed = entry[0] == '/' ? fs_format_path(path, sizeof path, "%s", entry)
                                        : fs_format_path(path, sizeof path, "%s/%s", root, entry);
        if(!composed)
            return fs_report_long_path(entry);

        if(fs_is_dir(path)) {
            if(!source_discovery_collect(path, out)) {
                fprintf(stderr, "molto: could not read [test].sources '%s'\n", entry);
                return false;
            }
        } else if(fs_path_exists(path)) {
            if(!str_list_push(out, path))
                return false;
        } else {
            fprintf(stderr, "molto: [test].sources '%s' does not exist\n", entry);
            return false;
        }
    }
    return true;
}

/* Everything a test link needs beyond its own objects. */
typedef struct {
    const char *root;
    const char *profile_dir;
    const project_ctx *ctx;
    const project_options *profile_opts;
    const resolved_toolchain *chain;
    const str_list *lib_objects; /* src objects, minus the app's main */
    bool any_cpp;
    bool force; /* something was recompiled */
    wsdb *db;
} test_link_context;

/* Link `objects` into `binary`, and record it as one of the built tests. */
static bool link_one_test(const test_link_context *context, const str_list *objects,
                          const char *binary, bool cpp, str_list *binaries_out) {
    if(!make_parent_dirs(binary))
        return false;
    if(!link_project(cpp, objects, binary, &context->ctx->target, context->profile_opts,
                     &context->ctx->env, context->chain, context->force, context->db)) {
        fprintf(stderr, "molto: failed to link '%s'\n", binary);
        return false;
    }
    return str_list_push(binaries_out, binary);
}

/* One executable per test file: each links its own object with the project's
   library objects, and brings its own main(). */
static int link_tests_per_file(const test_link_context *context, const str_list *test_sources,
                               const str_list *test_objects, str_list *binaries_out) {
    for(size_t i = 0; i < str_list_count(test_sources); i++) {
        const char *source = str_list_get(test_sources, i);
        const char *object = str_list_get(test_objects, i);

        char binary[PATH_BUFFER_SIZE];
        if(!test_binary_path(context->root, context->profile_dir, source, binary, sizeof binary))
            return exit_build_failure;

        str_list link_objects;
        str_list_init(&link_objects);
        bool ok = str_list_push(&link_objects, object);
        for(size_t j = 0; ok && j < str_list_count(context->lib_objects); j++)
            ok = str_list_push(&link_objects, str_list_get(context->lib_objects, j));
        ok = ok && link_one_test(context, &link_objects, binary,
                                 context->any_cpp || source_is_cpp(source), binaries_out);
        str_list_free(&link_objects);
        if(!ok)
            return exit_build_failure;
    }
    return exit_ok;
}

/* One executable for the whole suite: every test object, the extra sources,
   and the project's library objects. The main() comes from those extra
   sources, which is what a framework that registers its cases provides. */
static int link_tests_single(const test_link_context *context, const str_list *test_sources,
                             const str_list *test_objects, str_list *binaries_out) {
    if(str_list_count(test_objects) == 0)
        return exit_ok; /* nothing to link */

    char binary[PATH_BUFFER_SIZE];
    if(!fs_format_path(binary, sizeof binary, "%s/" DIR_BUILD "/%s/" DIR_TESTS "/%s%s",
                       context->root, context->profile_dir, context->ctx->project_name,
                       TEST_SUITE_SUFFIX)) {
        (void)fs_report_long_path(context->ctx->project_name);
        return exit_build_failure;
    }

    str_list link_objects;
    str_list_init(&link_objects);
    bool ok = true;
    bool cpp = context->any_cpp;
    for(size_t i = 0; ok && i < str_list_count(test_objects); i++) {
        ok = str_list_push(&link_objects, str_list_get(test_objects, i));
        cpp = cpp || source_is_cpp(str_list_get(test_sources, i));
    }
    for(size_t i = 0; ok && i < str_list_count(context->lib_objects); i++)
        ok = str_list_push(&link_objects, str_list_get(context->lib_objects, i));

    ok = ok && link_one_test(context, &link_objects, binary, cpp, binaries_out);
    str_list_free(&link_objects);
    return ok ? exit_ok : exit_build_failure;
}

int build_tests(const char *root, build_profile profile, bool refresh_toolchain,
                str_list *test_binaries_out) {
    wsdb *db = wsdb_open(root);
    if(db == NULL) {
        fprintf(stderr, "molto: could not open the workspace database (locked?)\n");
        return exit_build_failure;
    }

    project_ctx ctx;
    resolved_toolchain chain;
    str_list objects;
    str_list include_flags;
    str_list_init(&objects);
    str_list_init(&include_flags);
    bool any_cpp = false;
    bool any_compiled = false;
    int result = compile_project(root, profile, db, refresh_toolchain, &ctx, &chain, &objects,
                                 &include_flags, &any_cpp, &any_compiled);
    if(result != exit_ok) {
        str_list_free(&objects);
        str_list_free(&include_flags);
        warn_if_not_saved(db);
        return result;
    }

    manifest_profile settings = profile_settings(&ctx, profile);
    const project_options *profile_opts = profile_options_for(&ctx, profile);
    const char *profile_dir = profile_name(profile);

    /* Object of src/main.c (the app entry point), if any, to exclude from test
       links: the tests supply their own entry point. */
    char main_source[PATH_BUFFER_SIZE];
    char main_object[PATH_BUFFER_SIZE];
    char src_dir[PATH_BUFFER_SIZE];
    char tests_dir[PATH_BUFFER_SIZE];
    if(!fs_format_path(main_source, sizeof main_source, "%s/" DIR_SRC "/main.c", root) ||
       !object_path_for(root, profile_dir, main_source, main_object, sizeof main_object) ||
       !fs_format_path(src_dir, sizeof src_dir, "%s/" DIR_SRC, root) ||
       !fs_format_path(tests_dir, sizeof tests_dir, "%s/" DIR_TESTS, root)) {
        (void)fs_report_long_path(root);
        str_list_free(&objects);
        str_list_free(&include_flags);
        warn_if_not_saved(db);
        return exit_build_failure;
    }
    bool has_main = fs_path_exists(main_source);

    /* Library objects = every src object except the app's main object. */
    str_list lib_objects;
    str_list_init(&lib_objects);
    for(size_t i = 0; i < str_list_count(&objects) && result == exit_ok; i++) {
        const char *object = str_list_get(&objects, i);
        if(has_main && strcmp(object, main_object) == 0)
            continue;
        if(!str_list_push(&lib_objects, object))
            result = exit_build_failure;
    }

    str_list test_sources;
    str_list_init(&test_sources);
    if(result == exit_ok && !collect_test_sources(root, &ctx, tests_dir, &test_sources))
        result = exit_build_failure;

    /* Compiled through the same path as the project's own sources, so tests get
       the parallel build, the dependency tracking and the up-to-date checks
       instead of a second implementation of all three. */
    str_list test_objects;
    str_list_init(&test_objects);
    bool tests_cpp = false;
    bool tests_compiled = false;
    if(result == exit_ok && str_list_count(&test_sources) > 0)
        result = compile_sources(root, profile, &settings, &include_flags, &ctx.target,
                                 profile_opts, &ctx.test.options, &ctx.env, &chain, db,
                                 &test_sources, &test_objects, &tests_cpp, &tests_compiled);

    if(result == exit_ok) {
        const test_link_context context = {
            .root = root,
            .profile_dir = profile_dir,
            .ctx = &ctx,
            .profile_opts = profile_opts,
            .chain = &chain,
            .lib_objects = &lib_objects,
            .any_cpp = any_cpp || tests_cpp,
            .force = any_compiled || tests_compiled,
            .db = db,
        };
        result =
            ctx.test.mode == test_mode_single
                ? link_tests_single(&context, &test_sources, &test_objects, test_binaries_out)
                : link_tests_per_file(&context, &test_sources, &test_objects, test_binaries_out);
    }

    /* A deleted test leaves behind an object and an executable that `molto test`
       would happily keep running. Prune both (RFC-0004). */
    if(result == exit_ok) {
        char prefix[PATH_BUFFER_SIZE];
        if(fs_format_path(prefix, sizeof prefix, "%s/" DIR_BUILD "/%s/" DIR_OBJ "/" DIR_TESTS "/",
                          root, profile_dir))
            wsdb_prune(db, &test_objects, prefix);
        if(fs_format_path(prefix, sizeof prefix, "%s/" DIR_BUILD "/%s/" DIR_TESTS "/", root,
                          profile_dir))
            wsdb_prune(db, test_binaries_out, prefix);
    }

    str_list_free(&test_objects);
    str_list_free(&test_sources);
    str_list_free(&lib_objects);
    str_list_free(&objects);
    str_list_free(&include_flags);
    warn_if_not_saved(db);
    return result;
}
