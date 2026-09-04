#include <molto/services/build_service.h>

#include "build_internal.h"

#include <molto/build/compile_db.h>
#include <molto/build/library.h>
#include <molto/build/report.h>
#include <molto/exit_code.h>
#include <molto/services/deps_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/ir_transform.h>
#include <molto/services/source_discovery.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/str_list.h>
#include <molto/workspace/wsdb.h>

#include <string.h>

/*
 * Building the test binaries, which is the same build with a different answer
 * to one question: what gets linked into what.
 *
 * A project links its objects into one artifact. A suite links each test source
 * against everything the project compiled, either once per file or all together
 * — `[test].mode`, which is the whole reason this is not `build_project` with a
 * flag. Both modes are here beside each other, because the thing a reader needs
 * to check is that they agree about everything except that.
 */

/* Output path of a test executable: build/<profile>/tests/<name>, mirroring the
   test source's path under tests/ with its extension stripped. */
[[nodiscard]] static bool test_binary_path(const char *root, const char *profile_dir,
                                           const char *test_source, char *out, size_t out_size) {
    char stem[PATH_BUFFER_SIZE];
    if(!fs_format_path(stem, sizeof stem, "%s", build_relative_to_root(root, test_source)))
        return fs_report_long_path(test_source);
    char *dot = strrchr(stem, '.');
    char *slash = strrchr(stem, '/');
    if(dot != NULL && (slash == NULL || dot > slash))
        *dot = '\0';
    return fs_format_path(out, out_size, "%s/" DIR_BUILD "/%s/%s" FS_EXECUTABLE_SUFFIX, root,
                          profile_dir, stem) ||
           fs_report_long_path(test_source);
}

/* Everything a test link needs beyond its own objects. */
typedef struct {
    const char *root;
    const char *profile_dir;
    const project_ctx *ctx;
    const resolved_toolchain *chain;
    /* The units the test targets describe, in the order `build_document_sources`
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
    if(!build_make_parent_dirs(binary))
        return false;
    if(!build_link_project(cpp, objects, binary, node, &context->ctx->env, context->chain,
                           context->force, context->db, context->root, context->report))
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
    if(!fs_format_path(binary, sizeof binary,
                       "%s/" DIR_BUILD "/%s/" DIR_TESTS "/%s%s" FS_EXECUTABLE_SUFFIX, context->root,
                       context->profile_dir, context->ctx->project_name, TEST_SUITE_SUFFIX)) {
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

/* Every object the project compiled except the one holding its `main`: a test
   binary brings its own entry point, and linking the app's would be two. */
[[nodiscard]] static int library_objects_of(const str_list *objects, const char *main_object,
                                            bool has_main, str_list *out) {
    for(size_t i = 0; i < str_list_count(objects); i++) {
        const char *object = str_list_get(objects, i);
        if(has_main && strcmp(object, main_object) == 0)
            continue;
        if(!str_list_push(out, object))
            return exit_build_failure;
    }
    return exit_ok;
}

/* A development dependency's own sources, planned as a pass of their own —
   each against its own options, exactly as a runtime dependency's are. Their
   objects join the test link rather than the project's, which is the whole
   separation RFC-0008 asks for. */
[[nodiscard]] static int plan_the_dev_packages(const char *root, const build_pass_env *env,
                                               build_plan *plan, str_list *lib_objects) {
    if(!build_document_sources(&plan->doc, root, doc_targets_dev_packages,
                               &plan->dev_package_sources))
        return exit_build_failure;
    if(str_list_count(&plan->dev_package_sources) == 0)
        return exit_ok;

    plan->dev_package_units = build_units_from_document(&plan->doc, doc_targets_dev_packages,
                                                        &plan->dev_package_sources, plan->labels);
    if(plan->dev_package_units == NULL)
        return exit_build_failure;
    return build_plan_add(plan, env, plan->dev_package_units,
                          str_list_count(&plan->dev_package_sources), lib_objects);
}

/* The test sources, planned through the same path the project's own take, so
   they get the parallel build, the dependency tracking and the up-to-date
   checks instead of a second implementation of all three. */
[[nodiscard]] static int plan_the_tests(const char *root, const build_pass_env *env,
                                        build_plan *plan, str_list *test_objects) {
    if(!build_document_sources(&plan->doc, root, doc_targets_tests, &plan->test_sources))
        return exit_build_failure;
    if(str_list_count(&plan->test_sources) == 0)
        return exit_ok;

    plan->test_units =
        build_units_from_document(&plan->doc, doc_targets_tests, &plan->test_sources, plan->labels);
    if(plan->test_units == NULL)
        return exit_build_failure;
    return build_plan_add(plan, env, plan->test_units, str_list_count(&plan->test_sources),
                          test_objects);
}

/* A deleted test leaves behind an object and an executable that `molto test`
   would happily keep running. Prune both (RFC-0004). */
static void prune_what_a_deleted_test_left(wsdb *db, const char *root, const char *profile_dir,
                                           const str_list *test_objects,
                                           const str_list *test_binaries) {
    char prefix[PATH_BUFFER_SIZE];
    if(fs_format_path(prefix, sizeof prefix, "%s/" DIR_BUILD "/%s/" DIR_OBJ "/" DIR_TESTS "/", root,
                      profile_dir))
        wsdb_prune(db, test_objects, prefix);
    if(fs_format_path(prefix, sizeof prefix, "%s/" DIR_BUILD "/%s/" DIR_TESTS "/", root,
                      profile_dir))
        wsdb_prune(db, test_binaries, prefix);
}

/* Link what was compiled into the binaries `molto test` will run: one per test
   source, or one for all of them, which is `[test].mode` and the only thing the
   two modes disagree about. */
[[nodiscard]] static int link_the_suite(const char *root, const char *profile_dir,
                                        const project_ctx *ctx, const resolved_toolchain *chain,
                                        const build_plan *plan, const str_list *lib_objects,
                                        str_list *test_objects, bool force, wsdb *db,
                                        build_report *report, str_list *test_binaries_out) {
    const test_link_context context = {
        .root = root,
        .profile_dir = profile_dir,
        .ctx = ctx,
        .chain = chain,
        .test_units = plan->test_units,
        .lib_objects = lib_objects,
        .any_cpp = plan->any_cpp,
        .force = force,
        .db = db,
        .report = report,
    };
    return ctx->test.mode == test_mode_single
               ? link_tests_single(&context, &plan->test_sources, test_objects, test_binaries_out)
               : link_tests_per_file(&context, &plan->test_sources, test_objects,
                                     test_binaries_out);
}

/*
 * Everything this command owns, released in one place.
 *
 * Four ways out and a different subset freed on three of them is how one of
 * them came to leak the compilation database and leave the workspace unsaved:
 * the path it needs is one no real project produces, so nobody ever took it and
 * nothing ever said so. One function that every exit goes through cannot drift
 * like that.
 *
 * `result` passes through so a caller can `return finish_tests(code, ...)` and
 * have one statement mean both.
 */
static int finish_tests(int result, build_plan *plan, compile_db *cdb, const char *root,
                        str_list *objects, str_list *lib_objects, str_list *test_objects,
                        wsdb *db) {
    build_plan_free(plan);
    build_publish_compile_db(cdb, root);
    compile_db_destroy(cdb);
    str_list_free(test_objects);
    str_list_free(lib_objects);
    str_list_free(objects);
    build_warn_if_not_saved(db);
    return result;
}

int build_tests(const char *root, build_profile profile, const char *platform,
                bool refresh_toolchain, size_t jobs, str_list *test_binaries_out,
                project_env *env_out) {
    return build_tests_with(root, profile, platform, refresh_toolchain, jobs, test_binaries_out,
                            env_out, NULL);
}

int build_tests_with(const char *root, build_profile profile, const char *platform,
                     bool refresh_toolchain, size_t jobs, str_list *test_binaries_out,
                     project_env *env_out, build_report *report) {
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
    str_list lib_objects;
    str_list test_objects;
    str_list_init(&objects);
    str_list_init(&lib_objects);
    str_list_init(&test_objects);
    bool any_compiled = false;
    build_plan plan;
    build_plan_init(&plan);
    /* One database for the whole command, so what it describes is everything a
       test build compiles: the project, its dependencies, and tests/ — which is
       what makes `molto test` the command that leaves an editor able to follow
       a test into the code it exercises. */
    const pass_options options = {.jobs = jobs, .cdb = compile_db_create()};
    int result = build_plan_project(root, profile, platform, refresh_toolchain, db, &options, &ctx,
                                    &chain, &objects, &plan);
    if(result != exit_ok)
        return finish_tests(result, &plan, options.cdb, root, &objects, &lib_objects, &test_objects,
                            db);
    if(env_out != NULL)
        *env_out = ctx.env;

    char profile_dir_storage[PATH_BUFFER_SIZE];
    if(!build_segment(profile, platform, profile_dir_storage, sizeof profile_dir_storage)) {
        (void)fs_report_long_path(root);
        return finish_tests(exit_build_failure, &plan, options.cdb, root, &objects, &lib_objects,
                            &test_objects, db);
    }
    const char *profile_dir = profile_dir_storage;

    const build_pass_env env = {
        .root = root,
        .profile = profile,
        .segment = profile_dir,
        .settings = build_profile_settings(&ctx, profile),
        .env = &ctx.env,
        .chain = &chain,
        .db = db,
        .options = &options,
    };

    /* Object of src/main.c (the app entry point), if any, to exclude from test
       links: the tests supply their own entry point. */
    char main_source[PATH_BUFFER_SIZE];
    char main_object[PATH_BUFFER_SIZE];
    if(!fs_format_path(main_source, sizeof main_source, "%s/" DIR_SRC "/main.c", root) ||
       !build_object_path_for(root, profile_dir, main_source, main_object, sizeof main_object)) {
        (void)fs_report_long_path(root);
        return finish_tests(exit_build_failure, &plan, options.cdb, root, &objects, &lib_objects,
                            &test_objects, db);
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

    if(result == exit_ok)
        result = library_objects_of(&objects, main_object, has_main, &lib_objects);

    if(result == exit_ok)
        result = plan_the_dev_packages(root, &env, &plan, &lib_objects);

    if(result == exit_ok)
        result = plan_the_tests(root, &env, &plan, &test_objects);

    /* Everything is planned, so the report can finally say how much there is —
       and only now does anything compile. The four passes run in the order
       they were planned, and the first that fails stops the rest. */
    if(result == exit_ok) {
        build_report_plan(&plan, root, report);
        build_report_begin(report, plan.to_build);
        result = build_run_plan(&plan, report, &any_compiled);
    }

    if(result == exit_ok)
        result = link_the_suite(root, profile_dir, &ctx, &chain, &plan, &lib_objects, &test_objects,
                                any_compiled, db, report, test_binaries_out);

    if(result == exit_ok)
        prune_what_a_deleted_test_left(db, root, profile_dir, &test_objects, test_binaries_out);

    return finish_tests(result, &plan, options.cdb, root, &objects, &lib_objects, &test_objects,
                        db);
}
