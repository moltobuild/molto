#include <molto/services/build_service.h>

#include <molto/build/compile_flags.h>
#include <molto/build/depfile.h>
#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/project/lockfile.h>
#include <molto/project/project_ctx.h>
#include <molto/services/conflict_prompt.h>
#include <molto/services/deps_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/manifest_service.h>
#include <molto/services/object_cache.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/progress.h>
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

/*
 * One translation unit, and everything its command line needs that the build
 * as a whole does not share.
 *
 * It exists so that one pass can compile units that do not agree about their
 * flags. A dependency is compiled against its own recipe and the project
 * against its manifest; before this they had to be two passes, which meant two
 * thread pools and a barrier between them for no reason anyone chose.
 */
typedef struct {
    const char *source;
    const project_target *target;
    /* Absent for a dependency, which is compiled against the language standard
       and its own recipe and nothing else the project chose. */
    const project_options *profile_opts;
    /* Applied last, so it can add to the rest. Absent when there is none. */
    const project_options *extra_opts;
    const str_list *include_flags;
} compile_unit;

/* Build the full compile command for one unit into `argv` (a str_list):
   driver, -c, source, -o, object, -O<n>, [-g], [-std], the unit's defines/
   includes/flags, -MMD -MF depfile, and its include flags. */
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
        ok = compile_flags_push_std(argv, unit->target, is_cpp);
    if(ok)
        ok = compile_flags_push_options(argv, root, &unit->target->options);
    if(ok && unit->profile_opts != NULL)
        ok = compile_flags_push_options(argv, root, unit->profile_opts);
    if(ok && unit->extra_opts != NULL)
        ok = compile_flags_push_options(argv, root, unit->extra_opts);
    if(ok)
        ok = str_list_push(argv, ARG_DEPFILE_GEN) && str_list_push(argv, ARG_DEPFILE_OUT) &&
             str_list_push(argv, depfile);
    /* Pre-composed "-I" flags: the project's own src/, then one per include
       directory a dependency exports. They are composed by the caller rather
       than stored in project_options because a dependency's is an absolute
       path into the cache, and a manifest option is sized for "-DFOO=1". */
    for(size_t i = 0; ok && i < str_list_count(unit->include_flags); i++)
        ok = str_list_push(argv, str_list_get(unit->include_flags, i));
    return ok;
}

/* Build the current compile command for a unit as a heap string, for the
   fingerprint comparison (caller frees). NULL on allocation failure. */
static char *compile_command_string(const char *root, const compile_unit *unit, const char *object,
                                    const manifest_profile *settings,
                                    const resolved_toolchain *chain) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    if(!depfile_path_for(object, depfile, sizeof depfile))
        return NULL;
    str_list argv;
    str_list_init(&argv);
    char *command = NULL;
    if(build_compile_argv(&argv, root, unit, object, settings, depfile, chain))
        command = join_args(&argv);
    str_list_free(&argv);
    return command;
}

/* Compile a single translation unit to `object`. gcc writes the header
   dependency file (`-MMD -MF <object>.d`) as a side effect; it is absorbed into
   the WSDB afterwards, on the main thread. */
static bool compile_one(const char *root, const compile_unit *unit, const char *object,
                        const manifest_profile *settings, const project_env *env,
                        const resolved_toolchain *chain) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    if(!depfile_path_for(object, depfile, sizeof depfile))
        return false;
    str_list argv;
    str_list_init(&argv);
    if(!build_compile_argv(&argv, root, unit, object, settings, depfile, chain)) {
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

/* One parallel compilation task: compile `unit` into `object`, recording a
   shared failure flag. Runs on a task_pool worker. */
typedef struct {
    const char *root;
    const compile_unit *unit;
    const char *object;
    const manifest_profile *settings;
    const project_env *env;
    const resolved_toolchain *chain;
    atomic_bool *failed;
    bool succeeded;     /* written only by the worker owning this task */
    uint64_t signature; /* what the source was when this compilation began */
} compile_task;

static void compile_task_run(void *arg) {
    compile_task *task = arg;
    task->succeeded =
        compile_one(task->root, task->unit, task->object, task->settings, task->env, task->chain);
    if(!task->succeeded) {
        fprintf(stderr, "molto: failed to compile '%s'\n", task->unit->source);
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

/* Every source in `sources` as a unit with one command line: what a pass over
   one package's own code needs, where nothing disagrees about its flags.

   The units borrow `sources` and everything else handed in, so all of it has to
   outlive them. Caller frees. NULL means the allocation failed, so callers with
   nothing to compile do not ask. */
[[nodiscard]] static compile_unit *units_from(const str_list *sources, const project_target *target,
                                              const project_options *profile_opts,
                                              const project_options *extra_opts,
                                              const str_list *include_flags) {
    compile_unit *units = calloc(str_list_count(sources), sizeof *units);
    if(units == NULL)
        return NULL;
    for(size_t i = 0; i < str_list_count(sources); i++) {
        units[i] = (compile_unit){
            .source = str_list_get(sources, i),
            .target = target,
            .profile_opts = profile_opts,
            .extra_opts = extra_opts,
            .include_flags = include_flags,
        };
    }
    return units;
}

static int compile_units(const char *root, build_profile profile, const manifest_profile *settings,
                         const project_env *env, const resolved_toolchain *chain, wsdb *db,
                         const compile_unit *units, size_t count, str_list *objects, bool *any_cpp,
                         bool *any_compiled) {
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
        const char *source = units[i].source;
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
        char *command = compile_command_string(root, &units[i], object, settings, chain);
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
            .unit = &units[i],
            .object = str_list_get(objects, objects_base + i),
            .settings = settings,
            .env = env,
            .chain = chain,
            .failed = &failed,
            .signature = fs_signature(units[i].source),
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
        const char *source = task->unit->source;
        if(fs_signature(source) != task->signature) {
            discard_depfile(task->object);
            continue;
        }
        char *command = compile_command_string(root, task->unit, task->object, settings, chain);
        if(command == NULL || !wsdb_absorb_object(db, source, task->object, command))
            fprintf(stderr, "molto: warning: could not record '%s' as up to date\n", source);
        if(command != NULL)
            share_in_object_cache(source, task->object, command);
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

/* What one dependency is compiled with: its own defines and flags, and its own
   include directories as pre-composed "-I" flags. Deliberately not the
   project's, and no longer every other dependency's either: see the call
   site. */
[[nodiscard]] static bool collect_unit_options(const prepared_unit *unit, project_options *options,
                                               str_list *include_flags) {
    for(size_t i = 0; i < str_list_count(&unit->defines); i++) {
        if(!append_option(options->defines, &options->define_count, PROJECT_MAX_OPTS,
                          str_list_get(&unit->defines, i), "a dependency's defines"))
            return false;
    }
    for(size_t i = 0; i < str_list_count(&unit->flags); i++) {
        if(!append_option(options->flags, &options->flag_count, PROJECT_MAX_OPTS,
                          str_list_get(&unit->flags, i), "a dependency's flags"))
            return false;
    }
    char flag[PATH_BUFFER_SIZE + 4];
    for(size_t i = 0; i < str_list_count(&unit->includes); i++) {
        const char *directory = str_list_get(&unit->includes, i);
        if(!fs_format_path(flag, sizeof flag, INCLUDE_FLAG_FORMAT, directory))
            return fs_report_long_path(directory);
        if(!str_list_push(include_flags, flag))
            return false;
    }
    return true;
}

/*
 * The pass that compiles dependencies: every source of every dependency, each
 * with the command line its own package asked for.
 *
 * It is one pass and not one per package on purpose. A pass is a thread pool
 * and a barrier at the end of it, so a pass per dependency would compile them
 * one package at a time — the flags become per unit, the parallelism stays
 * whole-build.
 *
 * Everything here borrows the `prepared_deps` it was built from, which must
 * outlive it, and nothing borrows the pass's own arrays, which is why growing
 * them is not allowed once the units point into them.
 */
typedef struct {
    compile_unit *units;
    size_t count;
    project_options *options; /* one per dependency */
    str_list *include_flags;  /* one per dependency */
    size_t package_count;     /* how many of the two above are initialised */
    project_target target;    /* the language standard, and nothing else */
} dep_pass;

static void dep_pass_free(dep_pass *pass) {
    for(size_t i = 0; i < pass->package_count; i++)
        str_list_free(&pass->include_flags[i]);
    free(pass->include_flags);
    free(pass->options);
    free(pass->units);
    memset(pass, 0, sizeof *pass);
}

[[nodiscard]] static bool dep_pass_build(dep_pass *pass, const prepared_deps *deps,
                                         const project_target *base) {
    memset(pass, 0, sizeof *pass);
    /* A dependency is compiled against the language standard and its own
       recipe: the consumer's defines would reach code that never asked for
       them, and the consumer's src/ on the include path is where a
       dependency's `#include "config.h"` finds the application's. It is also
       what makes one dependency compile identically in every project, which is
       what lets one object be shared. */
    pass->target = *base;
    memset(&pass->target.options, 0, sizeof pass->target.options);
    pass->target.link_count = 0;

    size_t total = 0;
    for(size_t i = 0; i < deps->unit_count; i++)
        total += str_list_count(&deps->units[i].sources);
    if(total == 0)
        return true;

    pass->options = calloc(deps->unit_count, sizeof *pass->options);
    pass->include_flags = calloc(deps->unit_count, sizeof *pass->include_flags);
    pass->units = calloc(total, sizeof *pass->units);
    if(pass->options == NULL || pass->include_flags == NULL || pass->units == NULL)
        return false;
    pass->package_count = deps->unit_count;

    for(size_t i = 0; i < deps->unit_count; i++) {
        const prepared_unit *unit = &deps->units[i];
        str_list_init(&pass->include_flags[i]);
        if(!collect_unit_options(unit, &pass->options[i], &pass->include_flags[i]))
            return false;
        for(size_t j = 0; j < str_list_count(&unit->sources); j++) {
            pass->units[pass->count++] = (compile_unit){
                .source = str_list_get(&unit->sources, j),
                .target = &pass->target,
                .profile_opts = NULL,
                .extra_opts = &pass->options[i],
                .include_flags = &pass->include_flags[i],
            };
        }
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
/* What the spinner says while the registry is being asked. */
#define RESOLVE_LABEL "asking the registry"

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
        if(load_project(root, &reloaded) != exit_ok) {
            snprintf(err, err_size, "Project.toml could not be read back after the edit");
            return false;
        }
        *ctx = reloaded;
    }
    return false;
}

[[nodiscard]] static bool prepare_and_lock(const char *root, project_ctx *ctx, prepared_deps *out,
                                           prepared_deps *dev_out, char *err, size_t err_size) {
    if(ctx->deps.count == 0 && ctx->dev_deps.count == 0)
        return true;

    /* Read before resolving, so what comes back can be held against it. A
       missing or stale lock is not an error: there is simply nothing to check
       against, and one is written at the end either way. */
    lockfile lock;
    char lock_err[512] = "";
    const bool locked =
        lockfile_read(root, &lock, lock_err, sizeof lock_err) && lockfile_matches(&lock, ctx);

    dep_graph *graph = NULL;
    if(!resolve_or_ask(root, ctx, &graph, err, err_size)) {
        if(lock.packages != NULL)
            lockfile_free(&lock);
        return false;
    }

    bool ok = !locked || lockfile_verify(&lock, graph, err, err_size);
    if(lock.packages != NULL)
        lockfile_free(&lock);
    if(!ok) {
        dep_graph_free(graph);
        return false;
    }

    ok = deps_prepare_graph(graph, out, err, err_size) &&
         deps_prepare_dev(graph, dev_out, err, err_size);
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
                           str_list *include_flags_out, bool *any_cpp_out, bool *any_compiled_out,
                           prepared_deps *dev_out) {
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
    if(!prepare_and_lock(root, ctx_out, &deps, dev_out, deps_err, sizeof deps_err)) {
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

    /* The interface of the dependencies folds into `[target]`; what each of
       them compiles itself with becomes a pass of its own units. `deps` has to
       outlive that pass, because the units point into it. */
    dep_pass pass = {0};
    const bool merged = merge_deps(ctx_out, &deps) &&
                        compose_include_flags(src_dir, &deps.includes, include_flags_out) &&
                        dep_pass_build(&pass, &deps, &ctx_out->target);
    if(!merged) {
        dep_pass_free(&pass);
        prepared_deps_free(&deps);
        str_list_free(&sources);
        return exit_dependency_failure;
    }
    if(str_list_count(&sources) == 0) {
        fprintf(stderr, "molto: no source files found under '%s'\n", src_dir);
        dep_pass_free(&pass);
        prepared_deps_free(&deps);
        str_list_free(&sources);
        return exit_build_failure;
    }

    /* Which compiler to use is settled once per build, after the sources are
       known: a project with C++ in it needs a toolchain that has a C++ driver,
       and that is part of the question. */
    bool needs_cpp = false;
    for(size_t i = 0; i < str_list_count(&sources); i++)
        needs_cpp = needs_cpp || source_is_cpp(str_list_get(&sources, i));
    for(size_t i = 0; i < pass.count; i++)
        needs_cpp = needs_cpp || source_is_cpp(pass.units[i].source);
    result = toolchain_resolve(&ctx_out->target, needs_cpp, db, refresh_toolchain, chain_out);
    if(result != exit_ok) {
        dep_pass_free(&pass);
        prepared_deps_free(&deps);
        str_list_free(&sources);
        return result;
    }

    *any_cpp_out = false;
    *any_compiled_out = false;

    /* Dependencies first, and in one pass of their own: each is compiled
       against the language standard and its own recipe, so what reaches the
       compiler is the same in every project that depends on it — which is what
       makes one compiled object worth sharing. */
    if(pass.count > 0) {
        bool dep_cpp = false;
        bool dep_compiled = false;
        result = compile_units(root, profile, &settings, &ctx_out->env, chain_out, db, pass.units,
                               pass.count, objects_out, &dep_cpp, &dep_compiled);
        *any_cpp_out = *any_cpp_out || dep_cpp;
        *any_compiled_out = *any_compiled_out || dep_compiled;
    }
    dep_pass_free(&pass);
    prepared_deps_free(&deps);

    if(result == exit_ok) {
        bool project_cpp = false;
        bool project_compiled = false;
        compile_unit *units =
            units_from(&sources, &ctx_out->target, profile_options_for(ctx_out, profile), NULL,
                       include_flags_out);
        result = units == NULL ? exit_build_failure
                               : compile_units(root, profile, &settings, &ctx_out->env, chain_out,
                                               db, units, str_list_count(&sources), objects_out,
                                               &project_cpp, &project_compiled);
        free(units);
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
    /* Development dependencies are resolved here too — they share the graph and
       the version check — but this build links none of them. */
    prepared_deps dev;
    prepared_deps_init(&dev);
    int result = compile_project(root, profile, db, refresh_toolchain, &ctx, &chain, &objects,
                                 &include_flags, &any_cpp, &any_compiled, &dev);
    prepared_deps_free(&dev);

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
    prepared_deps dev;
    prepared_deps_init(&dev);
    int result = compile_project(root, profile, db, refresh_toolchain, &ctx, &chain, &objects,
                                 &include_flags, &any_cpp, &any_compiled, &dev);
    if(result != exit_ok) {
        prepared_deps_free(&dev);
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

    /*
     * What `[dev-deps]` adds, and only here.
     *
     * Their include directories go onto the line that compiles `tests/` and
     * onto no other, which is what makes the separation real: a source under
     * `src/` that includes one fails to compile with "no such file", on the
     * first build, rather than at link time or in production (RFC-0008).
     *
     * Their defines and flags land in `[test].options` and their libraries in
     * this `ctx`'s link list — both local to this function, so the binary
     * `molto build` produces never sees them.
     */
    if(result == exit_ok && str_list_count(&dev.includes) > 0) {
        char flag[PATH_BUFFER_SIZE + 4];
        for(size_t i = 0; result == exit_ok && i < str_list_count(&dev.includes); i++) {
            const char *directory = str_list_get(&dev.includes, i);
            if(!fs_format_path(flag, sizeof flag, INCLUDE_FLAG_FORMAT, directory) ||
               !str_list_push(&include_flags, flag))
                result = exit_build_failure;
        }
    }
    for(size_t i = 0; result == exit_ok && i < str_list_count(&dev.defines); i++) {
        if(!append_option(ctx.test.options.defines, &ctx.test.options.define_count,
                          PROJECT_MAX_OPTS, str_list_get(&dev.defines, i), "[test].defines"))
            result = exit_build_failure;
    }
    for(size_t i = 0; result == exit_ok && i < str_list_count(&dev.flags); i++) {
        if(!append_option(ctx.test.options.flags, &ctx.test.options.flag_count, PROJECT_MAX_OPTS,
                          str_list_get(&dev.flags, i), "[test].flags"))
            result = exit_build_failure;
    }
    for(size_t i = 0; result == exit_ok && i < str_list_count(&dev.links); i++) {
        const char *library = str_list_get(&dev.links, i);
        if(ctx.target.link_count >= PROJECT_MAX_LINK ||
           !fs_format_path(ctx.target.link[ctx.target.link_count], PROJECT_LINK_NAME_MAX, "%s",
                           library)) {
            fprintf(stderr, "molto: a development dependency's '%s' does not fit the link line\n",
                    library);
            result = exit_build_failure;
        } else {
            ctx.target.link_count++;
        }
    }

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

    /* A development dependency's own sources are compiled in their own pass,
       each against its own options, exactly as a runtime one's are — and their
       objects join the test link rather than the project's. */
    if(result == exit_ok && dev.unit_count > 0) {
        dep_pass pass = {0};
        bool dev_cpp = false;
        bool dev_compiled = false;
        if(!dep_pass_build(&pass, &dev, &ctx.target))
            result = exit_build_failure;
        else if(pass.count > 0)
            result = compile_units(root, profile, &settings, &ctx.env, &chain, db, pass.units,
                                   pass.count, &lib_objects, &dev_cpp, &dev_compiled);
        dep_pass_free(&pass);
        any_cpp = any_cpp || dev_cpp;
        any_compiled = any_compiled || dev_compiled;
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
    if(result == exit_ok && str_list_count(&test_sources) > 0) {
        compile_unit *units =
            units_from(&test_sources, &ctx.target, profile_opts, &ctx.test.options, &include_flags);
        result = units == NULL ? exit_build_failure
                               : compile_units(root, profile, &settings, &ctx.env, &chain, db,
                                               units, str_list_count(&test_sources), &test_objects,
                                               &tests_cpp, &tests_compiled);
        free(units);
    }

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

    prepared_deps_free(&dev);
    str_list_free(&test_objects);
    str_list_free(&test_sources);
    str_list_free(&lib_objects);
    str_list_free(&objects);
    str_list_free(&include_flags);
    warn_if_not_saved(db);
    return result;
}
