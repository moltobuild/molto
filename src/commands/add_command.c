#include <molto/commands/add_command.h>

#include <molto/exit_code.h>
#include <molto/project/manifest_edit.h>
#include <molto/project/project_deps.h>
#include <molto/services/credentials_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/manifest_service.h>
#include <molto/services/registry_service.h>
#include <molto/services/resolve_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>
#include <string.h>

#define MANIFEST_FILENAME "Project.toml"

#define PATH_BUFFER_SIZE 4096

/* Room for the right-hand side an entry gets: an inline table with a URL in
   it, plus the keys around it. */
#define VALUE_BUFFER_SIZE 1536

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
    if(source == NULL && (version == NULL || version[0] == '\0')) {
        char reason[512] = "";
        if(!resolve_latest_version(registry_url(registry), name, newest, sizeof newest, reason,
                                   sizeof reason)) {
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

    char path[PATH_BUFFER_SIZE];
    if(!manifest_path(path, sizeof path))
        return exit_invalid_manifest;

    char value[VALUE_BUFFER_SIZE];
    if(!compose_value(version, source_key, source, registry, value, sizeof value)) {
        fprintf(stderr, "molto: the entry for '%s' is too long\n", name);
        return exit_usage_error;
    }

    const char *table = development ? "dev-deps" : "deps";
    char err[512] = "";
    if(!manifest_add_dep(path, table, name, value, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_invalid_manifest;
    }

    printf("Added %s = %s to [%s]\n", name, value, table);
    return exit_ok;
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
