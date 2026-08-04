#include <molto/services/lint_service.h>

#include <molto/build/compile_flags.h>
#include <molto/exit_code.h>
#include <molto/project/project_ctx.h>
#include <molto/project/style_config.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/services/style_translate.h>
#include <molto/services/tool_service.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/task_pool.h>
#include <molto/workspace/wsdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* On-disk layout this command reads. */
#define MANIFEST_FILENAME "Project.toml"
#define DIR_SRC "src"

/* Compiler arguments for a pass that checks and produces nothing. */
#define ARG_SYNTAX_ONLY "-fsyntax-only"
#define ARG_NO_COLOR "-fdiagnostics-color=never"
#define ARG_INCLUDE_SRC "-I%s/" DIR_SRC

/* The warnings the `molto` preset asks a compiler for. It is what Molto's own
   Makefile uses and what spec.md section 17 describes. */
#define PRESET_MOLTO_WARNINGS_COUNT 3
static const char *const preset_molto_warnings[PRESET_MOLTO_WARNINGS_COUNT] = {
    "-Wall",
    "-Wextra",
    "-Wpedantic",
};

/* clang-tidy arguments. Everything after the separator is what the file would
   have been compiled with, which is how a linter sees the same code the build
   does. */
#define ARG_CONFIG_FILE "--config-file=%s"
#define ARG_QUIET "--quiet" /* the tally is Molto's to report, not its */
#define ARG_SEPARATOR "--"

/* Size of the stack buffers used to compose paths. */
#define PATH_BUFFER_SIZE 4096

/* Size of the buffer receiving what a tool says about one file. Roughly 1600
   diagnostic lines; beyond that the truncation is reported, never silent. */
#define LINT_OUTPUT_SIZE 65536

/* Size of the buffer receiving a configuration error. */
#define CONFIG_ERROR_SIZE 512

/* One pass over one file. The worker runs the tool and parses what it said;
   what a failure means is decided on the main thread, where the order of the
   diagnostics is still the order of the sources. */
typedef struct {
    const char *source; /* absolute, borrowed from the sorted list */
    const char *tool;   /* what to blame when the run itself fails */
    const str_list *argv;
    const project_env *env;
    int status;
    bool truncated;
    diagnostic_list found;
} lint_task;

/* Everything resolved before a single process is spawned. Validating first is
   the point: a rule that cannot be translated should not cost N compilations
   before it is reported. */
typedef struct {
    project_ctx ctx;
    lint_config config;
    resolved_toolchain chain;
    resolved_tool linter;
    bool has_linter;
    char linter_config[STYLE_CONFIG_PATH_MAX];
} lint_setup;

/* Load the manifest with the public API; no new entry point is needed. */
static int load_manifest(const char *root, project_ctx *out) {
    char path[PATH_BUFFER_SIZE];
    if(!fs_format_path(path, sizeof path, "%s/" MANIFEST_FILENAME, root)) {
        (void)fs_report_long_path(root);
        return exit_invalid_manifest;
    }

    char err[CONFIG_ERROR_SIZE] = "";
    if(!project_load(path, out, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_invalid_manifest;
    }
    return exit_ok;
}

/* Collect the sources to analyze, minus what linter.json excludes. */
static bool collect_sources(const char *root, const style_excludes *excludes, str_list *out) {
    char src_dir[PATH_BUFFER_SIZE];
    if(!fs_format_path(src_dir, sizeof src_dir, "%s/" DIR_SRC, root))
        return fs_report_long_path(root);
    if(!fs_is_dir(src_dir))
        return true; /* nothing to lint is not a failure */

    str_list found;
    str_list_init(&found);
    bool ok = source_discovery_collect(src_dir, &found);
    for(size_t i = 0; ok && i < str_list_count(&found); i++) {
        const char *path = str_list_get(&found, i);
        if(!style_excludes_match(excludes, fs_relative_to(path, root)))
            ok = str_list_push(out, path);
    }
    str_list_free(&found);
    return ok;
}

/* Whether any source needs a C++ driver, which is part of what is asked of the
   toolchain. */
static bool needs_cpp(const str_list *sources) {
    for(size_t i = 0; i < str_list_count(sources); i++) {
        if(source_is_cpp(str_list_get(sources, i)))
            return true;
    }
    return false;
}

/* The per-profile extra options, as build_service selects them. */
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

/* The compile arguments both passes share: what the file would be built with,
   minus everything to do with producing an object.

   -O and -g are left out on purpose. A syntax-only pass stops before
   optimisation, so -O2 would be inert for anything needing dataflow
   (-Wmaybe-uninitialized, -Wstringop-overflow); passing it would advertise
   diagnostics the pass cannot deliver.

   The defines, includes and flags of [target] and [profile.*] do go through,
   and that is not optional: #ifdef makes a define decide what even compiles, so
   dropping them would have lint and build disagree about what the code says. */
static bool push_compile_arguments(str_list *argv, const char *root, const lint_setup *setup,
                                   build_profile profile, bool is_cpp) {
    return compile_flags_push_std(argv, &setup->ctx.target, is_cpp) &&
           compile_flags_push_options(argv, root, &setup->ctx.target.options) &&
           compile_flags_push_options(argv, root, profile_options_for(&setup->ctx, profile));
}

/* The syntax-only pass. The preset's warnings go before the project's own
   flags: in gcc and clang the last one wins, so a preset -Wall appended after a
   project's -Wno-unused would silently re-enable what it deliberately turned
   off. */
static bool build_compiler_argv(str_list *argv, const char *root, const lint_setup *setup,
                                build_profile profile, const char *source) {
    bool is_cpp = source_is_cpp(source);
    const char *driver = compile_flags_driver(&setup->chain, is_cpp);
    if(driver == NULL) {
        fprintf(stderr, "molto: '%s' needs a C++ compiler and none was resolved\n", source);
        return false;
    }

    bool ok = str_list_push(argv, driver) && str_list_push(argv, ARG_SYNTAX_ONLY) &&
              str_list_push(argv, ARG_NO_COLOR) && str_list_push(argv, source);
    if(ok && setup->config.preset == style_preset_molto) {
        for(size_t i = 0; ok && i < PRESET_MOLTO_WARNINGS_COUNT; i++)
            ok = str_list_push(argv, preset_molto_warnings[i]);
    }
    if(ok)
        ok = push_compile_arguments(argv, root, setup, profile, is_cpp);

    char include_src[PATH_BUFFER_SIZE];
    if(!fs_format_path(include_src, sizeof include_src, ARG_INCLUDE_SRC, root))
        return fs_report_long_path(root);
    return ok && str_list_push(argv, include_src);
}

/* The linter pass, told where the translated configuration is and given the
   same compile arguments after the separator. */
static bool build_linter_argv(str_list *argv, const char *root, const lint_setup *setup,
                              build_profile profile, const char *source) {
    char config[PATH_BUFFER_SIZE];
    if(!fs_format_path(config, sizeof config, ARG_CONFIG_FILE, setup->linter_config))
        return fs_report_long_path(setup->linter_config);

    bool ok = str_list_push(argv, setup->linter.path) && str_list_push(argv, config) &&
              str_list_push(argv, ARG_QUIET) && str_list_push(argv, source) &&
              str_list_push(argv, ARG_SEPARATOR) &&
              push_compile_arguments(argv, root, setup, profile, source_is_cpp(source));

    char include_src[PATH_BUFFER_SIZE];
    if(!fs_format_path(include_src, sizeof include_src, ARG_INCLUDE_SRC, root))
        return fs_report_long_path(root);
    return ok && str_list_push(argv, include_src);
}

/* Run one pass and read everything it said. The parse happens here, in the
   worker: the buffer is local and freed at once, so the peak is one buffer per
   worker rather than one per source, and the parsing parallelizes for free. */
static void lint_task_run(void *argument) {
    lint_task *task = argument;

    char *output = malloc(LINT_OUTPUT_SIZE);
    const char **argv = process_argv_from_list(task->argv);
    if(output == NULL || argv == NULL) {
        task->status = -1;
        free(output);
        free((void *)argv);
        return;
    }

    process_env_var vars[PROJECT_MAX_ENV];
    size_t var_count = project_env_to_vars(task->env, vars, PROJECT_MAX_ENV);
    task->status =
        process_capture_all(argv, vars, var_count, output, LINT_OUTPUT_SIZE, &task->truncated);
    (void)diagnostic_parse(output, &task->found);

    free(output);
    free((void *)argv);
}

/* Add a diagnostic Molto itself produced about a file. */
static bool push_synthetic(diagnostic_list *out, const char *file, diagnostic_severity severity,
                           const char *message) {
    diagnostic item;
    memset(&item, 0, sizeof item);
    item.severity = severity;
    snprintf(item.file, sizeof item.file, "%s", file);
    snprintf(item.message, sizeof item.message, "%s", message);
    return diagnostic_list_push(out, &item);
}

/* Fold one finished task into the report, on the main thread.

   Keeping one rule for the exit code is the point of the synthetic errors: a
   tool that fails without explaining itself still fails the lint, because it
   produced an error diagnostic like any other. */
static int absorb(lint_task *task, diagnostic_list *out) {
    if(task->status < 0 || task->status == 127) {
        fprintf(stderr, "molto: could not run '%s'\n", task->tool);
        return exit_build_failure;
    }

    size_t errors = diagnostic_count_severity(&task->found, diagnostic_severity_error);
    char message[512];
    if(task->status > 128) {
        snprintf(message, sizeof message,
                 "%s was killed by signal %d while "
                 "checking this file",
                 task->tool, task->status - 128);
        if(!push_synthetic(out, task->source, diagnostic_severity_error, message))
            return exit_build_failure;
    } else if(task->status != 0 && errors == 0) {
        snprintf(message, sizeof message,
                 "%s exited %d with nothing to say about "
                 "this file",
                 task->tool, task->status);
        if(!push_synthetic(out, task->source, diagnostic_severity_error, message))
            return exit_build_failure;
    }

    if(task->truncated) {
        snprintf(message, sizeof message,
                 "diagnostics for this file were "
                 "truncated at %d bytes",
                 LINT_OUTPUT_SIZE);
        if(!push_synthetic(out, task->source, diagnostic_severity_note, message))
            return exit_build_failure;
    }

    return diagnostic_list_append(out, &task->found) ? exit_ok : exit_build_failure;
}

/* Resolve everything the passes need, and translate the configuration. */
static int prepare(const char *root, const lint_request *request, lint_setup *setup,
                   const str_list *sources) {
    char err[CONFIG_ERROR_SIZE] = "";

    /* The database is opened only to reuse the recorded answers about the
       compiler and the linter, so lint does not pay pickup's cost every run. */
    wsdb *db = wsdb_open(root);
    int code = toolchain_resolve(&setup->ctx.target, needs_cpp(sources), db,
                                 request->refresh_toolchain, &setup->chain);
    if(code != exit_ok) {
        wsdb_close(db);
        return code;
    }

    /* No linter is not an error: the compiler pass is what RFC-0005 promises
       without anything installed, and degrading to it beats refusing to run. */
    setup->has_linter =
        tool_resolve(tool_kind_linter, db, request->refresh_tools, &setup->linter) == exit_ok;
    wsdb_close(db);

    if(setup->has_linter &&
       !style_translate_lint(root, &setup->config, &setup->linter, setup->linter_config,
                             sizeof setup->linter_config, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_invalid_manifest;
    }
    return exit_ok;
}

/* Compose every pass to run: the compiler over each source, then the linter. */
static bool build_tasks(const char *root, const lint_setup *setup, build_profile profile,
                        const str_list *sources, lint_task *tasks, str_list *argvs, size_t *count) {
    size_t at = 0;
    for(size_t i = 0; i < str_list_count(sources); i++) {
        const char *source = str_list_get(sources, i);

        str_list_init(&argvs[at]);
        if(!build_compiler_argv(&argvs[at], root, setup, profile, source))
            return false;
        tasks[at] = (lint_task){
            .source = source,
            .tool = setup->chain.cc,
            .argv = &argvs[at],
            .env = &setup->ctx.env,
        };
        diagnostic_list_init(&tasks[at].found);
        at++;

        if(!setup->has_linter)
            continue;

        str_list_init(&argvs[at]);
        if(!build_linter_argv(&argvs[at], root, setup, profile, source))
            return false;
        tasks[at] = (lint_task){
            .source = source,
            .tool = setup->linter.path,
            .argv = &argvs[at],
            .env = &setup->ctx.env,
        };
        diagnostic_list_init(&tasks[at].found);
        at++;
    }
    *count = at;
    return true;
}

int lint_project(const char *root, const lint_request *request, diagnostic_list *out) {
    lint_setup setup;
    memset(&setup, 0, sizeof setup);

    int code = load_manifest(root, &setup.ctx);
    if(code != exit_ok)
        return code;

    /* The configuration comes first because it decides which sources there are
       to analyze, and the sources decide what is asked of the toolchain. */
    char err[CONFIG_ERROR_SIZE] = "";
    if(!lint_config_load(root, &setup.config, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_invalid_manifest;
    }

    str_list sources;
    str_list_init(&sources);
    if(!collect_sources(root, &setup.config.paths, &sources)) {
        str_list_free(&sources);
        return exit_build_failure;
    }
    size_t source_count = str_list_count(&sources);
    if(source_count == 0) {
        str_list_free(&sources);
        return exit_ok;
    }

    code = prepare(root, request, &setup, &sources);
    if(code != exit_ok) {
        str_list_free(&sources);
        return code;
    }

    /* One pass per source, plus one more each when there is a linter. */
    size_t capacity = source_count * (setup.has_linter ? 2 : 1);
    lint_task *tasks = calloc(capacity, sizeof *tasks);
    str_list *argvs = calloc(capacity, sizeof *argvs);
    if(tasks == NULL || argvs == NULL) {
        free(tasks);
        free(argvs);
        str_list_free(&sources);
        return exit_build_failure;
    }

    size_t count = 0;
    bool ok = build_tasks(root, &setup, request->profile, &sources, tasks, argvs, &count);
    if(ok) {
        task_pool *pool = task_pool_create(0);
        if(pool == NULL) {
            ok = false;
        } else {
            for(size_t i = 0; i < count; i++) {
                if(!task_pool_submit(pool, lint_task_run, &tasks[i]))
                    ok = false;
            }
            task_pool_wait(pool);
            task_pool_destroy(pool);
        }
    }

    /* Fold in source order. The pool finishes in whatever order it likes;
       walking the tasks as they were composed is what makes two runs over one
       tree report the same thing in the same order. */
    code = ok ? exit_ok : exit_build_failure;
    for(size_t i = 0; code == exit_ok && i < count; i++)
        code = absorb(&tasks[i], out);

    for(size_t i = 0; i < capacity; i++) {
        diagnostic_list_free(&tasks[i].found);
        str_list_free(&argvs[i]);
    }
    free(tasks);
    free(argvs);
    str_list_free(&sources);
    return code;
}
