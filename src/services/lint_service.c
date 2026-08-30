#include <molto/services/lint_service.h>

#include <molto/build/compile_flags.h>
#include <molto/build/depfile.h>
#include <molto/exit_code.h>
#include <molto/project/project_ctx.h>
#include <molto/project/style_config.h>
#include <molto/services/build_service.h>
#include <molto/services/deps_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/host_service.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/services/style_translate.h>
#include <molto/services/tool_service.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/loader.h>
#include <molto/util/task_pool.h>
#include <molto/workspace/wsdb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* On-disk layout this command reads. */
#define MANIFEST_FILENAME "Project.toml"
#define DIR_SRC "src"
/* Where the dependency lists of the analysis passes are kept: Molto-owned
   metadata, beside the database that refers to them. */
#define DIR_BIN ".bin"
#define DIR_LINT "lint"
#define EXT_DEPFILE ".d"

/* What a recorded result is stored under, and what joins the parts of a
   fingerprint. A byte no argument can contain keeps two different commands
   from composing into one string. */
#define KEY_PREFIX "lint:"
#define FINGERPRINT_SEPARATOR "\x1f"

/* Compiler arguments for a pass that checks and produces nothing. */
#define ARG_SYNTAX_ONLY "-fsyntax-only"
#define ARG_NO_COLOR "-fdiagnostics-color=never"
#define ARG_INCLUDE_SRC "-I%s/" DIR_SRC
/* Written by the compiler pass so the cache knows which headers it read. */
#define ARG_DEPFILE "-MMD"
#define ARG_DEPFILE_TO "-MF"

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

/* What the spinner says it is doing while the pool runs. Not "linting": the
   compiler pass runs whether or not a linter was found, and what both passes
   have in common is that they read the code and produce nothing. */
#define LINT_LOADER_LABEL "analyzing ..."

/* One pass over one file. The worker runs the tool and parses what it said;
   what a failure means is decided on the main thread, where the order of the
   diagnostics is still the order of the sources. */
typedef struct {
    const char *source; /* absolute, borrowed from the sorted list */
    const char *tool;   /* what to blame when the run itself fails */
    const str_list *argv;
    const project_env *env;
    /* How this pass counts the columns it reports, which the parser cannot
       tell and the caret needs: the compiler answers for its own vendor, and
       clang-tidy counts bytes whichever compiler the project builds with. */
    diagnostic_column_unit columns;
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
    /* The translated configuration itself, not its path, because the path does
       not change when linter.json does. It goes into every fingerprint. */
    char *linter_config_text;
    /* What the project depends on, reduced to what a command line needs.
     *
     * Analysis without them is analysis of different code: a dependency's
     * headers are what its `#include` resolves to, and its defines are what a
     * `#ifdef` decides on. Lint used to have neither, so a project with any
     * dependency at all was told its own sources could not find their headers —
     * a diagnostic that blames the user for a file Molto never looked for. */
    prepared_deps deps;
    str_list include_flags;
} lint_setup;

/* One source and everything the cache needs to decide about it (RFC-0006).
   The unit is the file rather than the pass: the prerequisites come from the
   depfile the compiler pass writes, so answering "is the linter pass still
   valid" means running the compiler pass anyway. Recording them together costs
   a syntax-only run when only linter.json changed, and buys an entry that
   cannot disagree with itself about which headers it watched. */
typedef struct {
    const char *source;
    char key[PATH_BUFFER_SIZE];
    char depfile[PATH_BUFFER_SIZE];
    char *fingerprint;  /* heap: the argvs are unbounded */
    bool replayed;      /* answered from the store; no process ran */
    uint64_t signature; /* what the source was when its analysis began */
    diagnostic_list found;
    size_t first_task; /* index into the task array, when it ran */
    size_t task_count;
} lint_file;

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
    /* The standard goes last, which is not cosmetic: it is where the build puts
       it now that a compile line is composed from the document, where `-std`
       is a unit-scope option and unit scope reaches the line after the rest
       (RFC-0013). A compiler takes the last of two contradictory flags, so a
       `-std=` written by hand into `[target].flags` loses to `[target].std` —
       and lint composing it first would analyse the file as the other language.
       This function's own comment is the rule being kept: lint and build must
       not disagree about what the code says. */
    return compile_flags_push_options(argv, root, &setup->ctx.target.options) &&
           compile_flags_push_options(argv, root, profile_options_for(&setup->ctx, profile)) &&
           compile_flags_push_std(argv, &setup->ctx.target, is_cpp);
}

/* The project's own `src/` and then one per include a dependency exports, in
   the order the build composes them — last on the line, exactly as a compile
   line carries them, so lint and build resolve an `#include` the same way. */
static bool push_include_flags(str_list *argv, const lint_setup *setup) {
    for(size_t i = 0; i < str_list_count(&setup->include_flags); i++) {
        if(!str_list_push(argv, str_list_get(&setup->include_flags, i)))
            return false;
    }
    return true;
}

/* The syntax-only pass. The preset's warnings go before the project's own
   flags: in gcc and clang the last one wins, so a preset -Wall appended after a
   project's -Wno-unused would silently re-enable what it deliberately turned
   off. */
static bool build_compiler_argv(str_list *argv, const char *root, const lint_setup *setup,
                                build_profile profile, const char *source, const char *depfile) {
    bool is_cpp = source_is_cpp(source);
    const char *driver = compile_flags_driver(&setup->chain, is_cpp);
    if(driver == NULL) {
        fprintf(stderr, "molto: '%s' needs a C++ compiler and none was resolved\n", source);
        return false;
    }

    bool ok = str_list_push(argv, driver) && str_list_push(argv, ARG_SYNTAX_ONLY) &&
              str_list_push(argv, ARG_NO_COLOR) && str_list_push(argv, source);
    /* -MMD during a syntax-only pass costs nothing and is what tells the cache
       which headers this file read. */
    if(ok && depfile != NULL)
        ok = str_list_push(argv, ARG_DEPFILE) && str_list_push(argv, ARG_DEPFILE_TO) &&
             str_list_push(argv, depfile);
    if(ok && setup->config.preset == style_preset_molto) {
        for(size_t i = 0; ok && i < PRESET_MOLTO_WARNINGS_COUNT; i++)
            ok = str_list_push(argv, preset_molto_warnings[i]);
    }
    if(ok)
        ok = push_compile_arguments(argv, root, setup, profile, is_cpp);
    return ok && push_include_flags(argv, setup);
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
    return ok && push_include_flags(argv, setup);
}

/* --- the result cache (RFC-0006) --- */

/* Where the compiler pass is told to write its dependency list: under the
   directory Molto owns, mirroring the source path, so two sources of the same
   name in different directories do not share one. */
[[nodiscard]] static bool depfile_path_for(const char *root, const char *source, char *out,
                                           size_t out_size) {
    const char *relative = fs_relative_to(source, root);
    if(!fs_format_path(out, out_size, "%s/" DIR_BIN "/" DIR_LINT "/%s" EXT_DEPFILE, root, relative))
        return fs_report_long_path(source);

    char directory[PATH_BUFFER_SIZE];
    snprintf(directory, sizeof directory, "%s", out);
    char *slash = strrchr(directory, '/');
    if(slash == NULL)
        return false;
    *slash = '\0';
    return fs_make_dirs(directory);
}

/* The key an entry is stored under. Relative, so a workspace stays valid when
   the tree is moved. */
[[nodiscard]] static bool result_key_for(const char *root, const char *source, char *out,
                                         size_t out_size) {
    return fs_format_path(out, out_size, "%s%s", KEY_PREFIX, fs_relative_to(source, root)) ||
           fs_report_long_path(source);
}

/* Everything the recorded diagnostics depended on, flattened into one string.
   The argvs carry the flags, the defines, the includes and therefore the
   profile; the versions carry the tools; and the translated configuration
   carries linter.json, whose path alone would not change when it does. */
static char *join_parts(const str_list *parts) {
    size_t total = 1;
    for(size_t i = 0; i < str_list_count(parts); i++)
        total += strlen(str_list_get(parts, i)) + 1;

    char *out = malloc(total);
    if(out == NULL)
        return NULL;
    out[0] = '\0';
    size_t used = 0;
    for(size_t i = 0; i < str_list_count(parts); i++) {
        const char *part = str_list_get(parts, i);
        int written = snprintf(out + used, total - used, "%s%s", part, FINGERPRINT_SEPARATOR);
        if(written < 0 || (size_t)written >= total - used) {
            free(out);
            return NULL;
        }
        used += (size_t)written;
    }
    return out;
}

static char *fingerprint_for(const lint_setup *setup, const str_list *compiler_argv,
                             const str_list *linter_argv) {
    str_list parts;
    str_list_init(&parts);
    bool ok = str_list_push(&parts, setup->chain.version);
    for(size_t i = 0; ok && i < str_list_count(compiler_argv); i++)
        ok = str_list_push(&parts, str_list_get(compiler_argv, i));
    if(ok && linter_argv != NULL) {
        ok = str_list_push(&parts, setup->linter.version) &&
             str_list_push(&parts,
                           setup->linter_config_text != NULL ? setup->linter_config_text : "");
        for(size_t i = 0; ok && i < str_list_count(linter_argv); i++)
            ok = str_list_push(&parts, str_list_get(linter_argv, i));
    }
    /* The tools run in the project's [env], so a diagnostic recorded under one
       environment does not answer for another. Pushed only when there is one,
       which leaves the fingerprint of a project without [env] exactly as it was
       and its recorded analyses still valid. */
    char environment[PROJECT_ENV_FINGERPRINT_MAX];
    if(ok && project_env_fingerprint(&setup->ctx.env, environment, sizeof environment) > 0)
        ok = str_list_push(&parts, environment);

    char *joined = ok ? join_parts(&parts) : NULL;
    str_list_free(&parts);
    return joined;
}

/* Replay what was recorded, if it is still valid. */
static bool replay(wsdb *db, lint_file *file) {
    if(db == NULL || file->fingerprint == NULL)
        return false;
    if(!wsdb_result_fresh(db, file->key, file->fingerprint))
        return false;

    str_list values;
    str_list_init(&values);
    bool ok = wsdb_result_values(db, file->key, &values) &&
              diagnostic_list_from_values(&values, &file->found);
    str_list_free(&values);
    if(!ok) {
        /* A stored entry that cannot be read is not a reason to fail; it is a
           reason to analyse the file again and overwrite it. */
        diagnostic_list_free(&file->found);
        diagnostic_list_init(&file->found);
    }
    return ok;
}

/* Record what a file produced, watching the source and the headers the
   compiler pass reported. A failure here costs the next run its cache and
   nothing else, so it is reported to the caller and never fatal. */
static bool record(wsdb *db, const lint_file *file) {
    str_list prereqs;
    str_list_init(&prereqs);
    bool ok = depfile_read(file->depfile, &prereqs) && str_list_count(&prereqs) > 0;

    str_list values;
    str_list_init(&values);
    ok = ok && diagnostic_list_to_values(&file->found, &values) &&
         wsdb_record_result(db, file->key, file->fingerprint, &prereqs, &values);

    str_list_free(&values);
    str_list_free(&prereqs);
    return ok;
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
    diagnostic_list_set_columns(&task->found, task->columns);

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
/* Resolve the dependencies and fold what they export into `[target]`, exactly
   as a build does and through the same two functions — one implementation, so
   lint and build cannot come to disagree about what a source is compiled with.
 *
 * It fetches on a cold cache, which `molto lint` did not do before. That is not
 * a cost being added for tidiness: the headers have to be on disk to be read,
 * and analysis that skipped them was reporting on code that does not exist. */
/* The include directories of the libraries the host provides (RFC-0016).
 *
 * The build asks this question and lint has to ask it too, or a source that
 * includes <gtk/gtk.h> is analysed as a file whose headers do not exist. That
 * is not a finding about the code — it is lint being unable to read it, and it
 * reports as an error on every file in the project.
 *
 * `-isystem` rather than `-I`, which is the same distinction the build draws
 * when it marks these paths `system`: a toolkit's headers are not this
 * project's code, and analysing them buries whatever lint had to say about the
 * code that is. */
static bool host_include_flags(const project_ctx *ctx, str_list *out) {
    char err[CONFIG_ERROR_SIZE] = "";
    for(size_t i = 0; i < ctx->target.host_count; i++) {
        host_answer answer;
        if(!host_resolve(ctx->target.host[i], &answer, err, sizeof err)) {
            fprintf(stderr, "molto: %s\n", err);
            return false;
        }
        for(size_t j = 0; j < answer.include_count; j++) {
            if(!str_list_push(out, "-isystem") || !str_list_push(out, answer.includes[j]))
                return false;
        }
    }
    return true;
}

static bool prepare_deps(const char *root, lint_setup *setup) {
    char err[CONFIG_ERROR_SIZE] = "";
    if(!deps_prepare(&setup->ctx, &setup->deps, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return false;
    }
    char src_dir[PATH_BUFFER_SIZE];
    return fs_format_path(src_dir, sizeof src_dir, "%s/" DIR_SRC, root) &&
           deps_merge_interface(&setup->ctx, &setup->deps) &&
           deps_include_flags(src_dir, &setup->deps, &setup->include_flags) &&
           host_include_flags(&setup->ctx, &setup->include_flags);
}

static int prepare(const char *root, const lint_request *request, lint_setup *setup,
                   const str_list *sources) {
    char err[CONFIG_ERROR_SIZE] = "";

    if(!prepare_deps(root, setup))
        return exit_dependency_failure;

    /* The database is opened only to reuse the recorded answers about the
       compiler and the linter, so lint does not pay pickup's cost every run. */
    wsdb *db = wsdb_open(root);
    int code = toolchain_resolve(&setup->ctx.target, NULL, needs_cpp(sources), db,
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
    /* Read once, to fingerprint every file with it. Failing to read it is not
       fatal: it costs the cache, since a fingerprint without it cannot notice
       that linter.json changed, and the empty string is never mistaken for a
       configuration that was read. */
    if(setup->has_linter)
        setup->linter_config_text = fs_read_file(setup->linter_config);
    return exit_ok;
}

/* Compose the passes for every source, skipping the files the store can already
   answer for. A file that is replayed contributes no task at all, which is the
   whole point: the work avoided is the process that would have run. */
static bool build_tasks(const char *root, const lint_setup *setup, build_profile profile,
                        const str_list *sources, wsdb *db, lint_file *files, lint_task *tasks,
                        str_list *argvs, size_t *count) {
    size_t at = 0;
    for(size_t i = 0; i < str_list_count(sources); i++) {
        lint_file *file = &files[i];
        file->source = str_list_get(sources, i);
        diagnostic_list_init(&file->found);

        if(!result_key_for(root, file->source, file->key, sizeof file->key) ||
           !depfile_path_for(root, file->source, file->depfile, sizeof file->depfile))
            return false;

        size_t compiler_at = at;
        str_list_init(&argvs[at]);
        if(!build_compiler_argv(&argvs[at], root, setup, profile, file->source, file->depfile))
            return false;
        at++;

        size_t linter_at = 0;
        if(setup->has_linter) {
            linter_at = at;
            str_list_init(&argvs[at]);
            if(!build_linter_argv(&argvs[at], root, setup, profile, file->source))
                return false;
            at++;
        }

        file->fingerprint = fingerprint_for(setup, &argvs[compiler_at],
                                            setup->has_linter ? &argvs[linter_at] : NULL);
        file->replayed = replay(db, file);
        if(file->replayed) {
            /* Give back the slots: the argvs were only needed to know what the
               run would have been, which is exactly what the fingerprint is. */
            at = compiler_at;
            str_list_free(&argvs[compiler_at]);
            str_list_init(&argvs[compiler_at]);
            if(setup->has_linter) {
                str_list_free(&argvs[linter_at]);
                str_list_init(&argvs[linter_at]);
            }
            continue;
        }

        file->signature = fs_signature(file->source);
        file->first_task = compiler_at;
        file->task_count = setup->has_linter ? 2 : 1;
        tasks[compiler_at] = (lint_task){
            .source = file->source,
            .tool = setup->chain.cc,
            .argv = &argvs[compiler_at],
            .env = &setup->ctx.env,
            .columns = diagnostic_columns_of_vendor(setup->chain.vendor),
        };
        diagnostic_list_init(&tasks[compiler_at].found);
        if(setup->has_linter) {
            tasks[linter_at] = (lint_task){
                .source = file->source,
                .tool = setup->linter.path,
                .argv = &argvs[linter_at],
                .env = &setup->ctx.env,
                .columns = diagnostic_columns_byte,
            };
            diagnostic_list_init(&tasks[linter_at].found);
        }
    }
    *count = at;
    return true;
}

/* Whether what a pass produced may be recorded. A tool that crashed, exited
   without explaining itself, or whose output was cut off did not produce a
   result: storing one would replay a failure that a second attempt might not
   have, and would never retry it. */
static bool task_is_recordable(const lint_task *task) {
    if(task->truncated || task->status < 0 || task->status == 127 || task->status > 128)
        return false;
    return task->status == 0 ||
           diagnostic_count_severity(&task->found, diagnostic_severity_error) > 0;
}

/* Everything the setup owns. One place, because this function has six ways out
   and a field released on five of them is a leak nobody sees. */
static void setup_free(lint_setup *setup) {
    free(setup->linter_config_text);
    setup->linter_config_text = NULL;
    prepared_deps_free(&setup->deps);
    str_list_free(&setup->include_flags);
}

int lint_project(const char *root, const lint_request *request, diagnostic_list *out) {
    lint_setup setup;
    memset(&setup, 0, sizeof setup);
    prepared_deps_init(&setup.deps);
    str_list_init(&setup.include_flags);

    int code = load_manifest(root, &setup.ctx);
    if(code != exit_ok) {
        setup_free(&setup);
        return code;
    }

    /* The configuration comes first because it decides which sources there are
       to analyze, and the sources decide what is asked of the toolchain. */
    char err[CONFIG_ERROR_SIZE] = "";
    if(!lint_config_load(root, &setup.config, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        setup_free(&setup);
        return exit_invalid_manifest;
    }

    str_list sources;
    str_list_init(&sources);
    if(!collect_sources(root, &setup.config.paths, &sources)) {
        str_list_free(&sources);
        setup_free(&setup);
        return exit_build_failure;
    }
    size_t source_count = str_list_count(&sources);
    if(source_count == 0) {
        str_list_free(&sources);
        setup_free(&setup);
        return exit_ok;
    }

    code = prepare(root, request, &setup, &sources);
    if(code != exit_ok) {
        str_list_free(&sources);
        setup_free(&setup);
        return code;
    }

    /* One pass per source, plus one more each when there is a linter. */
    size_t capacity = source_count * (setup.has_linter ? 2 : 1);
    lint_task *tasks = calloc(capacity, sizeof *tasks);
    str_list *argvs = calloc(capacity, sizeof *argvs);
    lint_file *files = calloc(source_count, sizeof *files);
    if(tasks == NULL || argvs == NULL || files == NULL) {
        free(tasks);
        free(argvs);
        free(files);
        setup_free(&setup);
        str_list_free(&sources);
        return exit_build_failure;
    }

    /* --refresh-analysis is expressed as having no store to consult: the run
       proceeds exactly as a cold one, and records over what was there. */
    wsdb *db = request->refresh_analysis ? NULL : wsdb_open(root);

    size_t count = 0;
    bool ok =
        build_tasks(root, &setup, request->profile, &sources, db, files, tasks, argvs, &count);
    if(ok && count > 0) {
        /* The spinner turns for exactly as long as the pool does, because this
           is the only stretch of the command in which nothing else writes to
           stderr: the workers capture what their processes say into buffers of
           their own, and everything lint reports — the errors below, the
           diagnostics, the tally — is said afterwards on this thread. That is
           what lets the loader own the stream without a lock. */
        loader *spinner = request->progress ? loader_start(stderr, LINT_LOADER_LABEL) : NULL;
        task_pool *pool = task_pool_create(request->jobs);
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
        loader_stop(spinner);
    }

    /* Fold in source order. The pool finishes in whatever order it likes;
       walking the files as they were composed is what makes two runs over one
       tree report the same thing in the same order — and what makes a replayed
       run indistinguishable from the one that produced it. */
    code = ok ? exit_ok : exit_build_failure;
    for(size_t i = 0; code == exit_ok && i < source_count; i++) {
        lint_file *file = &files[i];
        if(!file->replayed) {
            bool recordable = true;
            for(size_t t = 0; code == exit_ok && t < file->task_count; t++) {
                lint_task *task = &tasks[file->first_task + t];
                recordable = recordable && task_is_recordable(task);
                code = absorb(task, &file->found);
            }
            /* A file that cannot be recorded — no depfile because the compiler
               does not write one, or a store that refused it — is simply
               analysed again next time. It is not reported: the result is
               unaffected, only the time it took, and one line per file would
               be noise proportional to the project. */
            /* If the source changed while it was being analysed, what the
               tools said is about content that is no longer there. Recording
               it would pin stale diagnostics under a current signature, and
               nothing would ever invalidate them. */
            if(code == exit_ok && db != NULL && recordable && file->fingerprint != NULL &&
               fs_signature(file->source) == file->signature)
                (void)record(db, file);
        }
        if(code == exit_ok && !diagnostic_list_append(out, &file->found))
            code = exit_build_failure;
    }

    if(db != NULL && !wsdb_close(db))
        fprintf(stderr, "molto: could not save the workspace state; "
                        "the next lint will not be incremental\n");

    for(size_t i = 0; i < capacity; i++) {
        diagnostic_list_free(&tasks[i].found);
        str_list_free(&argvs[i]);
    }
    for(size_t i = 0; i < source_count; i++) {
        diagnostic_list_free(&files[i].found);
        free(files[i].fingerprint);
    }
    free(tasks);
    free(argvs);
    free(files);
    setup_free(&setup);
    str_list_free(&sources);
    return code;
}
