#include <molto/services/ir_service.h>

#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * What the engine refuses to lower (RFC-0013).
 *
 * Permissions govern a plugin's process; they do not govern the document it
 * returns, and the document is executed by Molto, as the user, with the user's
 * privileges. A plugin denied the network can still return a compile option
 * that loads a shared object into the compiler, and a plugin with no filesystem
 * access can still return an include path of `/`. The sandbox decides what a
 * plugin can touch; this decides what Molto will do on its behalf, and a design
 * with only the first has neither.
 *
 * Nothing here consults a registry, a manifest or a permission list. A document
 * is refused on what it says, which is the only thing available at the moment
 * it matters — before any of it becomes a command.
 */

#define IR_ERR(err, size, ...)                                                                     \
    do {                                                                                           \
        if((err) != NULL && (size) > 0)                                                            \
            snprintf((err), (size), __VA_ARGS__);                                                  \
    } while(0)

#define IR_PATH_MAX 4096

/* --- paths --- */

/* Collapse `.` and `..` without touching the filesystem, so that a path naming
   a file that does not exist yet — an artifact under build/ — is still
   answerable. A trailing slash is dropped; the result never has one.

   Lexical on purpose and not only for convenience: `realpath` on a
   not-yet-created output would fail, and a check that can only run on existing
   files is a check a producer can dodge by naming something new. The symlink
   half of the rule is handled separately below, where a file that does exist
   gets resolved as well. */
#define IR_MAX_SEGMENTS 512

/* Append one segment to `out`, with a separator when one is needed. */
static bool put_segment(char *out, size_t size, size_t *used, const char *text, size_t length) {
    if(*used > 0 && out[*used - 1] != '/') {
        if(*used + 1 >= size)
            return false;
        out[(*used)++] = '/';
    }
    if(*used + length >= size)
        return false;
    memcpy(out + *used, text, length);
    *used += length;
    return true;
}

static bool normalise(const char *path, char *out, size_t size) {
    if(path == NULL || path[0] == '\0' || size < 2)
        return false;

    const bool absolute = fs_path_is_absolute(path);
    const char *start[IR_MAX_SEGMENTS];
    size_t length[IR_MAX_SEGMENTS];
    size_t count = 0;
    /* `..` that could not be cancelled against a segment. An absolute path
       cannot climb above the root, so these only accumulate on a relative one —
       and they are kept rather than dropped, so that anchoring it later still
       walks out and the containment check sees the escape instead of a
       normalisation that hid it. */
    size_t climbs = 0;

    for(const char *cursor = path; *cursor != '\0';) {
        while(*cursor == '/')
            cursor++;
        const size_t n = strcspn(cursor, "/");
        if(n == 0)
            break;

        if(n == 1 && cursor[0] == '.') {
            cursor += n;
            continue;
        }
        if(n == 2 && cursor[0] == '.' && cursor[1] == '.') {
            if(count > 0)
                count--;
            else if(!absolute)
                climbs++;
            cursor += n;
            continue;
        }
        if(count >= IR_MAX_SEGMENTS)
            return false;
        start[count] = cursor;
        length[count] = n;
        count++;
        cursor += n;
    }

    size_t used = 0;
    if(absolute)
        out[used++] = '/';
    for(size_t i = 0; i < climbs; i++) {
        if(!put_segment(out, size, &used, "..", 2))
            return false;
    }
    for(size_t i = 0; i < count; i++) {
        if(!put_segment(out, size, &used, start[i], length[i]))
            return false;
    }
    if(used == 0)
        out[used++] = '.';
    out[used] = '\0';
    return true;
}

/* True when `path` is `root` or sits under it. Compared segment-wise, so
   `/home/u/proj-evil` is not inside `/home/u/proj`. Both are expected already
   normalised. */
static bool inside(const char *path, const char *root) {
    if(root == NULL || root[0] == '\0')
        return false;
    const size_t length = strlen(root);
    if(strncmp(path, root, length) != 0)
        return false;
    if(path[length] == '\0')
        return true;
    /* A root of "/" already ends in its separator. */
    return path[length] == '/' || root[length - 1] == '/';
}

/* Anchor `path` at `base` when it is relative, then normalise it. */
static bool anchor(const char *path, const char *base, char *out, size_t size) {
    if(path == NULL || path[0] == '\0')
        return false;
    if(fs_path_is_absolute(path))
        return normalise(path, out, size);

    char joined[IR_PATH_MAX];
    const int written = snprintf(joined, sizeof joined, "%s/%s", base == NULL ? "." : base, path);
    if(written < 0 || (size_t)written >= sizeof joined)
        return false;
    return normalise(joined, out, size);
}

/* One bound in the two forms the rule needs. */
typedef struct {
    char path[IR_PATH_MAX];
    char real[IR_PATH_MAX];
} bound_root;

/* Every bound twice over: as written, and with every symlink resolved.
 *
 * Two forms because the rule has two halves and one form cannot answer both. A
 * lexical comparison catches `..` and an absolute prefix, and it is the only one
 * available for a file that does not exist yet. A symlink escapes lexically
 * undetected and needs the filesystem — but a bound may itself sit behind a
 * link, so the resolved path has to be compared against resolved bounds or a
 * project under a symlinked home is rejected for existing.
 *
 * `roots` is heap-allocated because there is one per resolved package and a
 * project may depend on many: a fixed array here would be a cap on a document a
 * machine produced, which RFC-0013 refuses. */
typedef struct {
    char workspace[IR_PATH_MAX];
    char build_dir[IR_PATH_MAX];
    char cache[IR_PATH_MAX];
    bool has_cache;
    char real_workspace[IR_PATH_MAX];
    char real_build_dir[IR_PATH_MAX];
    char real_cache[IR_PATH_MAX];
    bound_root *roots;
    size_t root_count;
} bounds_state;

/* Resolve every symlink in `path`, or fall back to `path` itself when it does
   not exist — a bound that is not there yet still bounds what may be written
   under it. */
static bool resolve_links(const char *path, char *out, size_t size) {
    if(fs_real_path(path, out, size))
        return true;
    const int written = snprintf(out, size, "%s", path);
    return written >= 0 && (size_t)written < size;
}

/* The directories the caller authorised, in both forms. An empty list is a
   document validated before anything was resolved, which is every document a
   frontend returns. */
static bool prepare_roots(const ir_bounds *bounds, bounds_state *out) {
    if(bounds->roots == NULL || bounds->root_count == 0)
        return true;

    out->roots = calloc(bounds->root_count, sizeof *out->roots);
    if(out->roots == NULL)
        return false;
    out->root_count = bounds->root_count;

    for(size_t i = 0; i < out->root_count; i++) {
        const char *root = bounds->roots[i];
        if(root == NULL || root[0] == '\0' ||
           !normalise(root, out->roots[i].path, sizeof out->roots[i].path) ||
           !resolve_links(out->roots[i].path, out->roots[i].real, sizeof out->roots[i].real))
            return false;
    }
    return true;
}

static void bounds_release(bounds_state *state) {
    free(state->roots);
    state->roots = NULL;
    state->root_count = 0;
}

static bool bounds_prepare(const ir_bounds *bounds, bounds_state *out) {
    memset(out, 0, sizeof *out);
    if(!normalise(bounds->workspace, out->workspace, sizeof out->workspace) ||
       !normalise(bounds->build_dir, out->build_dir, sizeof out->build_dir))
        return false;
    out->has_cache = bounds->cache != NULL && bounds->cache[0] != '\0';
    if(out->has_cache && !normalise(bounds->cache, out->cache, sizeof out->cache))
        return false;

    return resolve_links(out->workspace, out->real_workspace, sizeof out->real_workspace) &&
           resolve_links(out->build_dir, out->real_build_dir, sizeof out->real_build_dir) &&
           (!out->has_cache ||
            resolve_links(out->cache, out->real_cache, sizeof out->real_cache)) &&
           prepare_roots(bounds, out);
}

static bool within_bounds(const char *resolved, const bounds_state *bounds) {
    if(inside(resolved, bounds->workspace) || inside(resolved, bounds->build_dir) ||
       (bounds->has_cache && inside(resolved, bounds->cache)))
        return true;
    for(size_t i = 0; i < bounds->root_count; i++) {
        if(inside(resolved, bounds->roots[i].path))
            return true;
    }
    return false;
}

/* The symlink half. A path that does not exist has nothing to follow, and its
   lexical answer is the whole answer. */
static bool within_bounds_through_links(const char *resolved, const bounds_state *bounds) {
    char real[IR_PATH_MAX];
    if(!fs_real_path(resolved, real, sizeof real))
        return true;

    bool ok = inside(real, bounds->real_workspace) || inside(real, bounds->real_build_dir) ||
              (bounds->has_cache && inside(real, bounds->real_cache));
    for(size_t i = 0; !ok && i < bounds->root_count; i++)
        ok = inside(real, bounds->roots[i].real);
    return ok;
}

/* The one path rule: resolved, it is the workspace, the build directory, the
   global cache or a root the caller authorised, or somewhere under one of them
   — by `..`, by an absolute prefix or through a symlink alike.

   `what` and `where` name the node so a rejection is a diagnostic rather than a
   complaint. */
static bool path_allowed(const char *path, const char *base, const bounds_state *bounds,
                         const char *what, const char *where, char *err, size_t err_size) {
    char resolved[IR_PATH_MAX];
    if(!anchor(path, base, resolved, sizeof resolved)) {
        IR_ERR(err, err_size, "%s of %s is not a path this can resolve: '%s'", what, where, path);
        return false;
    }

    if(!within_bounds(resolved, bounds)) {
        IR_ERR(err, err_size,
               "%s of %s resolves to '%s', which is outside the workspace, the build directory, "
               "the cache and every dependency this build resolved",
               what, where, resolved);
        return false;
    }
    if(!within_bounds_through_links(resolved, bounds)) {
        IR_ERR(err, err_size, "%s of %s is inside the workspace but links out of it: '%s'", what,
               where, resolved);
        return false;
    }
    return true;
}

/* --- options a plugin may not name --- */

typedef enum {
    match_exact,
    match_prefix,
    match_contains,
} match_kind;

typedef struct {
    const char *pattern;
    match_kind kind;
    const char *why;
} forbidden_option;

/* Refused only in a document whose origin is a plugin.
 *
 * The asymmetry with `Project.toml` is deliberate and it is not a statement
 * about trust: a manifest is a file in the user's repository, which they wrote,
 * which their reviewer read and their version control records, and a plugin's
 * document is generated on the fly by a binary fetched from a registry. Those
 * two deserve different scrutiny even when the second is entirely well-behaved,
 * and the day they do not is the day the first one stopped being reviewable. */
static const forbidden_option FORBIDDEN[] = {
    {"-fplugin", match_prefix,
     "it loads code into the compiler, which is a second extension mechanism arrived at sideways "
     "with none of RFC-0013's rules"},
    {"-load", match_contains,
     "it loads code into the compiler, which is a second extension mechanism arrived at sideways "
     "with none of RFC-0013's rules"},
    {"-Xclang", match_prefix,
     "it passes options straight through to a compiler frontend, past "
     "every rule that applies to the options themselves"},
    {"-B", match_prefix,
     "it redirects the toolchain, and the toolchain is pickup's answer rather than a frontend's "
     "opinion (RFC-0003)"},
    {"--sysroot", match_prefix, "it redirects the toolchain (RFC-0003)"},
    {"-isysroot", match_prefix, "it redirects the toolchain (RFC-0003)"},
    {"-fuse-ld", match_prefix, "it redirects the linker (RFC-0003)"},
    {"-o", match_exact,
     "the engine composes output paths; a producer naming one is describing where its object goes, "
     "which is not its decision"},
    {"--output", match_prefix, "the engine composes output paths, not a producer"},
};

static bool matches(const forbidden_option *rule, const char *value) {
    switch(rule->kind) {
    case match_exact:
        return strcmp(value, rule->pattern) == 0;
    case match_prefix:
        return strncmp(value, rule->pattern, strlen(rule->pattern)) == 0;
    case match_contains:
        return strstr(value, rule->pattern) != NULL;
    }
    return false;
}

static bool options_allowed(const ir_option *options, size_t count, const char *where, char *err,
                            size_t err_size) {
    for(size_t i = 0; i < count; i++) {
        const char *value = options[i].value;
        if(value == NULL)
            continue;
        for(size_t k = 0; k < sizeof FORBIDDEN / sizeof FORBIDDEN[0]; k++) {
            if(matches(&FORBIDDEN[k], value)) {
                IR_ERR(err, err_size, "%s names the option '%s', which a plugin may not: %s", where,
                       value, FORBIDDEN[k].why);
                return false;
            }
        }
    }
    return true;
}

/* --- the target graph --- */

static const ir_target *find_target(const ir_document *doc, const char *name) {
    for(size_t i = 0; i < doc->target_count; i++) {
        if(doc->targets[i].name != NULL && strcmp(doc->targets[i].name, name) == 0)
            return &doc->targets[i];
    }
    return NULL;
}

static bool names_are_unique(const ir_document *doc, char *err, size_t err_size) {
    for(size_t i = 0; i < doc->target_count; i++) {
        for(size_t k = i + 1; k < doc->target_count; k++) {
            if(doc->targets[i].name != NULL && doc->targets[k].name != NULL &&
               strcmp(doc->targets[i].name, doc->targets[k].name) == 0) {
                IR_ERR(err, err_size, "two targets are named '%s', and a name has to say which one",
                       doc->targets[i].name);
                return false;
            }
        }
    }
    return true;
}

typedef enum {
    mark_none,
    mark_open,
    mark_done,
} visit_mark;

/* Depth-first, so a cycle is reported at the edge that closes it rather than as
   a deadlock discovered in the scheduler. */
static bool acyclic_from(const ir_document *doc, size_t index, visit_mark *marks, char *err,
                         size_t err_size) {
    if(marks[index] == mark_done)
        return true;
    if(marks[index] == mark_open) {
        IR_ERR(err, err_size, "target '%s' depends on itself, directly or through others",
               doc->targets[index].name);
        return false;
    }

    marks[index] = mark_open;
    const ir_target *target = &doc->targets[index];
    for(size_t i = 0; i < str_list_count(&target->depends_on); i++) {
        const char *name = str_list_get(&target->depends_on, i);
        const ir_target *next = find_target(doc, name);
        if(next == NULL) {
            IR_ERR(err, err_size,
                   "target '%s' depends on '%s', which this document does not "
                   "declare",
                   target->name, name);
            return false;
        }
        if(!acyclic_from(doc, (size_t)(next - doc->targets), marks, err, err_size))
            return false;
    }
    marks[index] = mark_done;
    return true;
}

static bool graph_is_sound(const ir_document *doc, char *err, size_t err_size) {
    if(!names_are_unique(doc, err, err_size))
        return false;
    if(doc->target_count == 0)
        return true;

    visit_mark *marks = calloc(doc->target_count, sizeof *marks);
    if(marks == NULL) {
        IR_ERR(err, err_size, "out of memory checking the target graph");
        return false;
    }

    bool ok = true;
    for(size_t i = 0; ok && i < doc->target_count; i++)
        ok = acyclic_from(doc, i, marks, err, err_size);

    free(marks);
    return ok;
}

/* --- per-node checks --- */

/* The dependency a target names, or NULL when it names none. */
static const ir_dependency *package_of(const ir_document *doc, const ir_target *target) {
    if(target->package == NULL)
        return NULL;
    for(size_t i = 0; i < doc->dependency_count; i++) {
        if(doc->dependencies[i].name != NULL &&
           strcmp(doc->dependencies[i].name, target->package) == 0)
            return &doc->dependencies[i];
    }
    return NULL;
}

static bool target_is_allowed(const ir_document *doc, const ir_target *target,
                              const bounds_state *bounds, bool from_plugin, char *err,
                              size_t err_size) {
    char where[512];
    snprintf(where, sizeof where, "target '%s'", target->name);

    /* A target that names a package has its paths relative to that package's
       root rather than to the project's, because a dependency's bytes live in
       the shared cache and a path relative to the project could not reach them
       without climbing out of it. An unresolvable name is an error rather than
       a fallback to the project root: falling back would anchor a dependency's
       sources at the wrong place and look like a missing file much later. */
    const ir_dependency *package = package_of(doc, target);
    if(target->package != NULL && package == NULL) {
        IR_ERR(err, err_size, "%s belongs to package '%s', which this document does not describe",
               where, target->package);
        return false;
    }
    const char *base = package != NULL ? package->root : doc->root;

    for(size_t i = 0; i < target->source_count; i++) {
        if(!path_allowed(target->sources[i].path, base, bounds, "a source", where, err, err_size))
            return false;
        if(from_plugin && !options_allowed(target->sources[i].options,
                                           target->sources[i].option_count, where, err, err_size))
            return false;
    }

    for(size_t i = 0; i < target->include_count; i++) {
        if(!path_allowed(target->includes[i].value, base, bounds, "an include path", where, err,
                         err_size))
            return false;
    }

    /* An artifact's path is relative to the build directory, and every output
       stays inside it: a target writing into src/ is editing the user's code as
       a side effect of a build. */
    if(target->has_artifact) {
        char resolved[IR_PATH_MAX];
        if(!anchor(target->artifact.path, bounds->build_dir, resolved, sizeof resolved) ||
           !inside(resolved, bounds->build_dir)) {
            IR_ERR(err, err_size, "the artifact of %s resolves outside the build directory: '%s'",
                   where, target->artifact.path);
            return false;
        }
    }

    return !from_plugin ||
           (options_allowed(target->options, target->option_count, where, err, err_size) &&
            options_allowed(target->links, target->link_count, where, err, err_size));
}

static bool dependency_is_allowed(const ir_dependency *dep, const bounds_state *bounds,
                                  bool from_plugin, char *err, size_t err_size) {
    char where[512];
    snprintf(where, sizeof where, "dependency '%s'", dep->name);

    if(!path_allowed(dep->root, NULL, bounds, "the root", where, err, err_size))
        return false;

    for(size_t i = 0; i < dep->include_count; i++) {
        if(!path_allowed(dep->includes[i].value, dep->root, bounds, "an include path", where, err,
                         err_size))
            return false;
    }

    return !from_plugin ||
           (options_allowed(dep->options, dep->option_count, where, err, err_size) &&
            options_allowed(dep->links, dep->link_count, where, err, err_size));
}

/* --- the whole document --- */

bool ir_validate(const ir_document *doc, const ir_bounds *bounds, char *err, size_t err_size) {
    if(doc == NULL || bounds == NULL || bounds->workspace == NULL || bounds->build_dir == NULL) {
        IR_ERR(err, err_size, "there is nothing to validate against");
        return false;
    }
    if(doc->schema != IR_SCHEMA) {
        IR_ERR(err, err_size, "the document is IR schema %ld and this molto speaks schema %d",
               doc->schema, IR_SCHEMA);
        return false;
    }
    if(doc->name == NULL || doc->root == NULL || doc->origin == NULL) {
        IR_ERR(err, err_size, "the document names no project");
        return false;
    }

    /* Heap-allocated because the two forms of three paths are more stack than a
       validation deserves, and because it is prepared once for a whole
       document rather than per node. */
    bounds_state *state = malloc(sizeof *state);
    if(state == NULL) {
        IR_ERR(err, err_size, "out of memory validating the document");
        return false;
    }
    if(!bounds_prepare(bounds, state)) {
        IR_ERR(err, err_size,
               "the workspace, build directory, cache or a resolved dependency is not a path this "
               "can resolve");
        /* It may have failed after allocating the roots, so the release runs on
           the failure path too. */
        bounds_release(state);
        free(state);
        return false;
    }

    const bool from_plugin = ir_is_from_plugin(doc);
    bool ok = path_allowed(doc->root, NULL, state, "the root", "the project", err, err_size) &&
              graph_is_sound(doc, err, err_size);

    for(size_t i = 0; ok && i < doc->target_count; i++)
        ok = target_is_allowed(doc, &doc->targets[i], state, from_plugin, err, err_size);
    for(size_t i = 0; ok && i < doc->dependency_count; i++)
        ok = dependency_is_allowed(&doc->dependencies[i], state, from_plugin, err, err_size);

    bounds_release(state);
    free(state);
    return ok;
}
