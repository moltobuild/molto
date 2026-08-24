#include <molto/services/deps_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/recipe_service.h>
#include <molto/services/source_discovery.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPS_PATH_MAX 1024

/* An include search directory, as a command line carries it. */
#define INCLUDE_FLAG_FORMAT "-I%s"

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

void prepared_deps_init(prepared_deps *out) {
    out->units = NULL;
    out->unit_count = 0;
    str_list_init(&out->includes);
    str_list_init(&out->defines);
    str_list_init(&out->flags);
    str_list_init(&out->links);
}

void prepared_deps_free(prepared_deps *out) {
    for(size_t i = 0; i < out->unit_count; i++) {
        str_list_free(&out->units[i].sources);
        str_list_free(&out->units[i].includes);
        str_list_free(&out->units[i].defines);
        str_list_free(&out->units[i].flags);
        str_list_free(&out->units[i].exports.includes);
        str_list_free(&out->units[i].exports.defines);
        str_list_free(&out->units[i].exports.flags);
        str_list_free(&out->units[i].exports.links);
    }
    free(out->units);
    out->units = NULL;
    out->unit_count = 0;
    str_list_free(&out->includes);
    str_list_free(&out->defines);
    str_list_free(&out->flags);
    str_list_free(&out->links);
}

/* Room for one more dependency, initialised and named. Grown one at a time:
   a graph is bounded at a few dozen packages, and doubling would be arithmetic
   in exchange for nothing measurable. */
static prepared_unit *unit_open(prepared_deps *out, const dep_node *node, char *err,
                                size_t err_size) {
    prepared_unit *grown = realloc(out->units, (out->unit_count + 1) * sizeof *grown);
    if(grown == NULL) {
        (void)set_error(err, err_size, "out of memory collecting dependencies");
        return NULL;
    }
    out->units = grown;
    prepared_unit *unit = &out->units[out->unit_count++];
    snprintf(unit->name, sizeof unit->name, "%s", node->name);
    snprintf(unit->version, sizeof unit->version, "%s", node->version);
    unit->origin = dep_graph_source_kind(node->source);
    snprintf(unit->root, sizeof unit->root, "%s", node->root);
    /* Copied rather than composed: a standard is one value the recipe either
       named or did not, and an empty one means the consumer's applies. */
    snprintf(unit->std, sizeof unit->std, "%s", node->artifacts.std);
    snprintf(unit->cpp_std, sizeof unit->cpp_std, "%s", node->artifacts.cpp_std);
    str_list_init(&unit->sources);
    str_list_init(&unit->includes);
    str_list_init(&unit->defines);
    str_list_init(&unit->flags);
    str_list_init(&unit->exports.includes);
    str_list_init(&unit->exports.defines);
    str_list_init(&unit->exports.flags);
    str_list_init(&unit->exports.links);
    return unit;
}

/* Everything in `src` onto the end of `dest`. */
static bool append_list(str_list *dest, const str_list *src, char *err, size_t err_size) {
    for(size_t i = 0; i < str_list_count(src); i++) {
        if(!str_list_push(dest, str_list_get(src, i)))
            return set_error(err, err_size, "out of memory collecting dependencies");
    }
    return true;
}

/* One package's export onto the sum of all of them, in the order a compile line
   receives it: includes, defines, flags, then libraries. */
static bool append_interface(prepared_deps *out, const prepared_interface *exports, char *err,
                             size_t err_size) {
    return append_list(&out->includes, &exports->includes, err, err_size) &&
           append_list(&out->defines, &exports->defines, err, err_size) &&
           append_list(&out->flags, &exports->flags, err, err_size) &&
           append_list(&out->links, &exports->links, err, err_size);
}

/* --- collecting what a dependency contributes --- */

/* A path inside the fetched source, absolute, because the cache is not under
   the project root and the build anchors a relative path there. */
static bool push_rooted(str_list *out, const char *root, const char *relative, char *err,
                        size_t err_size) {
    char path[DEPS_PATH_MAX];
    if(!fs_format_path(path, sizeof path, "%s/%s", root, relative))
        return set_error(err, err_size, "the path '%s/%s' is too long", root, relative);
    if(!str_list_push(out, path))
        return set_error(err, err_size, "out of memory collecting dependencies");
    return true;
}

static bool push_all(str_list *out, const char list[][PROJECT_OPT_LEN], size_t count, char *err,
                     size_t err_size) {
    for(size_t i = 0; i < count; i++) {
        if(!str_list_push(out, list[i]))
            return set_error(err, err_size, "out of memory collecting dependencies");
    }
    return true;
}

/* Every source under `root` the recipe wants compiled.

   A recipe that names its sources is taken at its word. One that names none is
   asking for everything the drop contains, which has to be discovered — and
   filtered, because `exclude` exists precisely for the file that must not be
   compiled. */
static bool collect_sources(const recipe_artifacts *artifacts, const char *root, str_list *out,
                            char *err, size_t err_size) {
    if(artifacts->source_count > 0) {
        for(size_t i = 0; i < artifacts->source_count; i++) {
            if(!recipe_artifacts_wants(artifacts, artifacts->sources[i]))
                continue;
            char path[DEPS_PATH_MAX];
            if(!fs_format_path(path, sizeof path, "%s/%s", root, artifacts->sources[i]))
                return set_error(err, err_size, "the source path for '%s' is too long",
                                 artifacts->sources[i]);
            if(!fs_path_exists(path))
                return set_error(err, err_size,
                                 "the recipe names '%s', which the source does not "
                                 "contain",
                                 artifacts->sources[i]);
            /* A directory is not a translation unit. Left to pass through, it
               would reach the compiler as an input and be reported by the
               compiler — as an unused linker argument, which names neither the
               recipe nor the key that put it there. `sources` names files one
               by one on purpose: it fails closed, so a file added upstream
               tomorrow does not join the build by itself (RFC-0009). */
            if(fs_is_dir(path))
                return set_error(err, err_size,
                                 "the recipe names '%s', which is a directory; "
                                 "[artifacts].sources names files one by one — drop the key "
                                 "to compile everything the source contains",
                                 artifacts->sources[i]);
            if(!str_list_push(out, path))
                return set_error(err, err_size, "out of memory collecting dependencies");
        }
        return true;
    }

    str_list found;
    str_list_init(&found);
    if(!source_discovery_collect(root, &found)) {
        str_list_free(&found);
        return set_error(err, err_size, "could not read the fetched source at '%s'", root);
    }

    bool ok = true;
    for(size_t i = 0; ok && i < str_list_count(&found); i++) {
        const char *path = str_list_get(&found, i);
        const char *name = path + strlen(root) + 1; /* what `exclude` names */
        if(recipe_artifacts_wants(artifacts, name) && !str_list_push(out, path))
            ok = set_error(err, err_size, "out of memory collecting dependencies");
    }
    str_list_free(&found);
    return ok;
}

/* One package's options onto three lists: its include directories anchored at
   its own root — never at the root of whoever is compiling — and its defines
   and flags as the recipe wrote them. */
static bool push_options(const project_options *options, const char *root, str_list *includes,
                         str_list *defines, str_list *flags, char *err, size_t err_size) {
    for(size_t i = 0; i < options->include_count; i++) {
        if(!push_rooted(includes, root, options->include[i], err, err_size))
            return false;
    }
    return push_all(defines, options->defines, options->define_count, err, err_size) &&
           push_all(flags, options->flags, options->flag_count, err, err_size);
}

/* What one package's own sources are compiled against.
 *
 * Nearest last: the interface of everything it reaches, then its own
 * interface, then its private table. A compiler reading a repeated flag takes
 * the last one, so the most specific statement about a package has to be the
 * one it sees last — and the most specific statement about a package is the
 * one the package itself made.
 *
 * A sibling it does not reach contributes nothing. That is the whole point:
 * two dependencies that know nothing of each other no longer share a
 * preprocessor. */
static bool collect_unit(const dep_graph *graph, const dep_node *node, prepared_unit *unit,
                         char *err, size_t err_size) {
    str_list reached;
    str_list_init(&reached);
    if(!dep_graph_closure(graph, node->name, &reached)) {
        str_list_free(&reached);
        return set_error(err, err_size, "out of memory collecting dependencies");
    }

    bool ok = true;
    for(size_t i = 0; ok && i < str_list_count(&reached); i++) {
        const dep_node *other = dep_graph_find(graph, str_list_get(&reached, i));
        if(other != NULL)
            ok = push_options(&other->artifacts.options, other->root, &unit->includes,
                              &unit->defines, &unit->flags, err, err_size);
    }
    str_list_free(&reached);

    return ok &&
           push_options(&node->artifacts.options, node->root, &unit->includes, &unit->defines,
                        &unit->flags, err, err_size) &&
           push_options(&node->artifacts.private_options, node->root, &unit->includes,
                        &unit->defines, &unit->flags, err, err_size);
}

static bool collect(const dep_graph *graph, const dep_node *node, prepared_deps *out, char *err,
                    size_t err_size) {
    const recipe_artifacts *artifacts = &node->artifacts;
    /* Only a source drop contributes translation units. A static or shared
       artifact is already built, and molto cannot consume one yet. */
    if(artifacts->type != recipe_artifact_source)
        return set_error(err, err_size,
                         "this dependency is published as a built library, and molto can only "
                         "consume [artifacts] type = \"source\" yet");

    prepared_unit *unit = unit_open(out, node, err, err_size);
    if(unit == NULL)
        return false;

    /* The interface, which the consumer compiles and links against. The private
       table is deliberately absent: it never leaves the package.
     *
     * Recorded against the package first and added to the sum second, from one
     * pass over one table — the sum is exactly the concatenation of the
     * packages' exports, and writing it that way is what stops the two from
     * ever disagreeing about what a dependency asked for. */
    if(!push_options(&artifacts->options, node->root, &unit->exports.includes,
                     &unit->exports.defines, &unit->exports.flags, err, err_size))
        return false;
    for(size_t i = 0; i < artifacts->link_count; i++) {
        if(!str_list_push(&unit->exports.links, artifacts->link[i]))
            return set_error(err, err_size, "out of memory collecting dependencies");
    }
    if(!append_interface(out, &unit->exports, err, err_size))
        return false;

    return collect_sources(artifacts, node->root, &unit->sources, err, err_size) &&
           collect_unit(graph, node, unit, err, err_size);
}

/* --- the whole graph --- */

/* Nodes come out sorted by name, so the flags a build receives are the same on
   every machine. That is the only ordering guarantee made here: `-l` entries
   name system libraries, which the platform resolves, and nothing is a built
   library whose link order could matter yet. */
static bool collect_scope(const dep_graph *graph, unsigned wanted, unsigned without,
                          prepared_deps *out, char *err, size_t err_size) {
    for(size_t i = 0; i < dep_graph_count(graph); i++) {
        const dep_node *node = dep_graph_at(graph, i);
        if((node->scope & wanted) == 0 || (node->scope & without) != 0)
            continue;
        char reason[512] = "";
        if(!collect(graph, node, out, reason, sizeof reason))
            return set_error(err, err_size, "dependency '%s': %s", node->name, reason);
    }
    return true;
}

bool deps_prepare_graph(const dep_graph *graph, prepared_deps *out, char *err, size_t err_size) {
    return collect_scope(graph, dep_scope_runtime, 0, out, err, err_size);
}

bool deps_prepare_dev(const dep_graph *graph, prepared_deps *out, char *err, size_t err_size) {
    /* Only what the test build *adds*. A package required by both tables is
       already in the runtime set, and handing it over twice would put its
       sources on the test link line twice — which is a duplicate symbol, the
       exact failure the one-version rule exists to prevent. */
    return collect_scope(graph, dep_scope_dev, dep_scope_runtime, out, err, err_size);
}

bool deps_prepare(const project_ctx *ctx, prepared_deps *out, char *err, size_t err_size) {
    if(ctx->deps.count == 0)
        return true;

    dep_graph *graph = NULL;
    if(!dep_graph_resolve(ctx, &graph, err, err_size))
        return false;

    const bool ok = deps_prepare_graph(graph, out, err, err_size);
    dep_graph_free(graph);
    return ok;
}

/* --- what a command line makes of them --- */

/* Append one entry to a fixed-size option array, refusing to overflow it: a
   dropped define or library produces a green build of something else. */
bool deps_append_option(char dest[][PROJECT_OPT_LEN], size_t *count, size_t capacity,
                        const char *value, const char *what) {
    if(*count >= capacity) {
        fprintf(stderr, "molto: %s has more than %zu entries once dependencies are added\n", what,
                capacity);
        return false;
    }
    if(!fs_format_path(dest[*count], PROJECT_OPT_LEN, "%s", value))
        return fs_report_long_path(value);
    (*count)++;
    return true;
}

/* The "-I" flags a compile line carries: the project's own src/, then one per
   include directory a dependency exports.

   Composed into a list rather than merged into project_options because a
   dependency's include is an absolute path into the shared cache, and a
   manifest option is sized for "-DFOO=1" — RFC-0003 caps one at 95 characters,
   which a real cache path exceeds. */
bool deps_include_flags(const char *src_dir, const prepared_deps *deps, str_list *out) {
    const str_list *dep_includes = deps != NULL ? &deps->includes : NULL;
    char flag[DEPS_PATH_MAX + 4];
    if(!fs_format_path(flag, sizeof flag, INCLUDE_FLAG_FORMAT, src_dir))
        return fs_report_long_path(src_dir);
    if(!str_list_push(out, flag))
        return false;

    for(size_t i = 0; dep_includes != NULL && i < str_list_count(dep_includes); i++) {
        const char *directory = str_list_get(dep_includes, i);
        if(!fs_format_path(flag, sizeof flag, INCLUDE_FLAG_FORMAT, directory))
            return fs_report_long_path(directory);
        if(!str_list_push(out, flag))
            return false;
    }
    return true;
}

/* Fold what the dependencies contribute into `[target]`.
 *
 * A dependency's include directories, defines, flags and libraries are exactly
 * the things `[target]` already carries, and everything downstream — the
 * compile line, the link line, `molto lint`, the test build — reads them from
 * there. Merging here means none of those has to learn what a dependency is.
 */
bool deps_merge_interface(project_ctx *ctx, const prepared_deps *deps) {
    /* The package name and version live past the manifest's own limit, so a
       dependency may not take their slots. */
    for(size_t i = 0; i < str_list_count(&deps->defines); i++) {
        if(!deps_append_option(ctx->target.options.defines, &ctx->target.options.define_count,
                               PROJECT_MAX_OPTS + PROJECT_PKG_DEFINES,
                               str_list_get(&deps->defines, i), "[target].defines"))
            return false;
    }
    for(size_t i = 0; i < str_list_count(&deps->flags); i++) {
        if(!deps_append_option(ctx->target.options.flags, &ctx->target.options.flag_count,
                               PROJECT_MAX_OPTS, str_list_get(&deps->flags, i), "[target].flags"))
            return false;
    }
    for(size_t i = 0; i < str_list_count(&deps->links); i++) {
        const char *library = str_list_get(&deps->links, i);
        if(ctx->target.link_count >= PROJECT_MAX_LINK) {
            fprintf(stderr,
                    "molto: [target].link has more than %d entries once dependencies are "
                    "added\n",
                    PROJECT_MAX_LINK);
            return false;
        }
        if(!fs_format_path(ctx->target.link[ctx->target.link_count], PROJECT_LINK_NAME_MAX, "%s",
                           library))
            return fs_report_long_path(library);
        ctx->target.link_count++;
    }
    return true;
}
