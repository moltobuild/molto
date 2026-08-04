#include <molto/services/fmt_service.h>

#include <molto/build/diff.h>
#include <molto/exit_code.h>
#include <molto/project/project_ctx.h>
#include <molto/project/style_config.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/services/style_translate.h>
#include <molto/services/tool_service.h>
#include <molto/util/task_pool.h>
#include <molto/workspace/wsdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where a project keeps the code a style tool has an opinion about. */
#define DIR_SRC "src"
#define DIR_INCLUDE "include"

/* The manifest, read here only for the language its headers are written in. */
#define MANIFEST_FILENAME "Project.toml"

/* clang-format arguments. */
#define ARG_STYLE_FILE "--style=file:%s" /* a config outside the project tree */
#define ARG_IN_PLACE "-i"
#define ARG_DRY_RUN "--dry-run"
#define ARG_WERROR "--Werror" /* makes --dry-run exit non-zero on a change */

/* Size of the stack buffers used to compose paths. */
#define PATH_BUFFER_SIZE 4096

/* Size of the buffer receiving what the formatter says about one file. */
#define MESSAGE_SIZE 65536

/* Room a formatted file may need beyond its original size, plus a floor for a
   file that starts out empty. */
#define FORMATTED_GROWTH 2
#define FORMATTED_FLOOR 4096

/* Size of the buffer receiving a configuration error. */
#define CONFIG_ERROR_SIZE 512

/* One file's formatting. The worker runs the tool and reads what it produced;
   what that means is decided on the main thread, where the order of the files
   is still the order they were discovered in. */
typedef struct {
    const char *path; /* absolute, borrowed from the discovered list */
    const str_list *argv;
    fmt_mode mode;
    int status;
    bool truncated;
    char *original;  /* what the file was, in diff and write modes; owned */
    char *formatted; /* what it became: captured in diff, read back in write */
    char message[MESSAGE_SIZE];
} fmt_task;

void fmt_result_init(fmt_result *result) {
    diagnostic_list_init(&result->diagnostics);
    str_list_init(&result->changed);
}

void fmt_result_free(fmt_result *result) {
    diagnostic_list_free(&result->diagnostics);
    str_list_free(&result->changed);
}

/* Collect what the formatter should look at: the sources and the headers of
   the project, minus what format.json excludes. */
static bool collect_files(const char *root, const style_excludes *excludes, str_list *out) {
    static const char *const directories[] = {DIR_SRC, DIR_INCLUDE};
    str_list found;
    str_list_init(&found);

    bool ok = true;
    for(size_t i = 0; ok && i < sizeof directories / sizeof directories[0]; i++) {
        char path[PATH_BUFFER_SIZE];
        if(!fs_format_path(path, sizeof path, "%s/%s", root, directories[i]))
            ok = fs_report_long_path(directories[i]);
        else if(fs_is_dir(path))
            ok = source_discovery_collect_styleable(path, &found);
    }

    for(size_t i = 0; ok && i < str_list_count(&found); i++) {
        const char *path = str_list_get(&found, i);
        /* Patterns are written the way a project is read, so they are matched
           against the relative path and not the absolute one. */
        if(!style_excludes_match(excludes, fs_relative_to(path, root)))
            ok = str_list_push(out, path);
    }

    str_list_free(&found);
    return ok;
}

/* Compose the command for one file. Only the mode differs: the configuration
   and the backend are the same for all of them. */
static bool build_argv(str_list *argv, const resolved_tool *backend, const char *config_path,
                       const char *file, fmt_mode mode) {
    char style[PATH_BUFFER_SIZE];
    if(!fs_format_path(style, sizeof style, ARG_STYLE_FILE, config_path))
        return fs_report_long_path(config_path);

    bool ok = str_list_push(argv, backend->path) && str_list_push(argv, style);
    if(ok && mode == fmt_mode_write)
        ok = str_list_push(argv, ARG_IN_PLACE);
    if(ok && mode == fmt_mode_check)
        ok = str_list_push(argv, ARG_DRY_RUN) && str_list_push(argv, ARG_WERROR);
    return ok && str_list_push(argv, file);
}

/* Run the formatter for one file. In diff mode its stdout is the formatted
   file, so only stderr may be mixed in elsewhere. */
static void fmt_task_run(void *argument) {
    fmt_task *task = argument;
    task->message[0] = '\0';

    const char **argv = process_argv_from_list(task->argv);
    if(argv == NULL) {
        task->status = -1;
        return;
    }

    if(task->mode == fmt_mode_diff) {
        size_t size = strlen(task->original) * FORMATTED_GROWTH + FORMATTED_FLOOR;
        task->formatted = malloc(size);
        if(task->formatted == NULL) {
            task->status = -1;
            free((void *)argv);
            return;
        }
        /* Only stdout: mixing stderr in would corrupt the formatted text. */
        task->status = process_capture(argv, task->formatted, size);
    } else {
        task->status = process_capture_all(argv, NULL, 0, task->message, sizeof task->message,
                                           &task->truncated);
        /* --in-place rewrote the file and exits zero whether or not it had to,
           so the file itself is the only record of what happened. Read it back
           here, on the worker, where the reading is already parallel. */
        if(task->mode == fmt_mode_write && task->status == 0)
            task->formatted = fs_read_file(task->path);
    }

    free((void *)argv);
}

/* Note that a file changed, or would have. */
static bool record_change(fmt_result *result, const char *path) {
    return str_list_push(&result->changed, path);
}

/* Turn a failed run into a diagnostic, so a tool that fails silently still
   fails the command through the same single rule as everything else. */
static bool report_failure(fmt_result *result, const fmt_task *task, const char *backend_path) {
    diagnostic item;
    memset(&item, 0, sizeof item);
    item.severity = diagnostic_severity_error;
    snprintf(item.file, sizeof item.file, "%s", task->path);
    if(task->status > 128)
        snprintf(item.message, sizeof item.message,
                 "%s was killed by signal %d while formatting this file", backend_path,
                 task->status - 128);
    else
        snprintf(item.message, sizeof item.message,
                 "%s exited %d with nothing to say about this file", backend_path, task->status);
    return diagnostic_list_push(&result->diagnostics, &item);
}

/* Fold one finished task into the result, on the main thread. */
static int absorb(fmt_task *task, const fmt_request *request, fmt_result *result,
                  const char *backend_path, const char *root) {
    if(task->status < 0 || task->status == 127) {
        fprintf(stderr, "molto: could not run '%s'\n", backend_path);
        return exit_build_failure;
    }

    if(request->mode == fmt_mode_diff) {
        bool changed = false;
        if(!diff_unified(task->original, task->formatted, fs_relative_to(task->path, root),
                         DIFF_CONTEXT_LINES, request->diff_stream, &changed))
            return exit_build_failure;
        if(changed && !record_change(result, task->path))
            return exit_build_failure;
        return exit_ok;
    }

    size_t before = diagnostic_list_count(&result->diagnostics);
    if(!diagnostic_parse(task->message, &result->diagnostics))
        return exit_build_failure;
    bool said_something = diagnostic_list_count(&result->diagnostics) > before;

    if(request->mode == fmt_mode_check) {
        /* --dry-run --Werror exits non-zero precisely when the file would
           change, which is what --check reports on. */
        if(task->status != 0 && !record_change(result, task->path))
            return exit_build_failure;
    } else if(task->status == 0) {
        /* Write mode: the exit status says nothing, so the file is compared
           against what it was. Failing to read back what was just written is a
           real failure and not something to count as unchanged. */
        if(task->formatted == NULL) {
            fprintf(stderr, "molto: could not read back '%s'\n", task->path);
            return exit_build_failure;
        }
        if(strcmp(task->original, task->formatted) != 0 && !record_change(result, task->path))
            return exit_build_failure;
    }

    if(task->status != 0 && !said_something && !report_failure(result, task, backend_path))
        return exit_build_failure;
    return exit_ok;
}

/* Read what the files were, before anything runs and rewrites them. */
static bool read_originals(fmt_task *tasks, size_t count) {
    for(size_t i = 0; i < count; i++) {
        tasks[i].original = fs_read_file(tasks[i].path);
        if(tasks[i].original == NULL) {
            fprintf(stderr, "molto: could not read '%s'\n", tasks[i].path);
            return false;
        }
    }
    return true;
}

static void free_tasks(fmt_task *tasks, size_t count, str_list *argvs) {
    for(size_t i = 0; i < count; i++) {
        free(tasks[i].original);
        free(tasks[i].formatted);
        str_list_free(&argvs[i]);
    }
    free(tasks);
    free(argvs);
}

/* The `[target].cpp_std` of the manifest, or "" when the project is C or the
   manifest cannot be read. Formatting is not a build and must not start failing
   over a manifest that a build would reject anyway: a project that cannot be
   parsed is treated as C, which is the assumption that was already in force. */
static void read_cpp_std(const char *root, char *out, size_t out_size) {
    out[0] = '\0';

    char path[PATH_BUFFER_SIZE];
    if(!fs_format_path(path, sizeof path, "%s/" MANIFEST_FILENAME, root))
        return;

    project_ctx ctx;
    char err[CONFIG_ERROR_SIZE] = "";
    if(project_load(path, &ctx, err, sizeof err))
        snprintf(out, out_size, "%s", ctx.target.cpp_std);
}

int fmt_project(const char *root, const fmt_request *request, fmt_result *result) {
    char err[CONFIG_ERROR_SIZE] = "";
    style_config config;
    if(!style_config_load(root, &config, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_invalid_manifest;
    }

    char cpp_std[16];
    read_cpp_std(root, cpp_std, sizeof cpp_std);

    /* The database is opened only to reuse the recorded answer about the tool,
       so `molto fmt` does not pay pickup's cost on every run. */
    wsdb *db = wsdb_open(root);
    resolved_tool backend;
    int code = tool_resolve(tool_kind_formatter, db, request->refresh_tools, &backend);
    if(code != exit_ok) {
        if(code == exit_dependency_failure)
            fprintf(stderr, "molto: this machine has no formatter "
                            "(try: pickup install clang-format)\n");
        wsdb_close(db);
        return code;
    }

    char config_path[STYLE_CONFIG_PATH_MAX];
    if(!style_translate_format(root, &config, &backend, cpp_std, config_path, sizeof config_path,
                               err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        wsdb_close(db);
        return exit_invalid_manifest;
    }
    wsdb_close(db);

    str_list files;
    str_list_init(&files);
    if(!collect_files(root, &config.paths, &files)) {
        str_list_free(&files);
        return exit_build_failure;
    }
    size_t count = str_list_count(&files);
    if(count == 0) {
        str_list_free(&files);
        return exit_ok;
    }

    fmt_task *tasks = calloc(count, sizeof *tasks);
    str_list *argvs = calloc(count, sizeof *argvs);
    if(tasks == NULL || argvs == NULL) {
        free(tasks);
        free(argvs);
        str_list_free(&files);
        return exit_build_failure;
    }

    bool ok = true;
    for(size_t i = 0; ok && i < count; i++) {
        str_list_init(&argvs[i]);
        tasks[i].path = str_list_get(&files, i);
        tasks[i].argv = &argvs[i];
        tasks[i].mode = request->mode;
        ok = build_argv(&argvs[i], &backend, config_path, tasks[i].path, request->mode);
    }
    /* Both modes that report on content need what the content was: diff prints
       the difference, write counts the files that had one. */
    if(ok && request->mode != fmt_mode_check)
        ok = read_originals(tasks, count);

    if(ok) {
        task_pool *pool = task_pool_create(0);
        if(pool == NULL) {
            ok = false;
        } else {
            for(size_t i = 0; i < count; i++) {
                if(!task_pool_submit(pool, fmt_task_run, &tasks[i]))
                    ok = false;
            }
            task_pool_wait(pool);
            task_pool_destroy(pool);
        }
    }

    /* Fold in discovery order, so two runs over one tree report the same thing
       in the same order however the pool happened to schedule them. */
    code = ok ? exit_ok : exit_build_failure;
    for(size_t i = 0; code == exit_ok && i < count; i++)
        code = absorb(&tasks[i], request, result, backend.path, root);

    free_tasks(tasks, count, argvs);
    str_list_free(&files);
    return code;
}
