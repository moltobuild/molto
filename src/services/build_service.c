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
#define LINK_FLAG_FORMAT  "-l%s"  /* link a system library, e.g. -lm */
#define INCLUDE_FLAG_FORMAT "-I%s" /* add an include search directory */

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

/* Fixed slots in a compile command's argv:
 *   compiler, -c, source, -o, object, -O<n>, [-g], [-std=..], -MMD, -MF,
 *   depfile, include, NULL
 * The optional -g and -std mean the count varies, so we size for the maximum. */
#define COMPILE_ARGV_MAX 13
/* Extra argv slots around the object list when linking: linker, -o, binary,
 * NULL (system libraries are counted separately). */
#define LINK_ARGV_EXTRA 4
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

/* Compile a single translation unit to `object`, using the toolchain and
   language standard from `target`. */
static bool compile_one(const char *source, const char *object,
                        const manifest_profile *settings, const char *include_flag,
                        const project_target *target) {
    const char *cc, *cxx;
    toolchain_drivers(target, &cc, &cxx);
    bool is_cpp = source_is_cpp(source);

    char opt_flag[OPT_FLAG_SIZE];
    snprintf(opt_flag, sizeof opt_flag, OPT_FLAG_FORMAT, settings->opt_level);

    const char *std_value = is_cpp ? target->cpp_std : target->std;
    bool has_std = std_value[0] != '\0';
    char std_flag[STD_FLAG_SIZE];
    if (has_std)
        snprintf(std_flag, sizeof std_flag, STD_FLAG_FORMAT, std_value);

    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    depfile_path_for(object, depfile, sizeof depfile);

    const char *argv[COMPILE_ARGV_MAX];
    size_t i = 0;
    argv[i++] = is_cpp ? cxx : cc;
    argv[i++] = ARG_COMPILE;
    argv[i++] = source;
    argv[i++] = ARG_OUTPUT;
    argv[i++] = object;
    argv[i++] = opt_flag;
    if (settings->debug_info)
        argv[i++] = ARG_DEBUG;
    if (has_std)
        argv[i++] = std_flag;
    argv[i++] = ARG_DEPFILE_GEN;   /* -MMD */
    argv[i++] = ARG_DEPFILE_OUT;   /* -MF  */
    argv[i++] = depfile;
    argv[i++] = include_flag;
    argv[i++] = NULL;
    return process_run(argv) == 0;
}

/* Link every object into the final executable, using the toolchain from
   `target` and appending its system libraries (`-l<lib>`). */
static bool link_all(bool any_cpp, const str_list *objects, const char *binary,
                     const project_target *target) {
    const char *cc, *cxx;
    toolchain_drivers(target, &cc, &cxx);
    const char *linker = any_cpp ? cxx : cc;
    size_t count = str_list_count(objects);
    size_t lib_count = target->link_count;
    const char **argv = malloc((count + LINK_ARGV_EXTRA + lib_count) * sizeof(char *));
    if (argv == NULL)
        return false;
    /* -l flags need storage that outlives process_run. */
    char lib_flags[PROJECT_MAX_LINK][PROJECT_LINK_NAME_MAX + sizeof("-l")];
    size_t i = 0;
    argv[i++] = linker;
    for (size_t j = 0; j < count; j++)
        argv[i++] = str_list_get(objects, j);
    argv[i++] = ARG_OUTPUT;
    argv[i++] = binary;
    /* System libraries go after the objects so the linker resolves them. */
    for (size_t j = 0; j < lib_count; j++) {
        snprintf(lib_flags[j], sizeof lib_flags[j], LINK_FLAG_FORMAT, target->link[j]);
        argv[i++] = lib_flags[j];
    }
    argv[i++] = NULL;
    bool ok = process_run(argv) == 0;
    free(argv);
    return ok;
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
    atomic_bool *failed;
} compile_task;

static void compile_task_run(void *arg) {
    compile_task *task = arg;
    if (!compile_one(task->source, task->object, task->settings,
                     task->include_flag, task->target)) {
        fprintf(stderr, "molto: failed to compile '%s'\n", task->source);
        atomic_store(task->failed, true);
    }
}

/* Decide whether `source` must be recompiled into `object`. Header-aware: if a
   dependency file from a previous build exists, the unit is stale when the
   source or any header it lists is newer than the object. Fail-safe: a missing
   object, a missing/empty depfile (e.g. the first build), or a deleted
   prerequisite all force a rebuild. */
static bool needs_rebuild(const char *source, const char *object) {
    if (!fs_path_exists(object))
        return true;
    char depfile[PATH_BUFFER_SIZE + sizeof(DEPFILE_SUFFIX)];
    depfile_path_for(object, depfile, sizeof depfile);
    str_list prerequisites;
    str_list_init(&prerequisites);
    bool stale;
    if (depfile_read(depfile, &prerequisites) && str_list_count(&prerequisites) > 0) {
        stale = false;
        for (size_t i = 0; i < str_list_count(&prerequisites); i++) {
            if (fs_source_newer(str_list_get(&prerequisites, i), object)) {
                stale = true;
                break;
            }
        }
    } else {
        stale = fs_source_newer(source, object);
    }
    str_list_free(&prerequisites);
    return stale;
}

/* Compile every source. Phase 1 (sequential) resolves object paths and marks
   which units are stale; phase 2 compiles the stale ones in parallel on a
   work-stealing pool. Reports whether C++ is present and whether anything was
   actually recompiled. */
static int compile_sources(const char *root, build_profile profile,
                           const manifest_profile *settings, const char *include_flag,
                           const project_target *target,
                           const str_list *sources, str_list *objects,
                           bool *any_cpp, bool *any_compiled) {
    size_t count = str_list_count(sources);
    bool *needs = calloc(count, sizeof(bool));
    if (needs == NULL)
        return exit_build_failure;

    /* Phase 1: resolve object paths and decide what to rebuild. Finishing all
       str_list_push here keeps the object pointers stable for phase 2. */
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
        needs[i] = needs_rebuild(source, object);
    }

    size_t to_build = 0;
    for (size_t i = 0; i < count; i++)
        to_build += needs[i] ? 1 : 0;
    if (to_build == 0) {
        *any_compiled = false;
        free(needs);
        return exit_ok;
    }

    /* Phase 2: compile the stale units concurrently. */
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

/* Load the manifest and compile every source under `root/src` into `objects`
   (caller-initialised, caller-freed). Reports whether C++ is present and whether
   anything was recompiled. Shared by build_project and build_tests. */
static int compile_project(const char *root, build_profile profile,
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
                             &sources, objects_out, any_cpp_out, any_compiled_out);
    str_list_free(&sources);
    return result;
}

int build_project(const char *root, build_profile profile,
                  char *out_binary, size_t out_binary_size) {
    project_ctx ctx;
    str_list objects;
    str_list_init(&objects);
    bool any_cpp = false;
    bool any_compiled = false;
    int result = compile_project(root, profile, &ctx, &objects, &any_cpp, &any_compiled);

    if (result == exit_ok) {
        char binary[PATH_BUFFER_SIZE];
        compose_binary_path(root, profile, ctx.project_name, binary, sizeof binary);
        /* Re-link only when something was rebuilt or the binary is stale. */
        if ((any_compiled || link_needed(&objects, binary))
            && !link_all(any_cpp, &objects, binary, &ctx.target)) {
            fprintf(stderr, "molto: failed to link '%s'\n", binary);
            result = exit_build_failure;
        }
        if (result == exit_ok && out_binary != NULL)
            snprintf(out_binary, out_binary_size, "%s", binary);
    }

    str_list_free(&objects);
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
    project_ctx ctx;
    str_list objects;
    str_list_init(&objects);
    bool any_cpp = false;
    bool any_compiled = false;
    int result = compile_project(root, profile, &ctx, &objects, &any_cpp, &any_compiled);
    if (result != exit_ok) {
        str_list_free(&objects);
        return result;
    }

    manifest_profile settings = profile_settings(&ctx, profile);
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
        if (needs_rebuild(test_source, test_object)) {
            if (!compile_one(test_source, test_object, &settings, include_flag,
                             &ctx.target)) {
                fprintf(stderr, "molto: failed to compile '%s'\n", test_source);
                result = exit_build_failure;
                break;
            }
            recompiled = true;
        }

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
        if ((recompiled || any_compiled || link_needed(&link_objects, test_binary))
            && !link_all(cpp, &link_objects, test_binary, &ctx.target)) {
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
    return result;
}
