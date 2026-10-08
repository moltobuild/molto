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

/* Output path of something built for one test source:
   build/<profile>/tests/<name><suffix>, mirroring the source's path under
   tests/ with its extension stripped. */
[[nodiscard]] static bool test_output_path(const char *root, const char *profile_dir,
                                           const char *test_source, const char *suffix, char *out,
                                           size_t out_size) {
    char stem[PATH_BUFFER_SIZE];
    if(!fs_format_path(stem, sizeof stem, "%s", build_relative_to_root(root, test_source)))
        return fs_report_long_path(test_source);
    char *dot = strrchr(stem, '.');
    char *slash = strrchr(stem, '/');
    if(dot != NULL && (slash == NULL || dot > slash))
        *dot = '\0';
    return fs_format_path(out, out_size, "%s/" DIR_BUILD "/%s/%s%s", root, profile_dir, stem,
                          suffix) ||
           fs_report_long_path(test_source);
}

/* Output path of a test executable. */
[[nodiscard]] static bool test_binary_path(const char *root, const char *profile_dir,
                                           const char *test_source, char *out, size_t out_size) {
    return test_output_path(root, profile_dir, test_source, FS_EXECUTABLE_SUFFIX, out, out_size);
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
    /* Where in `lib_objects` the development dependencies' objects begin: an
       isolated test links those loose and archives the rest (RFC-0021). */
    size_t dev_start;
    const build_plan *plan; /* what compiled each object, to find a replacement */
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
    /* No library names: a test binary is an executable, and the only thing they
       carry is the name to record inside a shared library. */
    if(!build_link_project(cpp, objects, binary, node, NULL, &context->ctx->env, context->chain,
                           context->force, context->db, context->root, context->report,
                           context->plan))
        return false;
    return str_list_push(binaries_out, binary);
}

/* --- Isolated tests (RFC-0021) --- */

/* Whether `unit` is what replacement `entry` names: a source of the project
   ("src/a.c"), one of a dependency ("dep:src/a.c"), or any of a dependency's
   ("dep"). Paths are the document's, relative to the package's own root. */
static bool replacement_names(const compile_unit *unit, const char *entry) {
    const char *package = unit->node->package;
    const char *path = unit->unit->path;
    const char *colon = strchr(entry, ':');
    if(colon != NULL) {
        const size_t length = (size_t)(colon - entry);
        return package != NULL && strlen(package) == length &&
               strncmp(package, entry, length) == 0 && strcmp(path, colon + 1) == 0;
    }
    if(strchr(entry, '/') != NULL)
        return package == NULL && strcmp(path, entry) == 0;
    return package != NULL && strcmp(package, entry) == 0;
}

/* The objects `entry` leaves out. Each replacement has to name a source the
   project or a runtime dependency compiles: one that names nothing leaves the
   real function in place beside the fake, and a development dependency is the
   test framework, not code under test. Either is a manifest error. */
[[nodiscard]] static int replaced_objects_of(const build_plan *plan,
                                             const project_isolated_test *entry,
                                             build_report *report, str_list *out) {
    for(size_t r = 0; r < entry->replace_count; r++) {
        const char *replacement = entry->replaces[r];
        bool found = false;
        for(size_t p = 0; p < plan->pass_count; p++) {
            const compile_pass *pass = &plan->passes[p];
            for(size_t u = 0; u < pass->count; u++) {
                const compile_unit *unit = pass->units[u].unit;
                if(unit->node->kind == ir_target_test || !replacement_names(unit, replacement))
                    continue;
                if(build_in_set(&plan->doc, unit->node, doc_targets_dev_packages)) {
                    build_report_message(report,
                                         "molto: [[test.isolated]] '%s' replaces '%s', a "
                                         "development dependency: the test framework is not "
                                         "code under test\n",
                                         entry->file, replacement);
                    return exit_invalid_manifest;
                }
                found = true;
                if(!str_list_push(out, pass->units[u].object))
                    return exit_build_failure;
            }
        }
        if(!found) {
            build_report_message(report,
                                 "molto: [[test.isolated]] '%s' replaces '%s', which is not a "
                                 "source of this build\n",
                                 entry->file, replacement);
            return exit_invalid_manifest;
        }
    }
    return exit_ok;
}

/* Whether `list` holds `text`. */
static bool list_holds(const str_list *list, const char *text) {
    for(size_t i = 0; i < str_list_count(list); i++) {
        if(strcmp(str_list_get(list, i), text) == 0)
            return true;
    }
    return false;
}

/* The `[[test.isolated]]` entry for `source`, or NULL when it links as usual. */
static const project_isolated_test *isolated_entry_for(const char *root, const project_test *test,
                                                       const char *source) {
    const char *relative = build_relative_to_root(root, source);
    for(size_t i = 0; i < test->isolated_count; i++) {
        if(strcmp(test->isolated[i].file, relative) == 0)
            return &test->isolated[i];
    }
    return NULL;
}

/* Whether `source` is a test file rather than one of `[test].sources`. */
static bool is_test_file(const char *root, const char *source) {
    return strncmp(build_relative_to_root(root, source), DIR_TESTS "/", sizeof DIR_TESTS) == 0;
}

/* Every entry names a test source and real replacements, checked once the
   plan knows every source and before anything compiles. */
[[nodiscard]] static int check_isolated_tests(const char *root, const project_test *test,
                                              const build_plan *plan, build_report *report) {
    for(size_t i = 0; i < test->isolated_count; i++) {
        const project_isolated_test *entry = &test->isolated[i];
        bool is_a_test = false;
        for(size_t s = 0; !is_a_test && s < str_list_count(&plan->test_sources); s++) {
            const char *source = str_list_get(&plan->test_sources, s);
            is_a_test = isolated_entry_for(root, test, source) == entry;
        }
        if(!is_a_test) {
            build_report_message(
                report, "molto: [[test.isolated]] file '%s' is not a test source\n", entry->file);
            return exit_invalid_manifest;
        }
        str_list replaced;
        str_list_init(&replaced);
        const int result = replaced_objects_of(plan, entry, report, &replaced);
        str_list_free(&replaced);
        if(result != exit_ok)
            return result;
    }
    return exit_ok;
}

/*
 * An isolated test's executable: its own object, `loose` (a framework's
 * sources in single mode) and the development dependencies' objects as they
 * are, then everything else the project and its runtime dependencies compiled,
 * minus the replaced sources, as one static archive.
 *
 * The archive is the point. Linked loose, every remaining object is in the
 * binary, and each one that calls into a replaced source needs a fake whether
 * the test reaches it or not. From an archive the linker takes only the
 * members the test reaches. It takes each of those whole, so the test fakes
 * what they call, used or not, and nothing beyond them.
 * One archive needs no --start-group: a linker rescans an archive's own index
 * until it stops pulling members. The framework stays loose because a
 * constructor in a member nothing references would never be pulled in.
 */
[[nodiscard]] static int link_isolated_test(const test_link_context *context,
                                            const project_isolated_test *entry, const char *source,
                                            const char *object, const str_list *loose, bool cpp,
                                            const ir_target *node, str_list *binaries_out) {
    char binary[PATH_BUFFER_SIZE];
    char archive[PATH_BUFFER_SIZE];
    if(!test_binary_path(context->root, context->profile_dir, source, binary, sizeof binary) ||
       !test_output_path(context->root, context->profile_dir, source, ".isolated.a", archive,
                         sizeof archive))
        return exit_build_failure;

    str_list replaced;
    str_list archived;
    str_list link_objects;
    str_list_init(&replaced);
    str_list_init(&archived);
    str_list_init(&link_objects);
    int result = replaced_objects_of(context->plan, entry, context->report, &replaced);
    bool ok = result == exit_ok && str_list_push(&link_objects, object);
    for(size_t i = 0; ok && i < str_list_count(loose); i++)
        ok = str_list_push(&link_objects, str_list_get(loose, i));
    const size_t total = str_list_count(context->lib_objects);
    for(size_t i = context->dev_start; ok && i < total; i++)
        ok = str_list_push(&link_objects, str_list_get(context->lib_objects, i));
    for(size_t i = 0; ok && i < context->dev_start; i++) {
        const char *kept = str_list_get(context->lib_objects, i);
        size_t length = strlen(kept);
        if(length >= 8 && !strcmp(kept + length - 8, ".entry.a"))
            ok = str_list_push(&link_objects, kept);
        else if(!list_holds(&replaced, kept))
            ok = str_list_push(&archived, kept);
    }
    if(ok && str_list_count(&archived) > 0)
        ok = build_make_parent_dirs(archive) &&
             build_archive_project(&archived, archive, &context->ctx->env, context->chain,
                                   context->force, context->db, context->report) &&
             str_list_push(&link_objects, archive);
    if(ok && !link_one_test(context, &link_objects, binary, cpp, node, binaries_out)) {
        /* What the linker cannot know. A symbol it missed lived in a file this
           test asked to leave out, or was a fake another test file defines for
           the shared suite: this executable links neither. */
        build_report_message(context->report,
                             "molto: note: %s replaces %s%s and links alone. A missing symbol "
                             "is a function of a replaced file, or a fake another test file "
                             "defines for the rest of the suite; define it in this test\n",
                             entry->file, entry->replaces[0],
                             entry->replace_count > 1 ? " and more" : "");
        ok = false;
    }
    if(result == exit_ok && !ok)
        result = exit_build_failure;
    str_list_free(&link_objects);
    str_list_free(&archived);
    str_list_free(&replaced);
    return result;
}

/* One executable per test file: each links its own object with the project's
   library objects, and brings its own main(). */
static int link_tests_per_file(const test_link_context *context, const str_list *test_sources,
                               const str_list *test_objects, str_list *binaries_out) {
    for(size_t i = 0; i < str_list_count(test_sources); i++) {
        const char *source = str_list_get(test_sources, i);
        const char *object = str_list_get(test_objects, i);
        const project_isolated_test *entry =
            isolated_entry_for(context->root, &context->ctx->test, source);
        if(entry != NULL) {
            const str_list none = {0};
            const int result = link_isolated_test(context, entry, source, object, &none,
                                                  context->any_cpp || source_is_cpp(source),
                                                  context->test_units[i].node, binaries_out);
            if(result != exit_ok)
                return result;
            continue;
        }

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

    /* An isolated file leaves the suite for an executable of its own; the
       framework's sources (outside tests/) go into every executable, since
       that is where main() is. */
    str_list link_objects;
    str_list framework;
    str_list_init(&link_objects);
    str_list_init(&framework);
    bool ok = true;
    bool cpp = context->any_cpp;
    size_t shared_tests = 0;
    for(size_t i = 0; ok && i < str_list_count(test_objects); i++) {
        const char *source = str_list_get(test_sources, i);
        cpp = cpp || source_is_cpp(source);
        if(isolated_entry_for(context->root, &context->ctx->test, source) != NULL)
            continue;
        ok = str_list_push(&link_objects, str_list_get(test_objects, i));
        if(ok && is_test_file(context->root, source))
            shared_tests++;
        else if(ok)
            ok = str_list_push(&framework, str_list_get(test_objects, i));
    }
    for(size_t i = 0; ok && i < str_list_count(context->lib_objects); i++)
        ok = str_list_push(&link_objects, str_list_get(context->lib_objects, i));

    /* One target for the whole suite in this mode, so every unit names it.
       Not linked at all when every test file is isolated: the framework alone
       is a suite of nothing. */
    if(ok && shared_tests > 0) {
        ok = link_one_test(context, &link_objects, binary, cpp, context->test_units[0].node,
                           binaries_out);
        /* No framework at all: nothing supplies main(), and all the linker
           can say is that main is undefined, not why. */
        const bool no_framework = str_list_count(&framework) == 0 &&
                                  context->dev_start == str_list_count(context->lib_objects);
        if(!ok && no_framework)
            build_report_message(context->report,
                                 "molto: note: [test] mode = \"single\" links every test into "
                                 "one executable whose main() a test framework supplies, and "
                                 "this build has none. Add one to [dev-deps] (moltest), or use "
                                 "mode = \"per_file\" with a main() in each test\n");
    }
    int result = ok ? exit_ok : exit_build_failure;
    for(size_t i = 0; result == exit_ok && i < str_list_count(test_objects); i++) {
        const char *source = str_list_get(test_sources, i);
        const project_isolated_test *entry =
            isolated_entry_for(context->root, &context->ctx->test, source);
        if(entry != NULL)
            result = link_isolated_test(context, entry, source, str_list_get(test_objects, i),
                                        &framework, cpp, context->test_units[0].node, binaries_out);
    }
    str_list_free(&framework);
    str_list_free(&link_objects);
    return result;
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
                                        size_t dev_start, str_list *test_objects, bool force,
                                        wsdb *db, build_report *report,
                                        str_list *test_binaries_out) {
    const test_link_context context = {
        .root = root,
        .profile_dir = profile_dir,
        .ctx = ctx,
        .chain = chain,
        .test_units = plan->test_units,
        .lib_objects = lib_objects,
        .dev_start = dev_start,
        .plan = plan,
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
                            env_out, NULL, NULL);
}

int build_tests_with(const char *root, build_profile profile, const char *platform,
                     bool refresh_toolchain, size_t jobs, str_list *test_binaries_out,
                     project_env *env_out, resolved_toolchain *chain_out, build_report *report) {
    /* Cleared up front so a caller that keeps going after a failure runs
       nothing in a half-read environment, or under a half-read toolchain. */
    if(env_out != NULL)
        memset(env_out, 0, sizeof *env_out);
    if(chain_out != NULL)
        memset(chain_out, 0, sizeof *chain_out);

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
    if(chain_out != NULL)
        *chain_out = chain;

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

    const size_t dev_start = str_list_count(&lib_objects);
    if(result == exit_ok)
        result = plan_the_dev_packages(root, &env, &plan, &lib_objects);

    if(result == exit_ok)
        result = plan_the_tests(root, &env, &plan, &test_objects);

    if(result == exit_ok)
        result = check_isolated_tests(root, &ctx.test, &plan, report);

    /* Everything is planned, so the report can finally say how much there is —
       and only now does anything compile. The four passes run in the order
       they were planned, and the first that fails stops the rest. */
    if(result == exit_ok) {
        build_report_plan(&plan, root, report);
        build_report_begin(report, plan.to_build);
        result = build_run_plan(&plan, report, &any_compiled);
    }

    str_list link_objects;
    str_list_init(&link_objects);
    if(result == exit_ok && !build_entry_objects(&plan, &lib_objects, &link_objects, report))
        result = exit_build_failure;
    if(result == exit_ok)
        result = link_the_suite(root, profile_dir, &ctx, &chain, &plan, &link_objects, dev_start,
                                &test_objects, any_compiled, db, report, test_binaries_out);

    if(result == exit_ok)
        prune_what_a_deleted_test_left(db, root, profile_dir, &test_objects, test_binaries_out);

    str_list_free(&link_objects);
    return finish_tests(result, &plan, options.cdb, root, &objects, &lib_objects, &test_objects,
                        db);
}
