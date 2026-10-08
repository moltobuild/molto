#include <molto/services/package_service.h>

#include <molto/exit_code.h>
#include <molto/services/build_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/source_discovery.h>
#include <molto/util/glob.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACKAGE_PATH_MAX 4096

static bool fail(char *err, size_t size, const char *message) {
    snprintf(err, size, "%s; its author can run `molto package` to see what a dependency needs",
             message);
    return false;
}

/* Package paths must remain relative, including on Windows. */
static bool safe_path(const char *path) {
    if(!*path || fs_path_is_absolute(path) || strchr(path, '\\') || strchr(path, ':'))
        return false;
    for(const char *at = path; *at;) {
        const char *end = strchr(at, '/');
        size_t n = end ? (size_t)(end - at) : strlen(at);
        if(n == 2 && !strncmp(at, "..", 2))
            return false;
        if(!end)
            break;
        at = end + 1;
    }
    return true;
}

static bool below(const char *path, const char *dir) {
    while(!strncmp(dir, "./", 2))
        dir += 2;
    size_t n = strlen(dir);
    while(n && dir[n - 1] == '/')
        n--;
    return n == 0 || (n == 1 && dir[0] == '.') ||
           (!strncmp(path, dir, n) && (path[n] == '/' || path[n] == '\0'));
}

static bool license_file(const char *path) {
    return !strchr(path, '/') && (!strncmp(path, "LICENSE", 7) || !strncmp(path, "NOTICE", 6) ||
                                  !strncmp(path, "COPYING", 7));
}

static bool matches(const char *pattern, const char *path) {
    while(!strncmp(pattern, "./", 2))
        pattern += 2;
    return glob_match(pattern, path) || below(path, pattern);
}

bool package_select(const project_ctx *ctx, const str_list *files, str_list *kept, char *err,
                    size_t err_size) {
    bool matched[PROJECT_MAX_OPTS] = {0};
    for(size_t i = 0; i < ctx->file_count; i++) {
        if(!safe_path(ctx->files[i]))
            return fail(err, err_size, "[package].files must name paths inside the package");
    }
    for(size_t i = 0; i < str_list_count(files); i++) {
        const char *path = str_list_get(files, i);
        if(!safe_path(path))
            return fail(err, err_size, "the package inventory contains an unsafe path");
        bool keep = !strcmp(path, "Project.toml") || below(path, "src") || below(path, "include") ||
                    !strcmp(path, ctx->entry) || license_file(path);
        for(size_t j = 0; j < ctx->target.options.include_count; j++)
            keep = keep || below(path, ctx->target.options.include[j]);
        for(size_t j = 0; j < ctx->interface.include_count; j++)
            keep = keep || below(path, ctx->interface.include[j]);
        for(size_t j = 0; j < ctx->file_count; j++) {
            if(matches(ctx->files[j], path)) {
                matched[j] = true;
                keep = true;
            }
        }
        /* These exclusions are unconditional, even when an extra glob is broad. */
        if(!strcmp(path, "src/main.c") || below(path, ".git") || !strcmp(path, ".molto-fetched"))
            keep = false;
        if(keep && !str_list_push(kept, path))
            return fail(err, err_size, "out of memory selecting package files");
    }
    for(size_t i = 0; i < ctx->file_count; i++) {
        if(!matched[i]) {
            char message[256];
            snprintf(message, sizeof message, "[package].files pattern '%s' matches nothing",
                     ctx->files[i]);
            return fail(err, err_size, message);
        }
    }
    return true;
}

/* A directory or file named by the manifest, resolved inside its package. */
static bool check_path(const char *root, const char *relative, bool directory) {
    char path[PACKAGE_PATH_MAX], real[PACKAGE_PATH_MAX], base[PACKAGE_PATH_MAX];
    if(!safe_path(relative) || !fs_format_path(path, sizeof path, "%s/%s", root, relative) ||
       !fs_real_path(root, base, sizeof base) || !fs_real_path(path, real, sizeof real))
        return false;
    size_t n = strlen(base);
    return !strncmp(real, base, n) && (real[n] == '/' || !real[n]) &&
           (directory ? fs_is_dir(path) : !fs_is_dir(path));
}

static bool same_package_path(const char *root, const char *left, const char *right) {
    char a[PACKAGE_PATH_MAX], b[PACKAGE_PATH_MAX], real_a[PACKAGE_PATH_MAX],
        real_b[PACKAGE_PATH_MAX];
    return fs_format_path(a, sizeof a, "%s/%s", root, left) &&
           fs_format_path(b, sizeof b, "%s/%s", root, right) &&
           fs_real_path(a, real_a, sizeof real_a) && fs_real_path(b, real_b, sizeof real_b) &&
           !strcmp(real_a, real_b);
}

static bool inventory(const char *root, str_list *out) {
    str_list absolute;
    str_list_init(&absolute);
    bool ok = source_discovery_collect_all(root, &absolute);
    for(size_t i = 0; ok && i < str_list_count(&absolute); i++)
        ok = str_list_push(out, str_list_get(&absolute, i) + strlen(root) + 1);
    str_list_free(&absolute);
    return ok;
}

static void problem(char *issues, size_t size, const char *format, ...) {
    size_t used = strlen(issues);
    if(used >= size - 1)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(issues + used, size - used, format, args);
    va_end(args);
}

bool package_read(const char *root, const char *name, project_ctx *ctx, recipe_artifacts *out,
                  char *err, size_t err_size) {
    char path[PACKAGE_PATH_MAX];
    char load_error[512] = "";
    if(!fs_format_path(path, sizeof path, "%s/Project.toml", root))
        return fail(err, err_size, "the manifest path is too long");
    if(!project_load_dependency(path, ctx, load_error, sizeof load_error))
        return fail(err, err_size, load_error);
    memset(out, 0, sizeof *out);
    char issues[2048] = "";
    if(name && strcmp(name, ctx->project_name))
        problem(issues, sizeof issues, "dependency name '%s' does not equal [package].name '%s'; ",
                name, ctx->project_name);
    if(!ctx->version_declared || !manifest_is_exact_version(ctx->version, NULL, 0))
        problem(issues, sizeof issues, "[package].version is required; ");
    if(ctx->artifact != artifact_static && ctx->artifact != artifact_shared)
        problem(issues, sizeof issues, "[package].artifact must be static or shared; ");
    if(!fs_format_path(path, sizeof path, "%s/recipe.toml", root))
        return fail(err, err_size, "the package path is too long");
    if(fs_path_exists(path))
        problem(issues, sizeof issues, "both recipe.toml and Project.toml describe this package; ");
    for(size_t scope = 0; scope < 2; scope++) {
        const project_options *options = scope ? &ctx->interface : &ctx->target.options;
        for(size_t i = 0; i < options->include_count; i++) {
            if(scope && same_package_path(root, options->include[i], "include"))
                problem(
                    issues, sizeof issues,
                    "[interface].include must not name include/: it is exported by convention; ");
            if(!check_path(root, options->include[i], true))
                problem(issues, sizeof issues,
                        "[%s].include '%s' is not a directory inside the package; ",
                        scope ? "interface" : "target", options->include[i]);
        }
    }
    if(*ctx->entry &&
       (same_package_path(root, ctx->entry, "src/main.c") || !check_path(root, ctx->entry, false)))
        problem(issues, sizeof issues,
                "[interface].entry must exist inside the package and must not be src/main.c; ");
    if(fs_format_path(path, sizeof path, "%s/include", root) && fs_path_exists(path) &&
       !check_path(root, "include", true))
        problem(issues, sizeof issues, "include/ must be a directory inside the package; ");
    str_list sources, files, kept;
    str_list_init(&sources);
    str_list_init(&files);
    str_list_init(&kept);
    if(fs_format_path(path, sizeof path, "%s/src", root) && fs_is_dir(path)) {
        if(!source_discovery_collect(path, &sources))
            problem(issues, sizeof issues, "could not discover src/; ");
    }
    for(size_t i = 0; i < str_list_count(&sources); i++) {
        const char *relative = str_list_get(&sources, i) + strlen(root) + 1;
        if(!strcmp(relative, "src/main.c"))
            continue;
        if(out->source_count == RECIPE_MAX_SOURCES || strlen(relative) >= RECIPE_SOURCE_MAX) {
            problem(issues, sizeof issues, "src/ exceeds the supported source capacity; ");
            break;
        }
        snprintf(out->sources[out->source_count++], RECIPE_SOURCE_MAX, "%s", relative);
    }
    if(!out->source_count)
        problem(issues, sizeof issues, "src/ needs a source other than src/main.c; ");
    if(*ctx->entry) {
        bool found = false;
        for(size_t i = 0; i < out->source_count; i++)
            found = found || !strcmp(out->sources[i], ctx->entry);
        if(!found) {
            if(out->source_count == RECIPE_MAX_SOURCES)
                problem(issues, sizeof issues, "the entry exceeds the supported source capacity; ");
            else
                snprintf(out->sources[out->source_count++], RECIPE_SOURCE_MAX, "%s", ctx->entry);
        }
        snprintf(out->entry, sizeof out->entry, "%s", ctx->entry);
    }
    char selection_error[512] = "";
    if(!inventory(root, &files) ||
       !package_select(ctx, &files, &kept, selection_error, sizeof selection_error))
        problem(issues, sizeof issues, "%s; ",
                *selection_error ? selection_error : "could not inventory the package");
    for(size_t i = 0; i < str_list_count(&kept); i++) {
        const char *file = str_list_get(&kept, i);
        if(!check_path(root, file, false))
            problem(issues, sizeof issues, "kept file '%s' resolves outside the package; ", file);
    }
    bool license = false;
    for(size_t i = 0; i < str_list_count(&files); i++)
        license = license || license_file(str_list_get(&files, i));
    str_list_free(&sources);
    str_list_free(&files);
    str_list_free(&kept);
    if(*issues)
        return fail(err, err_size, issues);
    if(!*ctx->about.license || !license)
        fprintf(stderr, "molto: warning: %s needs [package].license and a licence file\n",
                ctx->project_name);
    if(!*ctx->about.repository)
        fprintf(stderr, "molto: warning: %s has no [package].repository\n", ctx->project_name);
    out->from_manifest = true;
    out->type = recipe_artifact_source;
    snprintf(out->std, sizeof out->std, "%s", ctx->target.std);
    snprintf(out->cpp_std, sizeof out->cpp_std, "%s", ctx->target.cpp_std);
    out->options = ctx->interface;
    out->private_options = ctx->target.options;
    /* include/ is public by convention; other target includes stay private. */
    out->private_options.include_count = 0;
    for(size_t i = 0; i < ctx->target.options.include_count; i++) {
        const char *dir = ctx->target.options.include[i];
        if(!below("include", dir) || !below(dir, "include"))
            snprintf(out->private_options.include[out->private_options.include_count++],
                     PROJECT_OPT_LEN, "%s", dir);
    }
    if(fs_format_path(path, sizeof path, "%s/include", root) && fs_is_dir(path))
        snprintf(out->options.include[out->options.include_count++], PROJECT_OPT_LEN, "include");
    else if(!ctx->interface.include_count)
        fprintf(stderr, "molto: warning: %s exports no public header directory\n",
                ctx->project_name);
    if(ctx->target.link_count + ctx->interface_link_count > PROJECT_MAX_LINK)
        return fail(err, err_size, "target and interface links exceed the supported capacity");
    for(size_t i = 0; i < ctx->target.link_count; i++)
        snprintf(out->link[out->link_count++], PROJECT_LINK_NAME_MAX, "%s", ctx->target.link[i]);
    for(size_t i = 0; i < ctx->interface_link_count; i++)
        snprintf(out->link[out->link_count++], PROJECT_LINK_NAME_MAX, "%s", ctx->interface_link[i]);
    out->requirements = ctx->target;
    return true;
}

/* Remove entire unlisted subtrees; never descend through a directory symlink. */
static bool prune_tree(const char *root, const char *relative, const str_list *kept,
                       const project_ctx *ctx) {
    char directory[PACKAGE_PATH_MAX];
    if(!fs_format_path(directory, sizeof directory, "%s/%s", root, relative))
        return false;
    DIR *dir = opendir(directory);
    if(!dir)
        return false;
    bool ok = true;
    const struct dirent *entry;
    while(ok && (entry = readdir(dir))) {
        if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char name[PACKAGE_PATH_MAX], path[PACKAGE_PATH_MAX];
        ok = fs_format_path(name, sizeof name, "%s%s%s", relative, *relative ? "/" : "",
                            entry->d_name) &&
             fs_format_path(path, sizeof path, "%s/%s", root, name);
        if(!ok)
            break;
        bool retain = false;
        for(size_t i = 0; i < str_list_count(kept); i++)
            retain = retain || below(str_list_get(kept, i), name);
        if(fs_is_dir_no_follow(path)) {
            retain = retain || below(name, "src") || below(name, "include");
            for(size_t i = 0; i < ctx->target.options.include_count; i++)
                retain = retain || below(name, ctx->target.options.include[i]) ||
                         below(ctx->target.options.include[i], name);
            for(size_t i = 0; i < ctx->interface.include_count; i++)
                retain = retain || below(name, ctx->interface.include[i]) ||
                         below(ctx->interface.include[i], name);
        }
        if(below(name, ".git"))
            retain = false;
        if(!retain)
            ok = fs_remove_tree(path);
        else if(fs_is_dir_no_follow(path))
            ok = prune_tree(root, name, kept, ctx);
    }
    closedir(dir);
    return ok;
}

bool package_prune(const char *root, char *err, size_t err_size) {
    char manifest[PACKAGE_PATH_MAX];
    if(!fs_format_path(manifest, sizeof manifest, "%s/Project.toml", root))
        return false;
    if(!fs_path_exists(manifest))
        return true;
    project_ctx *ctx = calloc(1, sizeof *ctx);
    recipe_artifacts *artifacts = calloc(1, sizeof *artifacts);
    str_list files, kept;
    str_list_init(&files);
    str_list_init(&kept);
    bool ok = ctx && artifacts && package_read(root, NULL, ctx, artifacts, err, err_size) &&
              inventory(root, &files) && package_select(ctx, &files, &kept, err, err_size);
    if(ok)
        ok = prune_tree(root, "", &kept, ctx);
    str_list_free(&files);
    str_list_free(&kept);
    free(ctx);
    free(artifacts);
    return ok;
}
