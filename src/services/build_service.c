#include <molto/services/build_service.h>

#include <molto/build/compile_db.h>
#include <molto/build/compile_flags.h>
#include <molto/build/depfile.h>
#include <molto/build/diagnostic_view.h>
#include <molto/build/library.h>
#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/exit_code.h>
#include <molto/project/lockfile.h>
#include <molto/project/project_ctx.h>
#include <molto/services/conflict_prompt.h>
#include <molto/services/deps_service.h>
#include <molto/services/frontend_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/host_service.h>
#include <molto/services/ir_transform.h>
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
#include <unistd.h>

/* Compiler command-line arguments. */
#define ARG_COMPILE "-c"           /* compile only, do not link */
#define ARG_OUTPUT "-o"            /* next argument is the output path */
#define ARG_SHARED "-shared"       /* link a shared library rather than a program */
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

/* How much of what one tool says about one file is kept. Everything gcc has to
   say about a thoroughly broken translation unit fits several times over, and
   a unit that says more than this is told so. One of these exists per worker
   and only while that worker's compiler runs. */
#define BUILD_OUTPUT_SIZE 65536

/* Room for "gcc 12.3.0", or for the name of a binary chosen by hand. */
#define TOOLCHAIN_DESCRIPTION_MAX 128

/* What a run reports for a child killed by signal N, following the usual shell
   convention (see process_service.h). Above it, the compiler did not choose to
   stop and there is no diagnostic to look for. */
#define SIGNAL_EXIT_BASE 128

/* The project's own code, and the code that tests it. Neither is a package, so
   neither carries a name or a version: a line about one names the source. */
static const build_unit_label project_label = {.origin = build_origin_project};
static const build_unit_label tests_label = {.origin = build_origin_tests};

/* Which targets of a document one question is about.
 *
 * A build makes up to four passes over one document, and they differ only in
 * which of its targets they compile. Naming the four here is what lets the walk
 * that collects the paths and the walk that builds the units agree by
 * construction rather than by two matching conditions. */
typedef enum {
    doc_targets_project,          /* what the project owns, tests excluded */
    doc_targets_tests,            /* its tests */
    doc_targets_runtime_packages, /* a runtime dependency's own sources */
    doc_targets_dev_packages,     /* a development dependency's own sources */
} doc_target_set;

/* The dependency a target belongs to, or NULL when it belongs to the project. */
static const ir_dependency *package_of(const ir_document *doc, const ir_target *target) {
    if(target->package == NULL)
        return NULL;
    for(size_t i = 0; i < doc->dependency_count; i++) {
        if(doc->dependencies[i].name != NULL &&
           strcmp(doc->dependencies[i].name, target->package) == 0)
            return &doc->dependencies[i];
    }
    /* Unreachable for a document the transforms produced: the same pass writes
       the `Target` and the `Dependency` it names. A plugin's document is held
       to it by ir_validate, which refuses an unresolvable name outright. */
    return NULL;
}

static bool in_set(const ir_document *doc, const ir_target *target, doc_target_set set) {
    const ir_dependency *package = package_of(doc, target);
    if(package != NULL) {
        return package->scope == ir_dep_scope_dev ? set == doc_targets_dev_packages
                                                  : set == doc_targets_runtime_packages;
    }
    return set == doc_targets_tests ? target->kind == ir_target_test
                                    : set == doc_targets_project && target->kind != ir_target_test;
}

/* What a target's paths are relative to: its package's root where it has one,
   and the project's otherwise (RFC-0013). */
static const char *target_root(const ir_document *doc, const ir_target *target, const char *root) {
    const ir_dependency *package = package_of(doc, target);
    return package != NULL && package->root != NULL ? package->root : root;
}

/* How the report names one target's units.
 *
 * Only two answers reach a line for a package — a registry package states a
 * version someone can verify, and everything else is a module whose bytes are
 * wherever the manifest said. Which of git, path or archive it was stays in the
 * lock, where it can be acted on.
 *
 * It borrows the document's own strings, so the labels die with the document
 * they were built from. */
static build_unit_label label_for_target(const ir_document *doc, const ir_target *target) {
    const ir_dependency *package = package_of(doc, target);
    if(package == NULL)
        return target->kind == ir_target_test ? tests_label : project_label;
    return (build_unit_label){
        .origin = package->origin == ir_dep_registry ? build_origin_registry : build_origin_module,
        .name = package->name,
        .version = package->version,
        .source = package->root,
    };
}

/* One label per target, indexed by the target's position in the document.

   Built once every transform has finished adding targets, and that ordering is
   the contract: the index is a position, so a target added afterwards would
   have no label and the units of the one before it would borrow the wrong name.

   Caller frees. NULL means the allocation failed. */
[[nodiscard]] static build_unit_label *labels_for(const ir_document *doc) {
    build_unit_label *labels = calloc(doc->target_count + 1, sizeof *labels);
    if(labels == NULL)
        return NULL;
    for(size_t t = 0; t < doc->target_count; t++)
        labels[t] = label_for_target(doc, &doc->targets[t]);
    return labels;
}

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

/* The fingerprint of a command: the argv it will run, and after the mark the
   environment it will run in. Heap string, caller frees; NULL on failure.

   The two are one string because two consumers ask the same question of it —
   the workspace database compares it, the shared object cache hashes it — and
   one string is what stops their answers from disagreeing. Nothing is appended
   when there is no [env], which is what leaves the databases and cache entries
   already on disk valid. */
static char *command_fingerprint(const str_list *argv, const project_env *env) {
    char environment[PROJECT_ENV_FINGERPRINT_MAX];
    size_t env_length = project_env_fingerprint(env, environment, sizeof environment);
    char *command = join_args(argv);
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
static int run_str_argv(const str_list *argv, const project_env *env, char *capture,
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
    /* What the document says this unit is. Everything a build compiles comes
       from here: the frontend described the project and its tests, the
       transforms said what the dependencies are and what they export, and a
       package's own sources are a `Target` of kind `object` like any other. The
       whole compile line below is read off the node. */
    const ir_target *node;
    const ir_source *unit;
    /* Who this unit belongs to, for the report. One label is shared by every
       unit of a target, so naming forty sources costs one struct. Nothing
       here reads it: it is carried, not used. */
    const build_unit_label *label;
} compile_unit;

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
    return depfile_path_for(object, depfile, sizeof depfile) &&
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
    const int status = run_str_argv(&argv, env, output, output_size, truncated);
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

/* What every compilation pass of a build shares, beyond the units it compiles.
   Both answers belong to the invocation rather than to the project: how much of
   the machine this build may take, and whether anyone asked for a description
   of what it compiled. */
typedef struct {
    /* Worker threads per pass; 0 takes the whole machine, which is what a build
       does when `-j` is not given. */
    size_t jobs;
    /* Where every unit's command line is recorded, or NULL when nothing is
       collecting them. */
    compile_db *cdb;
} pass_options;

/*
 * Everything a pass compiles against that is not one of its units.
 *
 * It is carried by value, or at least by pointers to things that outlive the
 * build, because a plan is made before anything is compiled and the answers it
 * was made from have to still be there when they are. `settings` used to be a
 * pointer into the frame that read the manifest; that frame now returns before
 * the first compiler runs.
 */
typedef struct {
    const char *root;
    build_profile profile;
    manifest_profile settings;
    const project_env *env;
    const resolved_toolchain *chain;
    wsdb *db;
    const pass_options *options;
} build_pass_env;

/*
 * One translation unit, and what asking about it cost.
 *
 * `command` is the fingerprint of its command line: what decided the unit was
 * stale, and what will be recorded once it compiles. It is kept rather than
 * recomputed because between those two moments the whole rest of the build
 * happens, and a second composition would be describing whatever the world
 * looks like by then. It is only kept for a unit that will be compiled — for
 * the others it has already answered its one question.
 */
typedef struct {
    const compile_unit *unit; /* borrows an arena the plan owns */
    const char *object;       /* borrows the str_list it was pushed into */
    char *command;            /* owned; NULL when nothing will be compiled */
    bool needs_compile;
} planned_unit;

typedef struct {
    build_pass_env env;
    planned_unit *units;
    size_t count;
    size_t to_build;
} compile_pass;

/* --- naming a unit --- */

/* Portion of `path` relative to `root` (drops a leading "root/"), or `path`
   unchanged if it is not under root. */
static const char *relative_to_root(const char *root, const char *path) {
    size_t root_len = strlen(root);
    if(strncmp(path, root, root_len) == 0 && path[root_len] == '/')
        return path + root_len + 1;
    return path;
}

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
    const char *relative = relative_to_root(root, source);
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
    if(label->source[0] != '/')
        return label->source;
    const char *relative = fs_relative_to(label->source, root);
    return relative != label->source ? relative : NULL;
}

/* Which compiler was asked, as a person would name it.
 *
 * Under C_COMPILER there is no vendor and no version to give — the compiler
 * was chosen by hand and never asked what it was — so the binary itself is the
 * honest answer, and a better one than an empty line. */
static void describe_compiler(const resolved_toolchain *chain, bool is_cpp, char *out,
                              size_t out_size) {
    if(chain->vendor[0] != '\0' && chain->version[0] != '\0') {
        snprintf(out, out_size, "%s %s", chain->vendor, chain->version);
        return;
    }
    const char *driver = compile_flags_driver(chain, is_cpp);
    if(driver == NULL) {
        snprintf(out, out_size, "%s", chain->vendor);
        return;
    }
    const char *base = strrchr(driver, '/');
    snprintf(out, out_size, "%s", base != NULL ? base + 1 : driver);
}

/* --- what the compiler said --- */

/* A diagnostic Molto wrote itself, for what a tool left unsaid. */
static void push_own(diagnostic_list *found, const char *source, diagnostic_severity severity,
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
        push_own(&found, unit->source, diagnostic_severity_note,
                 "there was more of this than Molto kept");
    if(status != 0 && diagnostic_count_severity(&found, diagnostic_severity_error) == 0)
        push_own(&found, unit->source, diagnostic_severity_error,
                 status > SIGNAL_EXIT_BASE ? "the compiler was killed while compiling this file"
                                           : "the compiler failed with nothing to say about this "
                                             "file");

    char compiler[TOOLCHAIN_DESCRIPTION_MAX];
    describe_compiler(env->chain, source_is_cpp(unit->source), compiler, sizeof compiler);
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
 * It walks the document in the same order `document_sources` did, so the path
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
[[nodiscard]] static compile_unit *units_from_document(const ir_document *doc, doc_target_set set,
                                                       const str_list *sources,
                                                       const build_unit_label *labels) {
    const size_t total = str_list_count(sources);
    compile_unit *units = calloc(total, sizeof *units);
    if(units == NULL)
        return NULL;

    size_t at = 0;
    for(size_t t = 0; t < doc->target_count; t++) {
        const ir_target *node = &doc->targets[t];
        if(!in_set(doc, node, set))
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
        if(!object_path_for(env->root, profile_name(env->profile), source, object, sizeof object))
            return exit_build_failure;
        if(!make_parent_dirs(object)) {
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
            command = command_fingerprint(&argv, env->env);
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

/* Return true if the executable must be re-linked: it is missing or older
   than at least one object file. */
static bool link_needed(const str_list *objects, const char *binary) {
    for(size_t i = 0; i < str_list_count(objects); i++) {
        if(fs_source_newer(str_list_get(objects, i), binary))
            return true;
    }
    return false;
}

/* One scope's link options, in the order the producer wrote them. */
static bool push_links(str_list *argv, const ir_target *node, ir_scope scope) {
    bool ok = true;
    for(size_t i = 0; ok && i < node->link_count; i++) {
        if(node->links[i].scope == scope)
            ok = str_list_push(argv, node->links[i].value);
    }
    return ok;
}

/* Build the link command into `argv`: linker, objects, -o binary, and then
   everything the document says reaches this target's link line.
 *
 * All of it comes off the node. A `LinkOption` is what reaches the line — the
 * `-l` is already on a library and `-flto` is just another value — so nothing
 * here tells one from the other, which is what lets the same loop carry both.
 *
 * Scope order is the compile line's, for the same reason: it is the only
 * ordering the document expresses, and a linker takes the last of two
 * contradictory flags exactly as a compiler does. `-o` moves ahead of them all,
 * where the manifest path put it in the middle — a linker does not care where
 * its output is named, and putting it before means the scopes stay contiguous.
 *
 * What still does not come off the node: the objects, which the engine composes,
 * and which driver runs, which is the toolchain's answer and not a document's
 * opinion. */
static bool build_link_argv(str_list *argv, bool any_cpp, const str_list *objects,
                            const char *binary, const ir_target *node,
                            const resolved_toolchain *chain) {
    const char *driver = compile_flags_driver(chain, any_cpp);
    if(driver == NULL) {
        fprintf(stderr, "molto: '%s' needs a C++ compiler and none was resolved\n", binary);
        return false;
    }
    bool ok = str_list_push(argv, driver);
    /* Read off the node rather than passed in: the document already says this
       is a shared library, and a second way of saying it could disagree with
       the first. The soname that goes with it is a LinkOption the frontend
       wrote, and arrives with the rest of them below. */
    if(ok && node->kind == ir_target_shared)
        ok = str_list_push(argv, ARG_SHARED);
    for(size_t i = 0; ok && i < str_list_count(objects); i++)
        ok = str_list_push(argv, str_list_get(objects, i));
    if(ok)
        ok = str_list_push(argv, ARG_OUTPUT) && str_list_push(argv, binary);
    return ok && push_links(argv, node, ir_scope_target) &&
           push_links(argv, node, ir_scope_profile) && push_links(argv, node, ir_scope_unit);
}

/* Everything a link's own report needs that the link itself does not. */
typedef struct {
    const char *root;
    const char *binary;
    const resolved_toolchain *chain;
    build_report *report;
    bool any_cpp;
} link_env;

/* What the linker said about one binary, framed the way a compiler's output is.
 *
 * Usually without an excerpt: a linker names a place in anyone's source only
 * when the objects it was given carry debug information, so an undefined
 * symbol can be pointed at under `debug` and never under a profile that turned
 * it off. What it always names is the symbol, which is the thing to go and
 * look for. */
static void report_link_diagnostics(const link_env *where, const char *output, bool truncated,
                                    int status) {
    diagnostic_list found;
    diagnostic_list_init(&found);
    /* A linker that failed names no severity because it has only the one; a
       linker that succeeded and still spoke was warning, and saying otherwise
       would fail a build that stands. */
    const bool parsed =
        status == 0 ? diagnostic_parse(output, &found) : diagnostic_parse_link(output, &found);
    if(!parsed) {
        diagnostic_list_free(&found);
        return;
    }
    diagnostic_list_set_columns(&found, diagnostic_columns_of_vendor(where->chain->vendor));
    if(truncated)
        push_own(&found, where->binary, diagnostic_severity_note,
                 "there was more of this than Molto kept");
    if(status != 0 && diagnostic_count_severity(&found, diagnostic_severity_error) == 0)
        push_own(&found, where->binary, diagnostic_severity_error,
                 "the linker failed with nothing to say about it");

    char compiler[TOOLCHAIN_DESCRIPTION_MAX];
    describe_compiler(where->chain, where->any_cpp, compiler, sizeof compiler);
    const diagnostic_context ctx = {
        .unit = fs_relative_to(where->binary, where->root),
        .action = diagnostic_view_linking,
        .compiler = compiler,
        .root = where->root,
    };
    char *block = diagnostic_view_render(&found, &ctx, build_report_wants_colour(where->report));
    diagnostic_list_free(&found);
    if(block == NULL)
        return;
    build_report_message(where->report, "%s\n", block);
    free(block);
}

/* Link `objects` into `binary` when needed — `force` (something recompiled),
   a stale/missing binary, or a changed link command (per the WSDB). Records the
   link command in the WSDB. Returns false only if a needed link failed. */
static bool link_project(bool any_cpp, const str_list *objects, const char *binary,
                         const ir_target *node, const project_env *env,
                         const resolved_toolchain *chain, bool force, wsdb *db, const char *root,
                         build_report *report) {
    str_list argv;
    str_list_init(&argv);
    if(!build_link_argv(&argv, any_cpp, objects, binary, node, chain)) {
        str_list_free(&argv);
        return false;
    }
    /* The environment belongs in the link fingerprint for the same reason it
       belongs in the compile one: it reaches the linker, so a different
       LIBRARY_PATH is a different binary. Relying on `force` to catch that
       would be correct only by accident — it is true when something was
       recompiled, and every object could have come from the shared cache. */
    char *command = command_fingerprint(&argv, env);

    bool ok = true;
    if(force || command == NULL || !wsdb_binary_fresh(db, binary, command) ||
       link_needed(objects, binary)) {
        char *output = malloc(BUILD_OUTPUT_SIZE);
        bool truncated = false;
        const int status =
            run_str_argv(&argv, env, output, output != NULL ? BUILD_OUTPUT_SIZE : 0, &truncated);
        ok = status == 0;
        if(output != NULL) {
            const link_env where = {.root = root,
                                    .binary = binary,
                                    .chain = chain,
                                    .report = report,
                                    .any_cpp = any_cpp};
            report_link_diagnostics(&where, output, truncated, status);
        } else if(!ok) {
            build_report_message(report, "molto: failed to link '%s'\n", binary);
        }
        free(output);
        if(ok && (command == NULL || !wsdb_record_binary(db, binary, command)))
            fprintf(stderr, "molto: warning: could not record '%s' as up to date\n", binary);
    }
    free(command);
    str_list_free(&argv);
    return ok;
}

/*
 * A static library: the objects, with an index, and nothing else.
 *
 * `ar` is not a linker and this is not a link. Nothing is resolved, no symbol
 * is looked up and no other library is consulted — which is why the node's link
 * options are not on this line. They belong to whoever links the program that
 * finally uses this archive, and putting them here would be recording an
 * intention `ar` has no way to honour.
 *
 * The archive is removed first rather than updated in place. `ar r` replaces
 * the members it is given and leaves the rest, so an object whose source was
 * deleted would stay in the archive across every later build — present at link
 * time, absent from the sources, and impossible to account for.
 */
static bool build_archive_argv(str_list *argv, const char *archiver, const str_list *objects,
                               const char *archive) {
    bool ok =
        str_list_push(argv, archiver) && str_list_push(argv, "rcs") && str_list_push(argv, archive);
    for(size_t i = 0; ok && i < str_list_count(objects); i++)
        ok = str_list_push(argv, str_list_get(objects, i));
    return ok;
}

/* The same freshness discipline the link has, for the same reason: an archive
   whose objects have not moved is an archive that does not need making, and
   remaking it would give every consumer a new mtime to react to. */
static bool archive_project(const str_list *objects, const char *archive, const project_env *env,
                            const resolved_toolchain *chain, bool force, wsdb *db,
                            build_report *report) {
    char archiver[TOOLCHAIN_PATH_MAX];
    if(!library_archiver(chain->cc, archiver, sizeof archiver)) {
        build_report_message(report, "molto: the path to an archiver does not fit\n");
        return false;
    }

    str_list argv;
    str_list_init(&argv);
    if(!build_archive_argv(&argv, archiver, objects, archive)) {
        str_list_free(&argv);
        return false;
    }
    char *command = command_fingerprint(&argv, env);

    bool ok = true;
    if(force || command == NULL || !wsdb_binary_fresh(db, archive, command) ||
       link_needed(objects, archive)) {
        (void)remove(archive);
        char *output = malloc(BUILD_OUTPUT_SIZE);
        bool truncated = false;
        const int status =
            run_str_argv(&argv, env, output, output != NULL ? BUILD_OUTPUT_SIZE : 0, &truncated);
        ok = status == 0;
        /* An archiver says almost nothing, and what it does say is not a
           compiler diagnostic — so it is repeated as it came rather than framed
           as one. */
        if(!ok)
            build_report_message(report, "molto: %s could not archive '%s'%s%s\n", archiver,
                                 archive, output != NULL && output[0] != '\0' ? ": " : "",
                                 output != NULL ? output : "");
        free(output);
        (void)truncated;
        if(ok && (command == NULL || !wsdb_record_binary(db, archive, command)))
            fprintf(stderr, "molto: warning: could not record '%s' as up to date\n", archive);
    }
    free(command);
    str_list_free(&argv);
    return ok;
}

/*
 * The two names that point at a shared library, beside it.
 *
 * Relative, naming only the file: both links sit in the same directory as their
 * target, and an absolute link would write this machine's build path inside an
 * artifact whose whole purpose is to be copied somewhere else.
 *
 * A failure here is a warning and not a failed build. The library itself is
 * built and correct; what is missing is the convenience of linking against
 * `-lfoo`, and refusing a build over a symlink would be refusing the thing that
 * worked because of the thing that did not.
 */
static void place_shared_links(const char *directory, const library_names *names,
                               build_report *report) {
    const char *const links[] = {names->soname, names->devlink};
    for(size_t i = 0; i < sizeof links / sizeof links[0]; i++) {
        if(links[i][0] == '\0' || strcmp(links[i], names->file) == 0)
            continue;
        char path[PATH_BUFFER_SIZE];
        if(!fs_format_path(path, sizeof path, "%s/%s", directory, links[i])) {
            (void)fs_report_long_path(links[i]);
            continue;
        }
        /* Removed first: symlink refuses to replace what is already there, and
           what is already there is a link to a version that has moved on. */
        (void)remove(path);
        if(symlink(names->file, path) != 0)
            build_report_message(report, "molto: warning: could not link '%s' to '%s'\n", links[i],
                                 names->file);
    }
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

[[nodiscard]] static bool prepare_and_lock(const char *root, project_ctx *ctx, prepared_deps *out,
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

/*
 * Everything a build is going to do, worked out before it does any of it.
 *
 * The plan exists so that one question has an answer: how many units this
 * build will compile. Nothing can say that until every pass has been planned —
 * a test build makes four — and nothing should print a bar until something
 * can.
 *
 * It owns the arenas its units borrow from, which is the whole difference from
 * what came before. Each of these used to be freed the moment its own pass
 * finished; now the last pass runs long after the first was planned, so they
 * all have to outlive the plan itself.
 */
#define BUILD_MAX_PASSES 4

typedef struct {
    compile_pass passes[BUILD_MAX_PASSES];
    size_t pass_count;
    size_t to_build; /* across every pass: what the bar counts */
    bool any_cpp;

    /* What is to be built, as the frontend described it. The plan is derived
       from it and is never serialised; the document is, and is what `molto ir`
       prints. RFC-0015 splits these two apart and this is the split. */
    ir_document doc;
    prepared_deps deps; /* runtime dependencies */
    prepared_deps dev;  /* development dependencies, resolved with them */
    /* One per target of the document, indexed by its position in it: how the
       report names the units that target describes. */
    build_unit_label *labels;
    str_list sources;
    str_list test_sources;
    str_list package_sources;
    str_list dev_package_sources;
    compile_unit *project_units;
    compile_unit *test_units;
    compile_unit *package_units;
    compile_unit *dev_package_units;
} build_plan;

static void build_plan_init(build_plan *plan) {
    memset(plan, 0, sizeof *plan);
    ir_document_init(&plan->doc);
    prepared_deps_init(&plan->deps);
    prepared_deps_init(&plan->dev);
    str_list_init(&plan->sources);
    str_list_init(&plan->test_sources);
    str_list_init(&plan->package_sources);
    str_list_init(&plan->dev_package_sources);
}

static void build_plan_free(build_plan *plan) {
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
[[nodiscard]] static int plan_add(build_plan *plan, const build_pass_env *env,
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
static int run_plan(const build_plan *plan, build_report *report, bool *any_compiled) {
    int result = exit_ok;
    for(size_t i = 0; i < plan->pass_count && result == exit_ok; i++)
        result = run_pass(&plan->passes[i], report, any_compiled);
    return result;
}

/* Tell the report what the build is about to do: the work, unit by unit, and
   a count of everything that turned out not to be work at all. */
static void report_plan(const build_plan *plan, const char *root, build_report *report) {
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
[[nodiscard]] static bool document_sources(const ir_document *doc, const char *root,
                                           doc_target_set set, str_list *out) {
    for(size_t t = 0; t < doc->target_count; t++) {
        const ir_target *target = &doc->targets[t];
        if(!in_set(doc, target, set))
            continue;
        const char *base = target_root(doc, target, root);
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
                                              build_profile profile, const project_ctx *ctx,
                                              const prepared_deps *deps, const prepared_deps *dev,
                                              char *err, size_t err_size) {
    char build_dir[PATH_BUFFER_SIZE];
    if(!fs_format_path(build_dir, sizeof build_dir, "%s/" DIR_BUILD "/%s", root,
                       profile_name(profile)))
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
[[nodiscard]] static int plan_project(const char *root, build_profile profile, wsdb *db,
                                      bool refresh_toolchain, const pass_options *options,
                                      project_ctx *ctx_out, resolved_toolchain *chain_out,
                                      str_list *objects_out, build_plan *plan) {
    int result = load_project(root, ctx_out);
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
    if(!prepare_and_lock(root, ctx_out, &plan->deps, &plan->dev, deps_err, sizeof deps_err)) {
        fprintf(stderr, "molto: %s\n", deps_err);
        return exit_dependency_failure;
    }

    /* The frontend describes the project; the engine below consumes what it
       said. The manifest is read twice for now — once here and once by
       load_project above, which is still where the compile line's options come
       from — and the second read goes away with `project_ctx` when the options
       are lowered from the document too.
     *
       Asked of the whole selection rather than of the native frontend by name.
       Today that resolves to the same thing — load_project above has already
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
    if(!document_sources(&plan->doc, root, doc_targets_project, &plan->sources))
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

    plan->labels = labels_for(&plan->doc);
    if(plan->labels == NULL)
        return exit_build_failure;

    if(!document_is_allowed(&plan->doc, root, profile, ctx_out, &plan->deps, &plan->dev,
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
    if(!document_sources(&plan->doc, root, doc_targets_runtime_packages, &plan->package_sources))
        return exit_build_failure;
    plan->package_units = units_from_document(&plan->doc, doc_targets_runtime_packages,
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
    result = toolchain_resolve(&ctx_out->target, needs_cpp, db, refresh_toolchain, chain_out);
    if(result != exit_ok)
        return result;

    const build_pass_env env = {
        .root = root,
        .profile = profile,
        .settings = profile_settings(ctx_out, profile),
        .env = &ctx_out->env,
        .chain = chain_out,
        .db = db,
        .options = options,
    };

    /* Dependencies first, and in one pass of their own: each is compiled
       against the language standard and its own recipe, so what reaches the
       compiler is the same in every project that depends on it — which is what
       makes one compiled object worth sharing. */
    result = plan_add(plan, &env, plan->package_units, str_list_count(&plan->package_sources),
                      objects_out);
    if(result != exit_ok)
        return result;

    plan->project_units =
        units_from_document(&plan->doc, doc_targets_project, &plan->sources, plan->labels);
    if(plan->project_units == NULL)
        return exit_build_failure;
    return plan_add(plan, &env, plan->project_units, str_list_count(&plan->sources), objects_out);
}

/* Write out what this build compiled, for whoever parses this code without
   being the build: clangd, clang-tidy, cppcheck (RFC-0007).
 *
 * It is published even when the build failed, because that is when an editor
 * that understands the project is worth the most — and a command line does not
 * become wrong just because the code it describes does not compile. Failing to
 * write it is a warning: nothing about the artifact depends on it. */
static void publish_compile_db(const compile_db *cdb, const char *root) {
    if(compile_db_count(cdb) == 0)
        return;
    if(!compile_db_write(cdb, root))
        fprintf(stderr, "molto: warning: could not write compile_commands.json; "
                        "editors and static analysers will have to guess\n");
}

int build_project(const char *root, build_profile profile, bool refresh_toolchain, size_t jobs,
                  char *out_binary, size_t out_binary_size) {
    return build_project_with(root, profile, refresh_toolchain, jobs, out_binary, out_binary_size,
                              NULL);
}

int build_project_with(const char *root, build_profile profile, bool refresh_toolchain, size_t jobs,
                       char *out_binary, size_t out_binary_size, build_report *report) {
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
    int result =
        plan_project(root, profile, db, refresh_toolchain, &options, &ctx, &chain, &objects, &plan);
    if(result == exit_ok) {
        report_plan(&plan, root, report);
        build_report_begin(report, plan.to_build);
        result = run_plan(&plan, report, &any_compiled);
    }
    const bool any_cpp = plan.any_cpp;
    publish_compile_db(options.cdb, root);
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
        if(!library_names_of(ctx.artifact, ctx.project_name, ctx.version, &names, name_err,
                             sizeof name_err) ||
           !compose_binary_path(root, profile, names.file, binary, sizeof binary) ||
           !fs_format_path(directory, sizeof directory, "%s/" DIR_BUILD "/%s", root,
                           profile_name(profile))) {
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
                archive_project(&objects, binary, &ctx.env, &chain, any_compiled, db, report);
        } else {
            produced = link_project(any_cpp, &objects, binary, node, &ctx.env, &chain, any_compiled,
                                    db, root, report);
            if(produced && node->kind == ir_target_shared)
                place_shared_links(directory, &names, report);
        }
        if(!produced)
            result = exit_build_failure;
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

    build_plan_free(&plan);
    str_list_free(&objects);
    warn_if_not_saved(db);
    return result;
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

/* Everything a test link needs beyond its own objects. */
typedef struct {
    const char *root;
    const char *profile_dir;
    const project_ctx *ctx;
    const resolved_toolchain *chain;
    /* The units the test targets describe, in the order `document_sources`
       produced their sources — so unit i belongs to binary i in per-file mode,
       and every unit shares the one target in single mode. It is where a link
       line comes from now. */
    const compile_unit *test_units;
    const str_list *lib_objects; /* src objects, minus the app's main */
    bool any_cpp;
    bool force; /* something was recompiled */
    wsdb *db;
    build_report *report; /* where a failed link says so */
} test_link_context;

/* Link `objects` into `binary`, and record it as one of the built tests. */
static bool link_one_test(const test_link_context *context, const str_list *objects,
                          const char *binary, bool cpp, const ir_target *node,
                          str_list *binaries_out) {
    if(!make_parent_dirs(binary))
        return false;
    if(!link_project(cpp, objects, binary, node, &context->ctx->env, context->chain, context->force,
                     context->db, context->root, context->report))
        return false;
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
                                 context->any_cpp || source_is_cpp(source),
                                 context->test_units[i].node, binaries_out);
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

    /* One target for the whole suite in this mode, so every unit names it. */
    ok = ok && link_one_test(context, &link_objects, binary, cpp, context->test_units[0].node,
                             binaries_out);
    str_list_free(&link_objects);
    return ok ? exit_ok : exit_build_failure;
}

int build_tests(const char *root, build_profile profile, bool refresh_toolchain, size_t jobs,
                str_list *test_binaries_out, project_env *env_out) {
    return build_tests_with(root, profile, refresh_toolchain, jobs, test_binaries_out, env_out,
                            NULL);
}

int build_tests_with(const char *root, build_profile profile, bool refresh_toolchain, size_t jobs,
                     str_list *test_binaries_out, project_env *env_out, build_report *report) {
    /* Cleared up front so a caller that keeps going after a failure runs
       nothing in a half-read environment. */
    if(env_out != NULL)
        memset(env_out, 0, sizeof *env_out);

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
    build_plan plan;
    build_plan_init(&plan);
    /* One database for the whole command, so what it describes is everything a
       test build compiles: the project, its dependencies, and tests/ — which is
       what makes `molto test` the command that leaves an editor able to follow
       a test into the code it exercises. */
    const pass_options options = {.jobs = jobs, .cdb = compile_db_create()};
    int result =
        plan_project(root, profile, db, refresh_toolchain, &options, &ctx, &chain, &objects, &plan);
    if(result != exit_ok) {
        build_plan_free(&plan);
        publish_compile_db(options.cdb, root);
        compile_db_destroy(options.cdb);
        str_list_free(&objects);
        warn_if_not_saved(db);
        return result;
    }
    if(env_out != NULL)
        *env_out = ctx.env;

    const build_pass_env env = {
        .root = root,
        .profile = profile,
        .settings = profile_settings(&ctx, profile),
        .env = &ctx.env,
        .chain = &chain,
        .db = db,
        .options = &options,
    };
    const char *profile_dir = profile_name(profile);

    /* Object of src/main.c (the app entry point), if any, to exclude from test
       links: the tests supply their own entry point. */
    char main_source[PATH_BUFFER_SIZE];
    char main_object[PATH_BUFFER_SIZE];
    char src_dir[PATH_BUFFER_SIZE];
    if(!fs_format_path(main_source, sizeof main_source, "%s/" DIR_SRC "/main.c", root) ||
       !object_path_for(root, profile_dir, main_source, main_object, sizeof main_object) ||
       !fs_format_path(src_dir, sizeof src_dir, "%s/" DIR_SRC, root)) {
        (void)fs_report_long_path(root);
        build_plan_free(&plan);
        publish_compile_db(options.cdb, root);
        compile_db_destroy(options.cdb);
        str_list_free(&objects);
        warn_if_not_saved(db);
        return exit_build_failure;
    }
    bool has_main = fs_path_exists(main_source);

    /*
     * What `[dev-deps]` adds is not added here any more, and there is nothing
     * left to do about it in this function.
     *
     * Their includes, defines, flags and libraries all reach the test targets
     * as nodes on those targets, put there by the fold — which folds a
     * development dependency into a target of kind `test` and into no other.
     * The separation RFC-0008 calls enforcement is one rule in one place rather
     * than three lists this function had to remember not to widen, and the
     * lists it used to widen are read by the frontend, which has already run.
     */

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
    if(result == exit_ok &&
       !document_sources(&plan.doc, root, doc_targets_dev_packages, &plan.dev_package_sources))
        result = exit_build_failure;
    if(result == exit_ok && str_list_count(&plan.dev_package_sources) > 0) {
        plan.dev_package_units = units_from_document(&plan.doc, doc_targets_dev_packages,
                                                     &plan.dev_package_sources, plan.labels);
        result = plan.dev_package_units == NULL
                     ? exit_build_failure
                     : plan_add(&plan, &env, plan.dev_package_units,
                                str_list_count(&plan.dev_package_sources), &lib_objects);
    }

    if(result == exit_ok &&
       !document_sources(&plan.doc, root, doc_targets_tests, &plan.test_sources))
        result = exit_build_failure;

    /* Compiled through the same path as the project's own sources, so tests get
       the parallel build, the dependency tracking and the up-to-date checks
       instead of a second implementation of all three. */
    str_list test_objects;
    str_list_init(&test_objects);
    if(result == exit_ok && str_list_count(&plan.test_sources) > 0) {
        plan.test_units =
            units_from_document(&plan.doc, doc_targets_tests, &plan.test_sources, plan.labels);
        result = plan.test_units == NULL
                     ? exit_build_failure
                     : plan_add(&plan, &env, plan.test_units, str_list_count(&plan.test_sources),
                                &test_objects);
    }

    /* Everything is planned, so the report can finally say how much there is —
       and only now does anything compile. The four passes run in the order
       they were planned, and the first that fails stops the rest. */
    if(result == exit_ok) {
        report_plan(&plan, root, report);
        build_report_begin(report, plan.to_build);
        result = run_plan(&plan, report, &any_compiled);
    }

    if(result == exit_ok) {
        const test_link_context context = {
            .root = root,
            .profile_dir = profile_dir,
            .ctx = &ctx,
            .chain = &chain,
            .test_units = plan.test_units,
            .lib_objects = &lib_objects,
            .any_cpp = plan.any_cpp,
            .force = any_compiled,
            .db = db,
            .report = report,
        };
        result =
            ctx.test.mode == test_mode_single
                ? link_tests_single(&context, &plan.test_sources, &test_objects, test_binaries_out)
                : link_tests_per_file(&context, &plan.test_sources, &test_objects,
                                      test_binaries_out);
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

    build_plan_free(&plan);
    publish_compile_db(options.cdb, root);
    compile_db_destroy(options.cdb);
    str_list_free(&test_objects);
    str_list_free(&lib_objects);
    str_list_free(&objects);

    warn_if_not_saved(db);
    return result;
}
