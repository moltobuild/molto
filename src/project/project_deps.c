#include <molto/project/project_deps.h>

#include <molto/services/manifest_service.h>
#include <molto/util/str_list.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DEPS_SECTION "deps"
#define DEV_DEPS_SECTION "dev-deps"
#define REGISTRIES_SECTION "registries"

/* Longest "deps.<name>" this reader composes. */
#define DEP_SECTION_MAX TOML_SECTION_MAX

static bool set_error(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool set_error(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

/* --- what a dependency may say --- */

/* The source keys, in the order they are looked for. Exactly one may appear:
   two would leave the reader to pick, and a dependency that resolves one way
   here and another way elsewhere is a suggestion rather than a dependency. */
static const struct {
    const char *key;
    dep_source source;
} DEP_SOURCES[] = {
    {"version", dep_source_version},
    {"git", dep_source_git},
    {"path", dep_source_path},
    {"archive", dep_source_archive},
};

/* The git references. At most one, and only beside `git`. */
static const struct {
    const char *key;
    dep_git_ref ref;
} DEP_GIT_REFS[] = {
    {"branch", dep_git_ref_branch},
    {"tag", dep_git_ref_tag},
    {"rev", dep_git_ref_rev},
};

/* Everything else this reader acts on. */
static const char *const DEP_SUPPORTED_KEYS[] = {"registry", "sha256", "strip_prefix"};

/* Keys RFC-0003 defines and this reader does not implement. Refused rather
   than ignored, following [package].artifact: accepting a key and quietly
   doing nothing with it tells the user their manifest said something it did
   not. `recipe` is here for a different reason — RFC-0003 forbids pairing it
   with `version`, so a recipe dependency has no version to resolve, which is a
   gap in the specification rather than in this code. */
static const char *const DEP_PENDING_KEYS[] = {"recipe", "artifact", "optional", "features",
                                               "default_features"};

static bool is_in(const char *const *list, size_t count, const char *key) {
    for(size_t i = 0; i < count; i++) {
        if(strcmp(list[i], key) == 0)
            return true;
    }
    return false;
}

static bool is_known_key(const char *key) {
    for(size_t i = 0; i < sizeof DEP_SOURCES / sizeof DEP_SOURCES[0]; i++) {
        if(strcmp(DEP_SOURCES[i].key, key) == 0)
            return true;
    }
    for(size_t i = 0; i < sizeof DEP_GIT_REFS / sizeof DEP_GIT_REFS[0]; i++) {
        if(strcmp(DEP_GIT_REFS[i].key, key) == 0)
            return true;
    }
    return is_in(DEP_SUPPORTED_KEYS, sizeof DEP_SUPPORTED_KEYS / sizeof DEP_SUPPORTED_KEYS[0], key);
}

/* Every key is classified before any is read, so a typo is an error instead of
   a dependency that silently resolves to something else. */
static bool check_keys(doc_view doc, const char *table, const char *section, const char *name,
                       char *err, size_t err_size) {
    str_list keys;
    str_list_init(&keys);
    if(!doc_table_members(doc, section, &keys)) {
        str_list_free(&keys);
        return set_error(err, err_size, "could not read [%s].%s", table, name);
    }

    bool ok = true;
    for(size_t i = 0; ok && i < str_list_count(&keys); i++) {
        const char *key = str_list_get(&keys, i);
        if(is_in(DEP_PENDING_KEYS, sizeof DEP_PENDING_KEYS / sizeof DEP_PENDING_KEYS[0], key))
            ok = set_error(err, err_size, "[%s].%s: '%s' is not supported yet", table, name, key);
        else if(!is_known_key(key))
            ok = set_error(err, err_size, "[%s].%s: unknown key '%s'", table, name, key);
    }

    str_list_free(&keys);
    return ok;
}

/* --- reading one dependency --- */

static bool read_version(const char *table, const char *name, const char *version, project_dep *out,
                         char *err, size_t err_size) {
    char range_operator[8] = "";
    if(!manifest_is_exact_version(version, range_operator, sizeof range_operator)) {
        if(range_operator[0] != '\0')
            return set_error(err, err_size,
                             "[%s].%s = \"%s\" is a version range, and versions are exact. The "
                             "operator '%s' is not part of the manifest format (RFC-0008): a range "
                             "lets a release nobody has read enter this build without a diff. "
                             "Write the one version you mean",
                             table, name, version, range_operator);
        return set_error(err, err_size, "[%s].%s = \"%s\" is not a version", table, name, version);
    }
    if(strlen(version) >= DEP_VERSION_MAX)
        return set_error(err, err_size, "[%s].%s: the version is longer than %d characters", table,
                         name, DEP_VERSION_MAX - 1);

    out->source = dep_source_version;
    out->resolution = dep_resolution_registry;
    snprintf(out->version, sizeof out->version, "%s", version);
    return true;
}

/* The plain-string form. RFC-0003: `dep = "1.2.3"` is exactly
   `dep = { version = "1.2.3" }`. */
static bool read_shorthand(doc_view doc, const char *table, const char *name, project_dep *out,
                           char *err, size_t err_size) {
    char version[DEP_VERSION_MAX * 2];
    if(!doc_get_string(doc, table, name, version, sizeof version))
        return set_error(err, err_size, "[%s].%s must be a version string or a table", table, name);
    return read_version(table, name, version, out, err, err_size);
}

static bool read_source_key(doc_view doc, const char *table, const char *section, const char *name,
                            project_dep *out, char *err, size_t err_size) {
    bool found = false;
    for(size_t i = 0; i < sizeof DEP_SOURCES / sizeof DEP_SOURCES[0]; i++) {
        char value[DEP_LOCATION_MAX];
        if(!doc_get_string(doc, section, DEP_SOURCES[i].key, value, sizeof value))
            continue;
        if(found)
            return set_error(err, err_size,
                             "[%s].%s names more than one source, and a dependency has exactly "
                             "one",
                             table, name);
        found = true;

        if(DEP_SOURCES[i].source == dep_source_version) {
            if(!read_version(table, name, value, out, err, err_size))
                return false;
            continue;
        }

        out->source = DEP_SOURCES[i].source;
        out->resolution = dep_resolution_carried;
        snprintf(out->location, sizeof out->location, "%s", value);
    }

    if(!found)
        return set_error(err, err_size,
                         "[%s].%s names no source: one of version, git, path or archive", table,
                         name);
    return true;
}

static bool read_git_ref(doc_view doc, const char *table, const char *section, const char *name,
                         project_dep *out, char *err, size_t err_size) {
    bool found = false;
    for(size_t i = 0; i < sizeof DEP_GIT_REFS / sizeof DEP_GIT_REFS[0]; i++) {
        char value[DEP_REFERENCE_MAX];
        if(!doc_get_string(doc, section, DEP_GIT_REFS[i].key, value, sizeof value))
            continue;
        if(found)
            return set_error(err, err_size,
                             "[%s].%s sets more than one of branch, tag and rev, and they name "
                             "the same thing",
                             table, name);
        found = true;
        out->git_ref = DEP_GIT_REFS[i].ref;
        snprintf(out->reference, sizeof out->reference, "%s", value);
    }

    if(found && out->source != dep_source_git)
        return set_error(err, err_size, "[%s].%s names a git reference but no git repository",
                         table, name);
    return true;
}

static bool read_table(doc_view doc, const char *table, const char *section, const char *name,
                       project_dep *out, char *err, size_t err_size) {
    if(!check_keys(doc, table, section, name, err, err_size))
        return false;
    if(!read_source_key(doc, table, section, name, out, err, err_size))
        return false;
    if(!read_git_ref(doc, table, section, name, out, err, err_size))
        return false;

    if(!doc_get_string(doc, section, "registry", out->registry, sizeof out->registry))
        out->registry[0] = '\0';
    if(!doc_get_string(doc, section, "sha256", out->sha256, sizeof out->sha256))
        out->sha256[0] = '\0';
    if(!doc_get_string(doc, section, "strip_prefix", out->strip_prefix, sizeof out->strip_prefix))
        out->strip_prefix[0] = '\0';

    if(out->resolution == dep_resolution_registry)
        return true;

    /* Converted now rather than at fetch time, so the same two rules a
       recipe's [source] owes are enforced while the manifest is still what the
       error can point at. */
    source_spec spec;
    char reason[256] = "";
    if(!project_dep_to_source(out, &spec, reason, sizeof reason))
        return set_error(err, err_size, "[%s].%s: %s", table, name, reason);
    return true;
}

static bool read_one(doc_view doc, const char *table, const char *name, project_dep *out, char *err,
                     size_t err_size) {
    memset(out, 0, sizeof *out);

    if(!manifest_is_valid_name(name))
        return set_error(err, err_size, "[%s].%s is not a package name", table, name);
    if(strlen(name) >= DEP_NAME_MAX)
        return set_error(err, err_size, "[%s].%s is longer than %d characters", table, name,
                         DEP_NAME_MAX - 1);
    snprintf(out->name, sizeof out->name, "%s", name);

    char section[DEP_SECTION_MAX];
    if(snprintf(section, sizeof section, "%s.%s", table, name) < 0)
        return set_error(err, err_size, "[%s].%s: the name is too long", table, name);

    /* A value under [deps] is the shorthand; a table under [deps.<name>] is the
       long form. The parser stores an inline table exactly as it stores a
       header, so both arrive here the same way. */
    return doc_has_table(doc, section) ? read_table(doc, table, section, name, out, err, err_size)
                                       : read_shorthand(doc, table, name, out, err, err_size);
}

bool project_deps_read_table(doc_view doc, const char *table, project_deps *out, char *err,
                             size_t err_size) {
    out->count = 0;

    str_list names;
    str_list_init(&names);
    if(!doc_table_members(doc, table, &names)) {
        str_list_free(&names);
        return set_error(err, err_size, "could not read the [%s] table", table);
    }

    bool ok = true;
    const size_t total = str_list_count(&names);
    /* Checked before any entry is read, so the message is about the limit and
       not about whatever the thirty-third entry happens to say. */
    if(total > PROJECT_MAX_DEPS)
        ok = set_error(err, err_size, "[%s] has more than %d entries", table, PROJECT_MAX_DEPS);

    for(size_t i = 0; ok && i < total; i++) {
        ok = read_one(doc, table, str_list_get(&names, i), &out->items[out->count], err, err_size);
        if(ok)
            out->count++;
    }

    str_list_free(&names);
    if(!ok)
        out->count = 0;
    return ok;
}

bool project_deps_read_doc(doc_view doc, project_deps *out, char *err, size_t err_size) {
    return project_deps_read_table(doc, DEPS_SECTION, out, err, err_size);
}

bool project_dev_deps_read_doc(doc_view doc, project_deps *out, char *err, size_t err_size) {
    return project_deps_read_table(doc, DEV_DEPS_SECTION, out, err, err_size);
}

bool project_deps_read(const toml_document *doc, project_deps *out, char *err, size_t err_size) {
    return project_deps_read_doc(doc_from_toml(doc), out, err, err_size);
}

bool project_dep_to_source(const project_dep *dep, source_spec *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);

    switch(dep->source) {
    case dep_source_git:
        out->origin = source_origin_git;
        break;
    case dep_source_path:
        out->origin = source_origin_path;
        break;
    case dep_source_archive:
        out->origin = source_origin_archive;
        break;
    case dep_source_version:
        return set_error(err, err_size,
                         "a version dependency has a coordinate and not a source: resolve it "
                         "against a registry first");
    }

    snprintf(out->location, sizeof out->location, "%s", dep->location);
    snprintf(out->reference, sizeof out->reference, "%s", dep->reference);
    snprintf(out->sha256, sizeof out->sha256, "%s", dep->sha256);
    snprintf(out->strip_prefix, sizeof out->strip_prefix, "%s", dep->strip_prefix);

    return source_spec_validate(out, err, err_size);
}

/* --- registries --- */

bool project_registries_read(const toml_document *doc, project_registries *out, char *err,
                             size_t err_size) {
    out->count = 0;

    str_list names;
    str_list_init(&names);
    if(!toml_section_keys(doc, REGISTRIES_SECTION, &names)) {
        str_list_free(&names);
        return set_error(err, err_size, "could not read the [registries] table");
    }

    bool ok = true;
    const size_t total = str_list_count(&names);
    if(total > PROJECT_MAX_REGISTRIES)
        ok = set_error(err, err_size, "[registries] has more than %d entries",
                       PROJECT_MAX_REGISTRIES);

    for(size_t i = 0; ok && i < total; i++) {
        const char *name = str_list_get(&names, i);
        char url[REGISTRY_URL_MAX];
        if(!toml_get_string(doc, REGISTRIES_SECTION, name, url, sizeof url))
            ok = set_error(err, err_size, "[registries].%s must be a URL string", name);
        else if(strlen(name) >= REGISTRY_NAME_MAX)
            ok = set_error(err, err_size, "[registries].%s is longer than %d characters", name,
                           REGISTRY_NAME_MAX - 1);
        else {
            snprintf(out->names[out->count], REGISTRY_NAME_MAX, "%s", name);
            snprintf(out->urls[out->count], REGISTRY_URL_MAX, "%s", url);
            out->count++;
        }
    }

    str_list_free(&names);
    if(!ok)
        out->count = 0;
    return ok;
}

const char *project_registries_url(const project_registries *registries, const char *name) {
    for(size_t i = 0; i < registries->count; i++) {
        if(strcmp(registries->names[i], name) == 0)
            return registries->urls[i];
    }
    return NULL;
}

bool project_deps_check_registries(const project_deps *deps, const char *table,
                                   const project_registries *registries, char *err,
                                   size_t err_size) {
    for(size_t i = 0; i < deps->count; i++) {
        const project_dep *dep = &deps->items[i];
        if(dep->registry[0] == '\0')
            continue;
        /* Falling back to the official registry would resolve the dependency
           against somewhere the manifest did not name. */
        if(project_registries_url(registries, dep->registry) == NULL)
            return set_error(err, err_size,
                             "[%s].%s names the registry '%s', which [registries] does not "
                             "declare",
                             table, dep->name, dep->registry);
    }
    return true;
}

/* --- reporting --- */

static const char *source_label(const project_dep *dep) {
    switch(dep->source) {
    case dep_source_version:
        return "version";
    case dep_source_git:
        return "git";
    case dep_source_path:
        return "path";
    case dep_source_archive:
        return "archive";
    }
    return "?";
}

const project_dep *project_deps_find(const project_deps *deps, const char *name) {
    for(size_t i = 0; i < deps->count; i++) {
        if(strcmp(deps->items[i].name, name) == 0)
            return &deps->items[i];
    }
    return NULL;
}

void project_deps_dump(const project_deps *deps, FILE *stream) {
    fprintf(stream, "deps: %zu\n", deps->count);
    for(size_t i = 0; i < deps->count; i++) {
        const project_dep *dep = &deps->items[i];
        fprintf(stream, "  %s (%s) ", dep->name, source_label(dep));
        if(dep->resolution == dep_resolution_registry)
            fprintf(stream, "%s%s%s\n", dep->version, dep->registry[0] == '\0' ? "" : " from ",
                    dep->registry);
        else
            fprintf(stream, "%s%s%s\n", dep->location, dep->reference[0] == '\0' ? "" : " @ ",
                    dep->reference);
    }
}
