#include <molto/services/build_service.h>

#include <molto/build/depfile.h>
#include <molto/build/profile.h>
#include <molto/exit_code.h>
#include <molto/project/project_ctx.h>
#include <molto/services/fs_service.h>
#include <molto/services/manifest_service.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/util/str_list.h>
#include <molto/util/task_pool.h>
#include <molto/workspace/wsdb.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Toolchain drivers per compiler family (default: GCC, v0.1 target). */
#define CC_C       "gcc"     /* GCC C driver */
#define CC_CPP     "g++"     /* GCC C++ driver */
#define CC_CLANG   "clang"   /* LLVM C driver */
#define CC_CLANGXX "clang++" /* LLVM C++ driver */
#define CC_MSVC    "cl"      /* MSVC driver (C and C++) */

/* Compiler command-line arguments. */
#define ARG_COMPILE       "-c"   /* compile only, do not link */
#define ARG_OUTPUT        "-o"   /* next argument is the output path */
#define ARG_DEBUG         "-g"   /* emit debug symbols */
#define ARG_DEPFILE_GEN   "-MMD" /* also write a header-dependency file */
#define ARG_DEPFILE_OUT   "-MF"  /* next argument is the dependency file path */
#define OPT_FLAG_FORMAT   "-O%d" /* optimisation level, e.g. -O2 */
#define STD_FLAG_FORMAT   "-std=%s" /* language standard, e.g. -std=c23 */
#define INCLUDE_FLAG_FORMAT "-I%s" /* add an include search directory */
#define DEFINE_FLAG_PREFIX  "-D"  /* prepended to a preprocessor define */
#define INCLUDE_FLAG_PREFIX "-I"  /* prepended to an include directory */
#define LINK_FLAG_PREFIX    "-l"  /* prepended to a system library name */

/* On-disk layout of a Molto project. */
#define MANIFEST_FILENAME "Project.toml"
#define DIR_BUILD         "build" /* output root, e.g. build/<profile>/ */
#define DIR_OBJ           "obj"   /* compiled objects under the build root */
#define DIR_SRC           "src"   /* where sources are discovered */
#define DIR_TESTS         "tests" /* where test sources are discovered */
#define OBJECT_SUFFIX     ".o"    /* appended to a source path for its object */
#define DEPFILE_SUFFIX    ".d"    /* appended to an object path for its depfile */

/* Size of the stack buffers used to compose filesystem paths. */
#define PATH_BUFFER_SIZE 4096

/* Size of the buffer receiving a manifest parse-error message. */
#define MANIFEST_ERROR_SIZE 256

/* Size of the small buffer holding the "-O<n>" flag. */
#define OPT_FLAG_SIZE 16
/* Size of the buffer holding the "-std=<name>" flag. */
#define STD_FLAG_SIZE 24

/* Compose the output executable path for a package. */
static void compose_binary_path(const char *root, build_profile profile,
                                const char *name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/" DIR_BUILD "/%s/%s", root, profile_name(profile), name);
}

/* Select the settings for `profile` from a parsed project context. */
static manifest_profile profile_settings(const project_ctx *ctx, build_profile profile) {
    switch (profile) {
        case profile_release: return ctx->profile.release;
        case profile_bench:   return ctx->profile.bench;
        case profile_custom:  return ctx->profile.custom;
        case profile_debug:
        default:              return ctx->profile.debug;
    }
}

/* Select the per-profile extra options for `profile`. */
static const project_options *profile_options_for(const project_ctx *ctx,
                                                  build_profile profile) {
    switch (profile) {
        case profile_release: return &ctx->profile_options.release;
        case profile_bench:   return &ctx->profile_options.bench;
        case profile_custom:  return &ctx->profile_options.custom;
        case profile_debug:
        default:              return &ctx->profile_options.debug;
    }
}

/* Resolve the C and C++ driver names for the requested toolchain. An empty
   `compiler` (autodetect) uses GCC, the v0.1 default. */
static void toolchain_drivers(const project_target *target,
                              const char **cc, const char **cxx) {
    if (strcmp(target->compiler, CC_CLANG) == 0 || strcmp(target->compiler, "llvm") == 0) {
        *cc = CC_CLANG;
        *cxx = CC_CLANGXX;
    } else if (strcmp(target->compiler, "msvc") == 0) {
        *cc = CC_MSVC;
        *cxx = CC_MSVC;
    } else {
        *cc = CC_C;
        *cxx = CC_CPP;
    }
}

/* Map a source path to its object path, mirroring the source tree under
   `root/build/<profile_dir>/obj`. */
static void object_path_for(const char *root, const char *profile_dir,
                            const char *source, char *out, size_t out_size) {
    size_t root_len = strlen(root);
    const char *relative = source;
    if (strncmp(source, root, root_len) == 0 && source[root_len] == '/')
        relative = source + root_len + 1;
    snprintf(out, out_size, "%s/" DIR_BUILD "/%s/" DIR_OBJ "/%s" OBJECT_SUFFIX,
             root, profile_dir, relative);
}

/* The dependency file gcc writes next to an object: "<object>.d". Used both to
   tell the compiler where to write it and to read it back when deciding whether
   to rebuild, so the two paths always match. */
static void depfile_path_for(const char *object, char *out, size_t out_size) {
    snprintf(out, out_size, "%s" DEPFILE_SUFFIX, object);
}

/* Create the parent directory chain for `path`. */
static bool make_parent_dirs(const char *path) {
    char directory[PATH_BUFFER_SIZE];
    snprintf(directory, sizeof directory, "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash == NULL)
        return true;
    *slash = '\0';
    return fs_make_dirs(directory);
}

/* Push "<prefix><value>" (e.g. "-DFOO=1") onto argv. */
static bool push_prefixed(str_list *argv, const char *prefix, const char *value) {
    char buffer[PROJECT_OPT_LEN + 4];
    snprintf(buffer, sizeof buffer, "%s%s", prefix, value);
    return str_list_push(argv, buffer);
}

/* Append a scope's defines (-D), include dirs (-I) and raw flags to argv. */
static bool push_options(str_list *argv, const project_options *options) {
    bool ok = true;
    for (size_t i = 0; ok && i < options->define_count; i++)
        ok = push_prefixed(argv, DEFINE_FLAG_PREFIX, options->defines[i]);
    for (size_t i = 0; ok && i < options->include_count; i++)
        ok = push_prefixed(argv, INCLUDE_FLAG_PREFIX, options->include[i]);
    for (size_t i = 0; ok && i < options->flag_count; i++)
        ok = str_list_push(argv, options->flags[i]);
    return ok;
}

/* Join argv items into one space-separated heap string (caller frees). */
static char *join_args(const str_list *argv) {
    size_t total = 1;
    for (size_t i = 0; i < str_list_count(argv); i++)
        total += strlen(str_list_get(argv, i)) + 1;
    char *out = malloc(total);
    if (out == NULL)
        return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < str_list_count(argv); i++)
        pos += (size_t)snprintf(out + pos, total - pos, "%s%s",
                                i > 0 ? " " : "", str_list_get(argv, i));
    return out;
}

/* Run a command held in a str_list argv (adds the NULL terminator). */
static int run_str_argv(const str_list *argv) {
    size_t count = str_list_count(argv);
    const char **cargv = malloc((count + 1) * sizeof(char *));
    if (cargv == NULL)
        return -1;
    for (size_t i = 0; i < count; i++)
        cargv[i] = str_list_get(argv, i);
    cargv[count] = NULL;
    int status = process_run(cargv);
    free(cargv);
    return status;
}

/* Build the full compile command for one source into `argv` (a str_list):
   driver, -c, source, -o, object, -O<n>, [-g], [-std], base+profile defines/
   includes/flags, -MMD -MF depfile, and the src include flag. */
static bool build_compile_argv(str_list *argv, const char *source, const char *object,
                               const manifest_profile *settings, const project_target *target,
                               const project_options *profile_opts, const char *include_flag,
                               const char *depfile) {
    const char *cc, *cxx;
    toolchain_drivers(target, &cc, &cxx);
    bool is_cpp = source_is_cpp(source);

    char opt_flag[OPT_FLAG_SIZE];
    snprintf(opt_flag, sizeof opt_flag, OPT_FLAG_FORMAT, settings->opt_level);

    bool ok = str_list_push(argv, is_cpp ? cxx : cc)
           && str_list_push(argv, ARG_COMPILE)
           && str_list_push(argv, source)
           && str_list_push(argv, ARG_OUTPUT)
           && str_list_push(argv, object)
           && str_list_push(argv, opt_flag);
    if (ok && settings->debug_info)
        ok = str_list_push(argv, ARG_DEBUG);
    const char *std_value = is_cpp ? target->cpp_std : target->std;
    if (ok && std_value[0] != '\0') {
        char std_flag[STD_FLAG_SIZE];
        snprintf(std_flag, sizeof std_flag, STD_FLAG_FORMAT, std_value);
        ok = str_list_push(argv, std_flag);
    }
    if (ok)
        ok = push_options(argv, &target->options);
    if (ok)
        ok = push_options(argv, profile_opts);
    if (ok)
        ok = str_list_push(argv, ARG_DEPFILE_GEN)
          && str_list_push(argv, ARG_DEPFILE_OUT)
          && str_list_push(argv, depfile)
          && str_list_push(argv, include_flag);
    return ok;
}

/* Build the current compile command for a source as a heap string, for the
   fingerprint comparison (caller frees). NULL on allocation failure. */
static char *compile_command_string(const char *source, const char *object,
                                     const manifest_profile *settings,
                                     const project_target *target,
                                     const project_options *profile_opts,
                                     const char *include_flag) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    depfile_path_for(object, depfile, sizeof depfile);
    str_list argv;
    str_list_init(&argv);
    char *command = NULL;
    if (build_compile_argv(&argv, source, object, settings, target,
                           profile_opts, include_flag, depfile))
        command = join_args(&argv);
    str_list_free(&argv);
    return command;
}

/* Compile a single translation unit to `object`. gcc writes the header
   dependency file (`-MMD -MF <object>.d`) as a side effect; it is absorbed into
   the WSDB afterwards, on the main thread. */
static bool compile_one(const char *source, const char *object,
                        const manifest_profile *settings, const project_target *target,
                        const project_options *profile_opts, const char *include_flag) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    depfile_path_for(object, depfile, sizeof depfile);
    str_list argv;
    str_list_init(&argv);
    if (!build_compile_argv(&argv, source, object, settings, target,
                            profile_opts, include_flag, depfile)) {
        str_list_free(&argv);
        return false;
    }
    bool ok = run_str_argv(&argv) == 0;
    str_list_free(&argv);
    return ok;
}

/* Record a freshly compiled object into the WSDB: read the prerequisites from
   gcc's depfile (falling back to just the source), store {command, prereqs},
   then delete the now-absorbed depfile. Runs on the main thread. */
static void wsdb_absorb_object(wsdb *db, const char *source, const char *object,
                               const char *command) {
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    depfile_path_for(object, depfile, sizeof depfile);
    str_list prereqs;
    str_list_init(&prereqs);
    if (!depfile_read(depfile, &prereqs) || str_list_count(&prereqs) == 0)
        (void)str_list_push(&prereqs, source);
    wsdb_record_object(db, object, command, &prereqs);
    str_list_free(&prereqs);
    remove(depfile);
}

/* Load and parse `root/Project.toml` into a project context, reporting the
   detailed parse error to stderr on failure. */
static int load_project(const char *root, project_ctx *out) {
    char manifest_path[PATH_BUFFER_SIZE];
    snprintf(manifest_path, sizeof manifest_path, "%s/" MANIFEST_FILENAME, root);
    if (!fs_path_exists(manifest_path)) {
        fprintf(stderr, "molto: no " MANIFEST_FILENAME " in '%s'\n", root);
        return exit_invalid_manifest;
    }
    char err[MANIFEST_ERROR_SIZE] = "";
    if (!project_load(manifest_path, out, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err[0] != '\0' ? err : "invalid manifest");
        return exit_invalid_manifest;
    }
    return exit_ok;
}

/* One parallel compilation task: compile `source` into `object`, recording a
   shared failure flag. Runs on a task_pool worker. */
typedef struct {
    const char *source;
    const char *object;
    const manifest_profile *settings;
    const char *include_flag;
    const project_target *target;
    const project_options *profile_opts;
    atomic_bool *failed;
} compile_task;

static void compile_task_run(void *arg) {
    compile_task *task = arg;
    if (!compile_one(task->source, task->object, task->settings, task->target,
                     task->profile_opts, task->include_flag)) {
        fprintf(stderr, "molto: failed to compile '%s'\n", task->source);
        atomic_store(task->failed, true);
    }
}

/* Compile every source. Phase 1 (sequential) resolves object paths and asks the
   WSDB which units are stale; phase 2 compiles them in parallel; phase 3 (back
   on this thread) records the results into the WSDB. Reports whether C++ is
   present and whether anything was recompiled. */
static int compile_sources(const char *root, build_profile profile,
                           const manifest_profile *settings, const char *include_flag,
                           const project_target *target, const project_options *profile_opts,
                           wsdb *db, const str_list *sources, str_list *objects,
                           bool *any_cpp, bool *any_compiled) {
    size_t count = str_list_count(sources);
    bool *needs = calloc(count, sizeof(bool));
    if (needs == NULL)
        return exit_build_failure;

    /* Phase 1: resolve object paths and ask the WSDB what is stale. Finishing
       all str_list_push here keeps the object pointers stable for phase 2. */
    for (size_t i = 0; i < count; i++) {
        const char *source = str_list_get(sources, i);
        if (source_is_cpp(source))
            *any_cpp = true;
        char object[PATH_BUFFER_SIZE];
        object_path_for(root, profile_name(profile), source, object, sizeof object);
        if (!make_parent_dirs(object)) {
            fprintf(stderr, "molto: could not create output directory for '%s'\n", object);
            free(needs);
            return exit_build_failure;
        }
        if (!str_list_push(objects, object)) {
            free(needs);
            return exit_build_failure;
        }
        char *command = compile_command_string(source, object, settings, target,
                                               profile_opts, include_flag);
        needs[i] = command == NULL || !wsdb_object_fresh(db, object, command);
        free(command);
    }

    size_t to_build = 0;
    for (size_t i = 0; i < count; i++)
        to_build += needs[i] ? 1 : 0;
    if (to_build == 0) {
        *any_compiled = false;
        free(needs);
        return exit_ok;
    }

    /* Phase 2: compile the stale units concurrently (no WSDB access here). */
    compile_task *tasks = calloc(to_build, sizeof(compile_task));
    task_pool *pool = task_pool_create(0);
    if (tasks == NULL || pool == NULL) {
        free(tasks);
        task_pool_destroy(pool);
        free(needs);
        return exit_build_failure;
    }

    atomic_bool failed = false;
    int result = exit_ok;
    size_t next = 0;
    for (size_t i = 0; i < count && result == exit_ok; i++) {
        if (!needs[i])
            continue;
        tasks[next] = (compile_task){
            .source = str_list_get(sources, i),
            .object = str_list_get(objects, i),
            .settings = settings,
            .include_flag = include_flag,
            .target = target,
            .profile_opts = profile_opts,
            .failed = &failed,
        };
        if (!task_pool_submit(pool, compile_task_run, &tasks[next]))
            result = exit_build_failure;
        next++;
    }
    task_pool_wait(pool);
    task_pool_destroy(pool);

    if (result == exit_ok && atomic_load(&failed))
        result = exit_build_failure;

    /* Phase 3: record the freshly built objects into the WSDB (single-threaded). */
    if (result == exit_ok) {
        for (size_t i = 0; i < count; i++) {
            if (!needs[i])
                continue;
            const char *source = str_list_get(sources, i);
            const char *object = str_list_get(objects, i);
            char *command = compile_command_string(source, object, settings, target,
                                                   profile_opts, include_flag);
            if (command != NULL) {
                wsdb_absorb_object(db, source, object, command);
                free(command);
            }
        }
    }

    *any_compiled = true;
    free(tasks);
    free(needs);
    return result;
}

/* Return true if the executable must be re-linked: it is missing or older
   than at least one object file. */
static bool link_needed(const str_list *objects, const char *binary) {
    for (size_t i = 0; i < str_list_count(objects); i++) {
        if (fs_source_newer(str_list_get(objects, i), binary))
            return true;
    }
    return false;
}

/* Build the link command into `argv`: linker, objects, raw flags (base +
   profile, so -flto/-fsanitize reach the linker too), -o binary, and the
   system libraries (-l<lib>). */
static bool build_link_argv(str_list *argv, bool any_cpp, const str_list *objects,
                            const char *binary, const project_target *target,
                            const project_options *profile_opts) {
    const char *cc, *cxx;
    toolchain_drivers(target, &cc, &cxx);
    bool ok = str_list_push(argv, any_cpp ? cxx : cc);
    for (size_t i = 0; ok && i < str_list_count(objects); i++)
        ok = str_list_push(argv, str_list_get(objects, i));
    for (size_t i = 0; ok && i < target->options.flag_count; i++)
        ok = str_list_push(argv, target->options.flags[i]);
    for (size_t i = 0; ok && i < profile_opts->flag_count; i++)
        ok = str_list_push(argv, profile_opts->flags[i]);
    if (ok)
        ok = str_list_push(argv, ARG_OUTPUT) && str_list_push(argv, binary);
    for (size_t i = 0; ok && i < target->link_count; i++)
        ok = push_prefixed(argv, LINK_FLAG_PREFIX, target->link[i]);
    return ok;
}

/* Link `objects` into `binary` when needed — `force` (something recompiled),
   a stale/missing binary, or a changed link command (per the WSDB). Records the
   link command in the WSDB. Returns false only if a needed link failed. */
static bool link_project(bool any_cpp, const str_list *objects, const char *binary,
                         const project_target *target, const project_options *profile_opts,
                         bool force, wsdb *db) {
    str_list argv;
    str_list_init(&argv);
    if (!build_link_argv(&argv, any_cpp, objects, binary, target, profile_opts)) {
        str_list_free(&argv);
        return false;
    }
    char *command = join_args(&argv);

    bool ok = true;
    if (force || command == NULL || !wsdb_binary_fresh(db, binary, command)
        || link_needed(objects, binary)) {
        ok = run_str_argv(&argv) == 0;
        if (ok && command != NULL)
            wsdb_record_binary(db, binary, command);
    }
    free(command);
    str_list_free(&argv);
    return ok;
}

/* Load the manifest and compile every source under `root/src` into `objects`
   (caller-initialised, caller-freed). Reports whether C++ is present and whether
   anything was recompiled. Shared by build_project and build_tests. */
static int compile_project(const char *root, build_profile profile, wsdb *db,
                           project_ctx *ctx_out, str_list *objects_out,
                           bool *any_cpp_out, bool *any_compiled_out) {
    int result = load_project(root, ctx_out);
    if (result != exit_ok)
        return result;
    manifest_profile settings = profile_settings(ctx_out, profile);

    char src_dir[PATH_BUFFER_SIZE];
    snprintf(src_dir, sizeof src_dir, "%s/" DIR_SRC, root);
    if (!fs_is_dir(src_dir)) {
        fprintf(stderr, "molto: no " DIR_SRC " directory in '%s'\n", root);
        return exit_build_failure;
    }

    str_list sources;
    str_list_init(&sources);
    if (!source_discovery_collect(src_dir, &sources) || str_list_count(&sources) == 0) {
        fprintf(stderr, "molto: no source files found under '%s'\n", src_dir);
        str_list_free(&sources);
        return exit_build_failure;
    }

    /* Extra room over src_dir for the "-I" prefix and the terminating NUL. */
    char include_flag[PATH_BUFFER_SIZE + 4];
    snprintf(include_flag, sizeof include_flag, INCLUDE_FLAG_FORMAT, src_dir);

    *any_cpp_out = false;
    *any_compiled_out = false;
    result = compile_sources(root, profile, &settings, include_flag, &ctx_out->target,
                             profile_options_for(ctx_out, profile), db,
                             &sources, objects_out, any_cpp_out, any_compiled_out);
    str_list_free(&sources);
    return result;
}

int build_project(const char *root, build_profile profile,
                  char *out_binary, size_t out_binary_size) {
    wsdb *db = wsdb_open(root);
    if (db == NULL) {
        fprintf(stderr, "molto: could not open the workspace database (locked?)\n");
        return exit_build_failure;
    }

    project_ctx ctx;
    str_list objects;
    str_list_init(&objects);
    bool any_cpp = false;
    bool any_compiled = false;
    int result = compile_project(root, profile, db, &ctx, &objects, &any_cpp, &any_compiled);

    if (result == exit_ok) {
        char binary[PATH_BUFFER_SIZE];
        compose_binary_path(root, profile, ctx.project_name, binary, sizeof binary);
        if (!link_project(any_cpp, &objects, binary, &ctx.target,
                          profile_options_for(&ctx, profile), any_compiled, db)) {
            fprintf(stderr, "molto: failed to link '%s'\n", binary);
            result = exit_build_failure;
        }
        if (result == exit_ok) {
            /* Prune objects orphaned by removed sources (scoped to src/). */
            char prefix[PATH_BUFFER_SIZE];
            snprintf(prefix, sizeof prefix, "%s/" DIR_BUILD "/%s/" DIR_OBJ "/" DIR_SRC "/",
                     root, profile_name(profile));
            wsdb_prune(db, &objects, prefix);
            if (out_binary != NULL)
                snprintf(out_binary, out_binary_size, "%s", binary);
        }
    }

    str_list_free(&objects);
    wsdb_close(db);
    return result;
}

/* Portion of `path` relative to `root` (drops a leading "root/"), or `path`
   unchanged if it is not under root. */
static const char *relative_to_root(const char *root, const char *path) {
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) == 0 && path[root_len] == '/')
        return path + root_len + 1;
    return path;
}

/* Output path of a test executable: build/<profile>/tests/<name>, mirroring the
   test source's path under tests/ with its extension stripped. */
static void test_binary_path(const char *root, const char *profile_dir,
                             const char *test_source, char *out, size_t out_size) {
    char stem[PATH_BUFFER_SIZE];
    snprintf(stem, sizeof stem, "%s", relative_to_root(root, test_source));
    char *dot = strrchr(stem, '.');
    char *slash = strrchr(stem, '/');
    if (dot != NULL && (slash == NULL || dot > slash))
        *dot = '\0';
    snprintf(out, out_size, "%s/" DIR_BUILD "/%s/%s", root, profile_dir, stem);
}

int build_tests(const char *root, build_profile profile, str_list *test_binaries_out) {
    wsdb *db = wsdb_open(root);
    if (db == NULL) {
        fprintf(stderr, "molto: could not open the workspace database (locked?)\n");
        return exit_build_failure;
    }

    project_ctx ctx;
    str_list objects;
    str_list_init(&objects);
    bool any_cpp = false;
    bool any_compiled = false;
    int result = compile_project(root, profile, db, &ctx, &objects, &any_cpp, &any_compiled);
    if (result != exit_ok) {
        str_list_free(&objects);
        wsdb_close(db);
        return result;
    }

    manifest_profile settings = profile_settings(&ctx, profile);
    const project_options *profile_opts = profile_options_for(&ctx, profile);
    const char *profile_dir = profile_name(profile);

    /* Object of src/main.c (the app entry point), if any, to exclude from test
       links: each test brings its own main(). */
    char main_source[PATH_BUFFER_SIZE];
    snprintf(main_source, sizeof main_source, "%s/" DIR_SRC "/main.c", root);
    char main_object[PATH_BUFFER_SIZE];
    object_path_for(root, profile_dir, main_source, main_object, sizeof main_object);
    bool has_main = fs_path_exists(main_source);

    /* Library objects = every src object except the app's main object. */
    str_list lib_objects;
    str_list_init(&lib_objects);
    for (size_t i = 0; i < str_list_count(&objects) && result == exit_ok; i++) {
        const char *object = str_list_get(&objects, i);
        if (has_main && strcmp(object, main_object) == 0)
            continue;
        if (!str_list_push(&lib_objects, object))
            result = exit_build_failure;
    }

    char src_dir[PATH_BUFFER_SIZE];
    snprintf(src_dir, sizeof src_dir, "%s/" DIR_SRC, root);
    char include_flag[PATH_BUFFER_SIZE + 4];
    snprintf(include_flag, sizeof include_flag, INCLUDE_FLAG_FORMAT, src_dir);

    char tests_dir[PATH_BUFFER_SIZE];
    snprintf(tests_dir, sizeof tests_dir, "%s/" DIR_TESTS, root);
    str_list test_sources;
    str_list_init(&test_sources);
    /* No tests/ directory (or empty) is not an error: nothing to build. */
    if (result == exit_ok && fs_is_dir(tests_dir))
        (void)source_discovery_collect(tests_dir, &test_sources);

    for (size_t i = 0; i < str_list_count(&test_sources) && result == exit_ok; i++) {
        const char *test_source = str_list_get(&test_sources, i);

        char test_object[PATH_BUFFER_SIZE];
        object_path_for(root, profile_dir, test_source, test_object, sizeof test_object);
        if (!make_parent_dirs(test_object)) {
            result = exit_build_failure;
            break;
        }
        bool recompiled = false;
        char *command = compile_command_string(test_source, test_object, &settings,
                                               &ctx.target, profile_opts, include_flag);
        bool stale = command == NULL || !wsdb_object_fresh(db, test_object, command);
        if (stale) {
            if (!compile_one(test_source, test_object, &settings, &ctx.target,
                             profile_opts, include_flag)) {
                fprintf(stderr, "molto: failed to compile '%s'\n", test_source);
                free(command);
                result = exit_build_failure;
                break;
            }
            if (command != NULL)
                wsdb_absorb_object(db, test_source, test_object, command);
            recompiled = true;
        }
        free(command);

        char test_binary[PATH_BUFFER_SIZE];
        test_binary_path(root, profile_dir, test_source, test_binary, sizeof test_binary);
        if (!make_parent_dirs(test_binary)) {
            result = exit_build_failure;
            break;
        }

        /* Link the test object together with the project's library objects. */
        str_list link_objects;
        str_list_init(&link_objects);
        bool pushed = str_list_push(&link_objects, test_object);
        for (size_t j = 0; pushed && j < str_list_count(&lib_objects); j++)
            pushed = str_list_push(&link_objects, str_list_get(&lib_objects, j));
        if (!pushed) {
            str_list_free(&link_objects);
            result = exit_build_failure;
            break;
        }
        bool cpp = any_cpp || source_is_cpp(test_source);
        if (!link_project(cpp, &link_objects, test_binary, &ctx.target, profile_opts,
                          recompiled || any_compiled, db)) {
            fprintf(stderr, "molto: failed to link '%s'\n", test_binary);
            result = exit_build_failure;
        }
        str_list_free(&link_objects);
        if (result != exit_ok)
            break;

        if (!str_list_push(test_binaries_out, test_binary)) {
            result = exit_build_failure;
            break;
        }
    }

    str_list_free(&test_sources);
    str_list_free(&lib_objects);
    str_list_free(&objects);
    wsdb_close(db);
    return result;
}
