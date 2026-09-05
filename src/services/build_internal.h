#ifndef MOLTO_BUILD_INTERNAL_H
#define MOLTO_BUILD_INTERNAL_H

/*
 * What the build service's own files share, and nothing else does.
 *
 * `build_service.c` was one file of 2324 lines. It is now six, cut where the
 * call graph already had seams: what it names and where it puts things
 * (`build_layout.c`), compiling (`build_compile.c`), linking and archiving
 * (`build_link.c`), resolving dependencies and taking the lock
 * (`build_prepare.c`), the test binaries (`build_tests.c`), and the six public
 * entry points that drive them (`build_service.c`).
 *
 * This header is why that could be done without widening anything. The public
 * API is still the six functions in <molto/services/build_service.h>; what is
 * declared here is internal, and it lives under src/ rather than under
 * include/molto/ so that including it from anywhere else is a thing a reader
 * notices. Nothing outside src/services/build_*.c may include it.
 */

#include <molto/build/compile_db.h>
#include <molto/build/diagnostic.h>
#include <molto/build/library.h>
#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/project/project_ctx.h>
#include <molto/services/deps_service.h>
#include <molto/services/ir_service.h>
#include <molto/services/process_service.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/str_list.h>
#include <molto/workspace/wsdb.h>

#include <stdbool.h>
#include <stddef.h>

/* Compiler command-line arguments. */
#define ARG_COMPILE "-c"       /* compile only, do not link */
#define ARG_OUTPUT "-o"        /* next argument is the output path */
#define ARG_SHARED "-shared"   /* link a shared library rather than a program */
#define ARG_DEBUG "-g"         /* emit debug symbols */
#define ARG_DEPFILE_GEN "-MMD" /* also write a header-dependency file */
#define ARG_DEPFILE_OUT "-MF"  /* next argument is the dependency file path */
#define OPT_FLAG_FORMAT "-O%d" /* optimisation level, e.g. -O2 */

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

/* --- what a pass compiles, and what a plan is made of --- */

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
    /* Where output goes under `build/`: the profile, and the target before it
       when one was named. Carried rather than recomposed, so every object of
       one build lands under one directory. */
    const char *segment;
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

/* --- build_layout.c: where a thing goes, and what it is called --- */

/* Whether `target` belongs to the set of a document's targets one pass is
   about. */
[[nodiscard]] bool build_in_set(const ir_document *doc, const ir_target *target,
                                doc_target_set set);

/* What a target's paths are relative to: its package's root where it has one,
   and the project's otherwise (RFC-0013). */
[[nodiscard]] const char *build_target_root(const ir_document *doc, const ir_target *target,
                                            const char *root);

/* One label per target, indexed by the target's position in the document.
   Caller frees. NULL means the allocation failed. */
[[nodiscard]] build_unit_label *build_labels_for(const ir_document *doc);

/* Where this build's output goes under `build/`: the profile, with the target
   in front of it when one was named. */
[[nodiscard]] bool build_segment(build_profile profile, const char *platform, char *out,
                                 size_t out_size);

/* Where the artifact lands, which is the one place that wants a filename
   rather than a name. */
[[nodiscard]] bool build_compose_binary_path(const char *root, const char *segment,
                                             const char *name, ir_target_kind kind, char *out,
                                             size_t out_size);

/* Select the settings for `profile` from a parsed project context. */
[[nodiscard]] manifest_profile build_profile_settings(const project_ctx *ctx,
                                                      build_profile profile);

/* Map a source path to its object path, mirroring the source tree under
   `root/build/<profile_dir>/obj`. */
[[nodiscard]] bool build_object_path_for(const char *root, const char *profile_dir,
                                         const char *source, char *out, size_t out_size);

/* The dependency file gcc writes next to an object: "<object>.d". */
[[nodiscard]] bool build_depfile_path_for(const char *object, char *out, size_t out_size);

/* Create the parent directory chain for `path`. */
[[nodiscard]] bool build_make_parent_dirs(const char *path);

/* Join argv items into one space-separated heap string (caller frees). */
[[nodiscard]] char *build_join_args(const str_list *argv);

/* Portion of `path` relative to `root`, or `path` unchanged if not under it. */
[[nodiscard]] const char *build_relative_to_root(const char *root, const char *path);

/* Which compiler was asked, as a person would name it. */
void build_describe_compiler(const resolved_toolchain *chain, bool is_cpp, char *out,
                             size_t out_size);

/* --- build_compile.c: running a tool, and what it said --- */

/* Run `argv`, capturing what it wrote. Returns the child's exit status, or a
   negative value when it could not be started. */
[[nodiscard]] int build_run_str_argv(const str_list *argv, const project_env *env, char *capture,
                                     size_t capture_size, bool *truncated);

/* The fingerprint of a command line and the environment it would run in
   (caller frees). NULL when it could not be composed. */
[[nodiscard]] char *build_command_fingerprint(const str_list *argv, const project_env *env);

/* A diagnostic Molto wrote itself, for what a tool left unsaid. */
void build_push_own(diagnostic_list *found, const char *source, diagnostic_severity severity,
                    const char *message);

/* --- build_link.c: objects into the thing that ships --- */

/* Link `objects` into `binary`. False when the link failed, which the report it
   was given already says in full. */
[[nodiscard]] bool build_link_project(bool any_cpp, const str_list *objects, const char *binary,
                                      const ir_target *node, const library_names *names,
                                      const project_env *env, const resolved_toolchain *chain,
                                      bool force, wsdb *db, const char *root, build_report *report);

/* Archive `objects` into a static library. */
[[nodiscard]] bool build_archive_project(const str_list *objects, const char *archive,
                                         const project_env *env, const resolved_toolchain *chain,
                                         bool force, wsdb *db, build_report *report);

/* The two links that sit beside a shared library and make it loadable. */
void build_place_shared_links(const char *directory, const library_names *names,
                              build_report *report);

/* --- build_prepare.c: everything true before the first compiler runs --- */

/* Read and parse the project's manifest. exit_ok, or the code to leave with. */
[[nodiscard]] int build_load_project(const char *root, project_ctx *out);

/* Close the workspace database, warning rather than failing when it could not
   be saved: the build already happened, and only the next one pays. */
void build_warn_if_not_saved(wsdb *db);

/* Resolve what the manifest depends on and take the workspace lock. False with
   a message in `err`; a project with no dependencies at all succeeds without
   doing anything. */
[[nodiscard]] bool build_prepare_and_lock(const char *root, project_ctx *ctx, prepared_deps *out,
                                          prepared_deps *dev_out, char *err, size_t err_size);

/* --- build_service.c: the plan, and what a document says --- */

/* The sources of a document's targets in `set`, appended to `out`. */
[[nodiscard]] bool build_document_sources(const ir_document *doc, const char *root,
                                          doc_target_set set, str_list *out);

/* An arena-owning plan of every pass a build will make. */
void build_plan_init(build_plan *plan);

void build_plan_free(build_plan *plan);

/* One pass's units, planned and added to `plan`. exit_ok, or the code to leave
   with. */
[[nodiscard]] int build_plan_add(build_plan *plan, const build_pass_env *env,
                                 const compile_unit *units, size_t count, str_list *objects);

/* Everything a build of the project itself will do, worked out before any of it
   happens: manifest read, dependencies resolved, toolchain resolved, document
   built, every pass planned. */
[[nodiscard]] int build_plan_project(const char *root, build_profile profile, const char *platform,
                                     bool refresh_toolchain, wsdb *db, const pass_options *options,
                                     project_ctx *ctx_out, resolved_toolchain *chain_out,
                                     str_list *objects_out, build_plan *plan);

/* Run every planned pass. */
int build_run_plan(const build_plan *plan, build_report *report, bool *any_compiled);

/* Tell the report what the plan is going to do, before it starts. */
void build_report_plan(const build_plan *plan, const char *root, build_report *report);

/* Write the compilation database where an editor will look for it. */
void build_publish_compile_db(const compile_db *cdb, const char *root);

/* One compile_unit per source in `set`, from an arena the caller frees. */
[[nodiscard]] compile_unit *build_units_from_document(const ir_document *doc, doc_target_set set,
                                                      const str_list *sources,
                                                      const build_unit_label *labels);

#endif /* MOLTO_BUILD_INTERNAL_H */
