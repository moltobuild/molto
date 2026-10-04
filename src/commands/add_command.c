#include <molto/commands/add_command.h>

#include <molto/exit_code.h>
#include <molto/project/manifest_edit.h>
#include <molto/project/project_deps.h>
#include <molto/services/credentials_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/manifest_service.h>
#include <molto/services/registry_service.h>
#include <molto/services/resolve_service.h>
#include <molto/services/source_service.h>
#include <molto/util/loader.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <string.h>

#define MANIFEST_FILENAME "Project.toml"

#define PATH_BUFFER_SIZE 4096

/* Room for the right-hand side an entry gets: an inline table with a URL in
   it, plus the keys around it. */
#define VALUE_BUFFER_SIZE 1536

/* What the spinner turns under. The name goes in because `molto add` is about
   one package: "resolving" alone would be true of any command that reaches a
   registry, and the person watching already knows which one they typed. */
#define LOADER_LABEL_FORMAT "resolving %s ..."

static int write_entry(const char *name, const char *value, bool development);

/* Find the manifest of the workspace this was run in. */
static bool manifest_path(char *out, size_t out_size) {
    char root[PATH_BUFFER_SIZE];
    if(!workspace_find_root(root, sizeof root)) {
        fprintf(stderr, "molto: not inside a Molto workspace (no " MANIFEST_FILENAME ")\n");
        return false;
    }
    if(!fs_format_path(out, out_size, "%s/" MANIFEST_FILENAME, root)) {
        fprintf(stderr, "molto: the path to " MANIFEST_FILENAME " is too long\n");
        return false;
    }
    return true;
}

/* Which registry answers when no version was given.
 *
 * A named one is looked up in the manifest that is about to be edited, then
 * whatever `molto login` stored, then the official one — the same order the
 * resolver uses, so `molto add` and the build that follows it cannot end up
 * asking two different registries about one name. */
static const char *registry_url(const char *named) {
    static char url[REGISTRY_URL_MAX];

    if(named != NULL) {
        char root[PATH_BUFFER_SIZE];
        char path[PATH_BUFFER_SIZE];
        project_ctx ctx;
        char err[256] = "";
        if(workspace_find_root(root, sizeof root) &&
           fs_format_path(path, sizeof path, "%s/" MANIFEST_FILENAME, root) &&
           project_load(path, &ctx, err, sizeof err)) {
            const char *declared = project_registries_url(&ctx.registries, named);
            if(declared != NULL) {
                snprintf(url, sizeof url, "%s", declared);
                return url;
            }
        }
    }

    credentials creds = {0};
    if(credentials_load(&creds, NULL, 0) && creds.registry[0] != '\0') {
        snprintf(url, sizeof url, "%s", creds.registry);
        return url;
    }
    return REGISTRY_DEFAULT_URL;
}

/* The TOML right-hand side for the entry being added.
 *
 * The short form for a plain registry dependency, because that is what a
 * manifest written by hand would say and `molto add` should not make a file
 * look machine-written. An inline table as soon as there is a second thing to
 * say. */
static bool compose_value(const char *version, const char *source_key, const char *source,
                          const char *registry, char *out, size_t out_size) {
    int written;
    if(source == NULL) {
        written = registry == NULL
                      ? snprintf(out, out_size, "\"%s\"", version)
                      : snprintf(out, out_size, "{ version = \"%s\", registry = \"%s\" }", version,
                                 registry);
    } else if(version == NULL) {
        written = snprintf(out, out_size, "{ %s = \"%s\" }", source_key, source);
    } else {
        /* A git reference arrives as the version: `molto add x@v1 --git …`
           means the tag, not a semver release. */
        written =
            snprintf(out, out_size, "{ %s = \"%s\", tag = \"%s\" }", source_key, source, version);
    }
    return written > 0 && (size_t)written < out_size;
}

bool add_command_asks_registry(const char *version, const char *source) {
    return source == NULL && (version == NULL || version[0] == '\0');
}

/*
 * Ask the registry for the newest release of `name`, turning a spinner at it.
 *
 * The loader owns stderr for exactly as long as the request lasts, and takes
 * its row away before anything else is written. Nothing has to lock for that:
 * curl is captured whole by `registry_service`, and what this command says it
 * says afterwards. On a pipe or a log file there is no loader and no row, and
 * this reads exactly as the bare call it wraps.
 */
static bool ask_for_newest(const char *url, const char *name, char *out, size_t out_size,
                           char *reason, size_t reason_size) {
    char label[LOADER_LABEL_MAX];
    snprintf(label, sizeof label, LOADER_LABEL_FORMAT, name);

    loader *spinner = loader_start(stderr, label);
    const bool ok = resolve_latest_version(url, name, out, out_size, reason, reason_size);
    loader_stop(spinner);
    return ok;
}

int add_command_run(const char *name, const char *version, const char *source_key,
                    const char *source, const char *registry, bool development) {
    if(name == NULL || name[0] == '\0') {
        fprintf(stderr, "molto: add needs the name of a dependency\n");
        return exit_usage_error;
    }
    if(!manifest_is_valid_name(name)) {
        fprintf(stderr, "molto: '%s' is not a package name\n", name);
        return exit_usage_error;
    }
    /* No version asked for: take the newest the registry has. The number is
       then written into the manifest like any other, so what a build resolves
       is still exactly what the file says — "newest" is decided once, here,
       and never again behind the user's back. */
    char newest[DEP_VERSION_MAX] = "";
    if(add_command_asks_registry(version, source)) {
        /* Resolved before the spinner starts rather than inside the call it
           labels: this one reads the manifest, and the loader is only safe to
           run over work that says nothing of its own. */
        const char *url = registry_url(registry);

        char reason[512] = "";
        if(!ask_for_newest(url, name, newest, sizeof newest, reason, sizeof reason)) {
            fprintf(stderr, "molto: %s\n", reason);
            return exit_dependency_failure;
        }
        version = newest;
    }
    if(version != NULL && source == NULL) {
        char range_operator[8] = "";
        if(!manifest_is_exact_version(version, range_operator, sizeof range_operator)) {
            fprintf(stderr,
                    "molto: '%s' is not an exact version. Ranges are not part of the manifest "
                    "format: they let a release nobody has read into a build without a diff\n",
                    version);
            return exit_usage_error;
        }
    }

    char value[VALUE_BUFFER_SIZE];
    if(!compose_value(version, source_key, source, registry, value, sizeof value)) {
        fprintf(stderr, "molto: the entry for '%s' is too long\n", name);
        return exit_usage_error;
    }
    return write_entry(name, value, development);
}

/* Put `name = value` into the table `development` selects. */
static int write_entry(const char *name, const char *value, bool development) {
    char path[PATH_BUFFER_SIZE];
    if(!manifest_path(path, sizeof path))
        return exit_invalid_manifest;

    const char *table = development ? "dev-deps" : "deps";
    char err[512] = "";
    if(!manifest_add_dep(path, table, name, value, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_invalid_manifest;
    }

    printf("Added %s = %s to [%s]\n", name, value, table);
    return exit_ok;
}

/* Characters a git URL or reference may carry into the manifest. Anything that
   would end or escape a TOML string, or a line, is refused rather than quoted:
   none of it belongs in either. */
static bool is_plain_text(const char *text) {
    for(const char *p = text; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        if(c <= ' ' || c == '"' || c == '\\' || c == 0x7f)
            return false;
    }
    return true;
}

bool add_git_spec_parse(const char *spec, char *name, size_t name_size, char *url, size_t url_size,
                        char *reference, size_t reference_size, char *err, size_t err_size) {
    static const char prefix[] = ADD_GIT_PREFIX;
    if(strncmp(spec, prefix, sizeof prefix - 1) != 0) {
        snprintf(err, err_size, "'%s' does not begin with %s", spec, prefix);
        return false;
    }
    const char *location = spec + sizeof prefix - 1;
    const char *hash = strchr(location, '#');
    const size_t url_length = hash == NULL ? strlen(location) : (size_t)(hash - location);
    const char *ref = hash == NULL ? "" : hash + 1;
    if(url_length == 0 || url_length >= url_size || strlen(ref) >= reference_size) {
        snprintf(err, err_size, "'%s' is not a git URL molto can hold", spec);
        return false;
    }
    snprintf(url, url_size, "%.*s", (int)url_length, location);
    snprintf(reference, reference_size, "%s", ref);
    if(url[0] == '-' || reference[0] == '-' || !is_plain_text(url) || !is_plain_text(reference) ||
       (hash != NULL && reference[0] == '\0')) {
        snprintf(err, err_size, "'%s' is not a git URL molto can hold", spec);
        return false;
    }

    /* The name is the repository's: the last segment of the path, without a
       trailing slash or `.git`. `git@host:org/repo` ends the same way. */
    size_t end = url_length;
    while(end > 0 && url[end - 1] == '/')
        end--;
    if(end >= 4 && strncmp(url + end - 4, ".git", 4) == 0)
        end -= 4;
    size_t start = end;
    while(start > 0 && url[start - 1] != '/' && url[start - 1] != ':')
        start--;
    if(end == start || end - start >= name_size) {
        snprintf(err, err_size, "'%s' names no repository", url);
        return false;
    }
    snprintf(name, name_size, "%.*s", (int)(end - start), url + start);
    if(!manifest_is_valid_name(name)) {
        snprintf(err, err_size,
                 "'%s' is not a package name; name it yourself with "
                 "`molto add <name> --git %s`",
                 name, url);
        return false;
    }
    return true;
}

int add_git_command_run(const char *spec, bool development) {
    char name[128];
    char url[1024];
    char requested[256];
    char err[512] = "";
    if(!add_git_spec_parse(spec, name, sizeof name, url, sizeof url, requested, sizeof requested,
                           err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_usage_error;
    }

    /* A git dependency needs a branch, tag or rev before any build can cache
       it, so the question is asked now and the answer written down, the way a
       registry's newest version is. */
    const char *key = NULL;
    char reference[256];
    if(!source_git_reference_key(url, requested, &key, reference, sizeof reference, err,
                                 sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_dependency_failure;
    }

    char value[VALUE_BUFFER_SIZE];
    const int written =
        snprintf(value, sizeof value, "{ git = \"%s\", %s = \"%s\" }", url, key, reference);
    if(written < 0 || (size_t)written >= sizeof value) {
        fprintf(stderr, "molto: the entry for '%s' is too long\n", name);
        return exit_usage_error;
    }
    return write_entry(name, value, development);
}

int remove_command_run(const char *name) {
    if(name == NULL || name[0] == '\0') {
        fprintf(stderr, "molto: remove needs the name of a dependency\n");
        return exit_usage_error;
    }

    char path[PATH_BUFFER_SIZE];
    if(!manifest_path(path, sizeof path))
        return exit_invalid_manifest;

    char err[512] = "";
    if(!manifest_remove_dep(path, name, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_invalid_manifest;
    }

    printf("Removed %s\n", name);
    return exit_ok;
}
