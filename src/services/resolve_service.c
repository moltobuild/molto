#include <molto/services/resolve_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/registry_service.h>
#include <molto/util/json.h>
#include <molto/util/semver.h>
#include <molto/util/str_list.h>

#include <stdlib.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The target a source recipe is published under: something that does not
   depend on a platform (RFC-0009). */
#define ANY_TARGET "any"

#define RESOLVE_PATH_MAX 512

/* The registry's answer, kept beside the source it describes. Named like the
   cache's own stamp so an unpacked archive cannot collide with it. */
#define RELEASE_FILE ".molto-release.json"

/* Enough to name the targets a version was published for, in the message that
   says none of them was usable. */
#define TARGET_LIST_MAX 256

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

/* --- the envelope --- */

/* The only function that knows the shape the catalogue answers with:
   { kind, name, version, targets: [...] }. Keeping it named and alone makes a
   protocol change a one-line change. */
static bool release_targets(json_value root, json_value *out, char *err, size_t err_size) {
    const json_value targets = json_get(root, "targets");
    if(json_type_of(targets) != json_type_array)
        return set_error(err, err_size, "the registry's answer carried no 'targets' list");
    *out = targets;
    return true;
}

static bool is_yanked(json_value artifact) {
    bool yanked = false;
    return json_bool(json_get(artifact, "yanked"), &yanked) && yanked;
}

/* Every target the version was published for, for a message that can be acted
   on rather than one that says only "not found". */
static void describe_targets(json_value targets, char *out, size_t size) {
    out[0] = '\0';
    size_t used = 0;
    const size_t total = json_count(targets);

    for(size_t i = 0; i < total; i++) {
        const char *name = json_string(json_get(json_at(targets, i), "target"));
        if(name == NULL)
            continue;
        const int written = snprintf(out + used, size - used, "%s%s", used > 0 ? ", " : "", name);
        if(written < 0 || (size_t)written >= size - used)
            return; /* what fits is enough to recognise the problem */
        used += (size_t)written;
    }
}

/*
 * Which artifact to use.
 *
 * Only `any` for now: nothing downstream can consume a prebuilt binary yet, so
 * preferring the host's triple would select something unusable and say nothing
 * about it. A refusal that names what *was* published is more useful than a
 * silent wrong choice, and this is the one place to revisit when binaries can
 * be consumed.
 */
static bool choose_artifact(json_value targets, const char *name, const char *version,
                            const char *wanted, json_value *out, char *err, size_t err_size) {
    const size_t total = json_count(targets);
    if(total == 0)
        return set_error(err, err_size, "%s %s is published with no targets at all", name, version);

    /* `any` unless a caller names a platform. A dependency is compiled from
       sources and does not have one; a plugin is a native executable and can
       only ever be the host's, which is why it asks (RFC-0014). */
    const char *sought = wanted != NULL ? wanted : ANY_TARGET;

    bool saw_any = false;
    for(size_t i = 0; i < total; i++) {
        const json_value artifact = json_at(targets, i);
        const char *target = json_string(json_get(artifact, "target"));
        if(target == NULL || strcmp(target, sought) != 0)
            continue;
        saw_any = true;
        /* A yanked artifact stays resolvable so old builds keep working, but it
           must not be chosen for a new one. */
        if(is_yanked(artifact))
            continue;
        *out = artifact;
        return true;
    }

    if(saw_any)
        return set_error(err, err_size, "%s %s was yanked and must not be used for a new build",
                         name, version);

    char published[TARGET_LIST_MAX];
    describe_targets(targets, published, sizeof published);
    if(wanted != NULL)
        return set_error(err, err_size, "%s %s is published for %s, and not for %s", name, version,
                         published, wanted);
    return set_error(err, err_size,
                     "%s %s is published for %s, and molto can only use a recipe published for "
                     "\"any\" yet",
                     name, version, published);
}

/* The registry is a remote party. A client that asks for one coordinate and
   caches whatever came back under it has no integrity story at all. */
static bool check_coordinate(const recipe_coordinate *coordinate, const char *name,
                             const char *version, char *err, size_t err_size) {
    if(strcmp(coordinate->name, name) != 0 || strcmp(coordinate->version, version) != 0)
        return set_error(err, err_size,
                         "asked the registry for %s %s and its recipe describes %s %s", name,
                         version, coordinate->name, coordinate->version);
    return true;
}

/* --- reading one artifact --- */

static void read_binary_fields(json_value artifact, resolved_dep *out) {
    const char *url = json_string(json_get(artifact, "download_url"));
    const char *checksum = json_string(json_get(artifact, "checksum"));
    snprintf(out->download_url, sizeof out->download_url, "%s", url == NULL ? "" : url);
    snprintf(out->checksum, sizeof out->checksum, "%s", checksum == NULL ? "" : checksum);
}

static bool read_artifact(json_value artifact, const char *name, const char *version,
                          resolved_dep *out, char *err, size_t err_size) {
    const json_value metadata = json_get(artifact, "metadata");
    if(json_type_of(metadata) != json_type_object)
        return set_error(err, err_size, "the registry served %s %s with no recipe in its metadata",
                         name, version);

    /* The recipe, read by exactly the code that reads one off disk. */
    const doc_view recipe = doc_from_json(metadata);
    if(!recipe_read_coordinate(recipe, &out->coordinate, err, err_size))
        return false;
    if(!check_coordinate(&out->coordinate, name, version, err, err_size))
        return false;
    if(!recipe_read_artifacts(recipe, &out->artifacts, err, err_size))
        return false;
    if(!recipe_read_build(recipe, &out->build, err, err_size))
        return false;
    /* Read here, while the answer is still parsed: this is what a transitive
       walk follows, and asking the registry again for a table it already sent
       would be a request per edge. */
    if(!project_deps_read_doc(recipe, &out->deps, err, err_size))
        return false;
    /* And the same for what the package says about itself, which the catalogue
       serves inside the very same recipe (RFC-0009). */
    if(!manifest_read_about(recipe, "about", &out->about, err, err_size))
        return false;

    if(out->coordinate.form == recipe_form_source)
        return source_read(recipe, &out->source, err, err_size);

    read_binary_fields(artifact, out);
    return true;
}

bool resolve_read_release_for(const char *body, const char *name, const char *version,
                              const char *target_wanted, resolved_dep *out, char *err,
                              size_t err_size) {
    memset(out, 0, sizeof *out);

    json_document *doc = json_parse(body);
    if(doc == NULL)
        return set_error(err, err_size, "the registry's answer was not JSON");

    json_value targets;
    json_value artifact;
    const bool ok =
        release_targets(json_root(doc), &targets, err, err_size) &&
        choose_artifact(targets, name, version, target_wanted, &artifact, err, err_size) &&
        read_artifact(artifact, name, version, out, err, err_size);

    json_free(doc);
    return ok;
}

bool resolve_read_release(const char *body, const char *name, const char *version,
                          resolved_dep *out, char *err, size_t err_size) {
    return resolve_read_release_for(body, name, version, NULL, out, err, err_size);
}

/* --- the newest release --- */

/* The versions in a catalogue listing: { kind, name, releases: [ {version} ] }. */
bool resolve_read_versions(const char *body, str_list *out, char *err, size_t err_size) {
    json_document *doc = json_parse(body);
    if(doc == NULL)
        return set_error(err, err_size, "the registry's answer was not JSON");

    const json_value releases = json_get(json_root(doc), "releases");
    bool ok = json_type_of(releases) == json_type_array;
    if(!ok)
        (void)set_error(err, err_size, "the registry's answer carried no 'releases' list");

    for(size_t i = 0; ok && i < json_count(releases); i++) {
        const char *version = json_string(json_get(json_at(releases, i), "version"));
        if(version != NULL && !str_list_push(out, version))
            ok = set_error(err, err_size, "out of memory reading the catalogue");
    }

    json_free(doc);
    return ok;
}

/* The catalogue listing for one name, whatever it holds. */
static bool fetch_listing(const char *base_url, const char *name, str_list *out, char *err,
                          size_t err_size) {
    char path[RESOLVE_PATH_MAX];
    if(!fs_format_path(path, sizeof path, "/v1/packages/%s", name))
        return set_error(err, err_size, "the registry path for %s is too long", name);

    registry_response response;
    if(!registry_get(base_url, path, &response, err, err_size))
        return false;
    if(response.status == 404)
        return set_error(err, err_size, "there is no package called '%s' in the registry", name);
    if(response.status != 200) {
        char detail[512];
        registry_explain(&response, detail, sizeof detail);
        return set_error(err, err_size, "the registry answered %ld for %s: %s", response.status,
                         name, detail);
    }

    if(!resolve_read_versions(response.body, out, err, err_size))
        return false;
    if(str_list_count(out) == 0)
        return set_error(err, err_size, "'%s' is in the registry with no releases", name);
    return true;
}

bool resolve_versions(const char *base_url, const char *name, str_list *out, char *err,
                      size_t err_size) {
    if(!fetch_listing(base_url, name, out, err, err_size))
        return false;

    /* Ordered here rather than by the registry, which serves versions as
       opaque strings in publication order (RFC-0010). What cannot be ordered
       is dropped: this list is what a proposal is chosen from. */
    const size_t ordered = semver_sort_desc(out);
    if(ordered == 0)
        return set_error(err, err_size, "'%s' has no release with a version molto can order", name);
    str_list_truncate(out, ordered);
    return true;
}

bool resolve_latest_version(const char *base_url, const char *name, char *out, size_t out_size,
                            char *err, size_t err_size) {
    str_list versions;
    str_list_init(&versions);

    bool ok = resolve_versions(base_url, name, &versions, err, err_size);
    if(ok) {
        const char *newest = str_list_get(&versions, 0);
        ok = snprintf(out, out_size, "%s", newest) > 0 && strlen(newest) < out_size;
        if(!ok)
            (void)set_error(err, err_size, "'%s' has a version too long to record", name);
    }
    str_list_free(&versions);
    return ok;
}

/* --- remembering an answer --- */

/* In the releases tree, which no fetch writes into and no fetch replaces. */
static bool release_path(const char *name, const char *version, char *out, size_t size) {
    char directory[RESOLVE_PATH_MAX];
    return source_cache_area_path(SOURCE_CACHE_RELEASES, name, version, ANY_TARGET, directory,
                                  sizeof directory) &&
           fs_format_path(out, size, "%s/%s", directory, RELEASE_FILE);
}

/* Where releases were kept before they had a tree of their own: beside the
   sources. Read but never written, so a cache filled by an older molto keeps
   answering instead of being silently refetched. */
static bool legacy_release_path(const char *name, const char *version, char *out, size_t size) {
    char directory[RESOLVE_PATH_MAX];
    return source_cache_path(name, version, ANY_TARGET, directory, sizeof directory) &&
           fs_format_path(out, size, "%s/%s", directory, RELEASE_FILE);
}

char *resolve_release_body(const char *name, const char *version) {
    char path[RESOLVE_PATH_MAX];
    if(release_path(name, version, path, sizeof path) && fs_path_exists(path))
        return fs_read_file(path);
    if(legacy_release_path(name, version, path, sizeof path) && fs_path_exists(path))
        return fs_read_file(path);
    return NULL;
}

bool resolve_remembered(const char *name, const char *version, resolved_dep *out) {
    char *body = resolve_release_body(name, version);
    if(body == NULL)
        return false;

    /* Read through exactly the path a live answer takes, so a remembered one
       cannot be accepted where a fresh one would have been refused. */
    char ignored[256] = "";
    const bool ok = resolve_read_release(body, name, version, out, ignored, sizeof ignored);
    free(body);
    return ok;
}

void resolve_remember(const char *name, const char *version, const char *body) {
    char path[RESOLVE_PATH_MAX];
    char directory[RESOLVE_PATH_MAX];
    if(!release_path(name, version, path, sizeof path))
        return;
    /* The directory is made here rather than assumed: the whole point of this
       tree is that it holds answers about coordinates nothing has fetched. */
    if(source_cache_area_path(SOURCE_CACHE_RELEASES, name, version, ANY_TARGET, directory,
                              sizeof directory) &&
       fs_make_dirs(directory))
        (void)fs_write_file(path, body);
}

bool resolve_version(const char *base_url, const char *name, const char *version, resolved_dep *out,
                     char *err, size_t err_size) {
    char path[RESOLVE_PATH_MAX];
    if(!fs_format_path(path, sizeof path, "/v1/packages/%s/%s", name, version))
        return set_error(err, err_size, "the registry path for %s %s is too long", name, version);

    registry_response response;
    if(!registry_get(base_url, path, &response, err, err_size))
        return false;

    if(response.status == 404)
        return set_error(err, err_size, "%s %s is not in the registry", name, version);
    if(response.status != 200) {
        char detail[512];
        registry_explain(&response, detail, sizeof detail);
        return set_error(err, err_size, "the registry answered %ld for %s %s: %s", response.status,
                         name, version, detail);
    }

    if(!resolve_read_release(response.body, name, version, out, err, err_size))
        return false;
    /* Remembered by the caller, after the fetch: writing it now would put it
       in a directory the fetch is about to replace. */
    snprintf(out->body, sizeof out->body, "%s", response.body);
    return true;
}
