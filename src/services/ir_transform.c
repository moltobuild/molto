#include <molto/services/ir_transform.h>

#include <molto/services/source_discovery.h>

#include <stdio.h>
#include <string.h>

/* How a resolved package's origin is spelled in a document.
 *
 * Two enumerations for one idea, and they are kept apart on purpose: one is how
 * a manifest said where to get something, the other is a document's vocabulary,
 * and a document's vocabulary is a wire format that outlives whatever the
 * manifest happens to spell today. Translating in one place is what keeps the
 * two from being assumed equal by a cast. */
static ir_dep_origin origin_of(dep_source source) {
    switch(source) {
    case dep_source_git:
        return ir_dep_git;
    case dep_source_path:
        return ir_dep_path;
    case dep_source_archive:
        return ir_dep_archive;
    case dep_source_version:
        break;
    }
    return ir_dep_registry;
}

static bool push_options(ir_option **array, size_t *count, const str_list *values) {
    for(size_t i = 0; i < str_list_count(values); i++) {
        /* Target scope: what a dependency exports applies to everything that
           compiles against it, and there is no narrower thing for it to apply
           to. The unit scope belongs to a translation unit, and a dependency
           does not know which of them will include its headers. */
        if(!ir_add_option(array, count, str_list_get(values, i), ir_scope_target))
            return false;
    }
    return true;
}

/* A define reaches the document as the flag it already is.
 *
 * `prepared_deps` keeps them bare — "FOO=1" — because that is what a recipe
 * wrote and what `append_option` stores. The document carries options as they
 * reach a command line, which is what lets a consumer read one without knowing
 * which table it came from, so the prefix goes on here. */
static bool push_defines(ir_option **array, size_t *count, const str_list *values) {
    for(size_t i = 0; i < str_list_count(values); i++) {
        /* Bounded by what a manifest or a recipe can hold, which is where every
           define here came from. */
        char flag[PROJECT_OPT_LEN + 4];
        const int written = snprintf(flag, sizeof flag, "-D%s", str_list_get(values, i));
        if(written < 0 || (size_t)written >= sizeof flag)
            return false;
        if(!ir_add_option(array, count, flag, ir_scope_target))
            return false;
    }
    return true;
}

static bool push_includes(ir_include **array, size_t *count, const str_list *values) {
    for(size_t i = 0; i < str_list_count(values); i++) {
        /* Not `system`, and that is a decision rather than an omission: -isystem
           suppresses warnings in the headers it names, and Molto has never
           passed it for a dependency. Saying `system` here would silence
           diagnostics no build has silenced before, from a document nobody
           asked to change. */
        if(!ir_add_include(array, count, str_list_get(values, i), ir_scope_target, false))
            return false;
    }
    return true;
}

/* Every package of one set, each node tagged with the scope that set is. */
static bool describe_all(ir_document *doc, const prepared_deps *deps, ir_dep_scope scope, char *err,
                         size_t err_size) {
    if(deps == NULL)
        return true;

    for(size_t i = 0; i < deps->unit_count; i++) {
        const prepared_unit *unit = &deps->units[i];
        /* A path dependency carries no version — its bytes are whatever is on
           disk — and the node says so by having none rather than by inventing
           one (RFC-0013). */
        const char *version = unit->version[0] != '\0' ? unit->version : NULL;
        ir_dependency *dep =
            ir_add_dependency(doc, unit->name, version, origin_of(unit->origin), scope, unit->root);
        if(dep == NULL ||
           !push_includes(&dep->includes, &dep->include_count, &unit->exports.includes) ||
           !push_defines(&dep->options, &dep->option_count, &unit->exports.defines) ||
           !push_options(&dep->options, &dep->option_count, &unit->exports.flags) ||
           !push_options(&dep->links, &dep->link_count, &unit->exports.links)) {
            snprintf(err, err_size, "out of memory describing dependency '%s'", unit->name);
            return false;
        }
    }
    return true;
}

bool ir_transform_dependencies(ir_document *doc, const prepared_deps *deps,
                               const prepared_deps *dev, char *err, size_t err_size) {
    if(doc == NULL)
        return true; /* nothing to say */

    /* Runtime first and development second, so the fold below reads them in the
       order a command line receives them by walking the array once. */
    return describe_all(doc, deps, ir_dep_scope_runtime, err, err_size) &&
           describe_all(doc, dev, ir_dep_scope_dev, err, err_size);
}

/* --- folding what they export into what compiles against them --- */

/* Copy `count` options onto the end of another array, scope and all. */
static bool copy_options(ir_option **array, size_t *count, const ir_option *from,
                         size_t from_count) {
    for(size_t i = 0; i < from_count; i++) {
        if(!ir_add_option(array, count, from[i].value, from[i].scope))
            return false;
    }
    return true;
}

static bool copy_includes(ir_include **array, size_t *count, const ir_include *from,
                          size_t from_count) {
    for(size_t i = 0; i < from_count; i++) {
        if(!ir_add_include(array, count, from[i].value, from[i].scope, from[i].system))
            return false;
    }
    return true;
}

/* One dependency's interface onto one target, in the order a command line
   receives it: options — defines then flags, as the node already holds them —
   then includes, then libraries. Appended after what the target already
   carried, which is where the build has always put them. */
static bool fold_one(ir_target *target, const ir_dependency *dep) {
    return copy_options(&target->options, &target->option_count, dep->options, dep->option_count) &&
           copy_includes(&target->includes, &target->include_count, dep->includes,
                         dep->include_count) &&
           copy_options(&target->links, &target->link_count, dep->links, dep->link_count);
}

/* Whether a dependency at this scope reaches this target. A development
   dependency reaches the test targets and no others, which is what makes the
   separation real rather than documented (RFC-0008).
 *
 * A target that belongs to a package receives nothing. It is compiled against
 * its own recipe and not against what the consumer resolved — not the
 * consumer's other dependencies, and not itself. That is what makes one package
 * compile identically in every project that depends on it, which is what lets
 * an object be shared between them. Stating it here rather than relying on this
 * transform running first keeps it true whatever order the list ends up in. */
static bool reaches(ir_dep_scope scope, const ir_target *target) {
    if(target->package != NULL)
        return false;
    return scope == ir_dep_scope_runtime || target->kind == ir_target_test;
}

bool ir_transform_fold_dependencies(ir_document *doc, char *err, size_t err_size) {
    if(doc == NULL)
        return true;

    for(size_t t = 0; t < doc->target_count; t++) {
        ir_target *target = &doc->targets[t];
        for(size_t i = 0; i < doc->dependency_count; i++) {
            const ir_dependency *dep = &doc->dependencies[i];
            if(!reaches(dep->scope, target))
                continue;
            if(!fold_one(target, dep)) {
                snprintf(err, err_size, "out of memory folding '%s' into target '%s'",
                         dep->name != NULL ? dep->name : "?",
                         target->name != NULL ? target->name : "?");
                return false;
            }
        }
    }
    return true;
}

/* --- a dependency's own sources as targets --- */

/* `path` with `root` and its separator taken off the front. False when the path
   is not under the root, which `prepared_unit` promises it is. */
static bool relative_to(const char *path, const char *root, const char **out) {
    const size_t length = strlen(root);
    if(length == 0 || strncmp(path, root, length) != 0 || path[length] != '/')
        return false;
    *out = path + length + 1;
    return true;
}

/* The standard one source is compiled to: what the package's recipe named for
   that language, or the consumer's where the recipe named none. Empty means
   neither said, and nothing is pushed. */
static const char *std_for(const prepared_unit *unit, bool is_cpp, const char *std,
                           const char *cpp_std) {
    const char *own = is_cpp ? unit->cpp_std : unit->std;
    if(own[0] != '\0')
        return own;
    const char *fallback = is_cpp ? cpp_std : std;
    return fallback != NULL ? fallback : "";
}

static bool push_std(ir_source *source, const char *value) {
    if(value[0] == '\0')
        return true;
    char flag[RECIPE_STD_MAX + 8];
    const int written = snprintf(flag, sizeof flag, "-std=%s", value);
    if(written < 0 || (size_t)written >= sizeof flag)
        return false;
    /* Unit scope, so the most specific statement about a translation unit is
       the last one the compiler sees (RFC-0013). */
    return ir_add_option(&source->options, &source->option_count, flag, ir_scope_unit);
}

/* One package's sources as one target. */
static bool describe_target(ir_document *doc, const prepared_unit *unit, const char *std,
                            const char *cpp_std, char *err, size_t err_size) {
    char name[DEP_NAME_MAX + 16];
    const int written = snprintf(name, sizeof name, "%s:objects", unit->name);
    if(written < 0 || (size_t)written >= sizeof name) {
        snprintf(err, err_size, "the name of package '%s' is too long to describe", unit->name);
        return false;
    }

    ir_target *target = ir_add_target(doc, name, ir_target_object);
    if(target == NULL || !ir_set_target_package(target, unit->name) ||
       !push_defines(&target->options, &target->option_count, &unit->defines) ||
       !push_options(&target->options, &target->option_count, &unit->flags) ||
       !push_includes(&target->includes, &target->include_count, &unit->includes)) {
        snprintf(err, err_size, "out of memory describing package '%s'", unit->name);
        return false;
    }

    for(size_t i = 0; i < str_list_count(&unit->sources); i++) {
        const char *absolute = str_list_get(&unit->sources, i);
        const char *relative = NULL;
        if(!relative_to(absolute, unit->root, &relative)) {
            snprintf(err, err_size, "source '%s' of package '%s' is not under its root '%s'",
                     absolute, unit->name, unit->root);
            return false;
        }
        const bool is_cpp = source_is_cpp(relative);
        ir_source *source =
            ir_add_source(target, relative, is_cpp ? ir_language_cpp : ir_language_c);
        if(source == NULL || !push_std(source, std_for(unit, is_cpp, std, cpp_std))) {
            snprintf(err, err_size, "out of memory describing source '%s' of package '%s'",
                     relative, unit->name);
            return false;
        }
    }
    return true;
}

/* Every package of one set that ships sources. */
static bool describe_targets(ir_document *doc, const prepared_deps *deps, const char *std,
                             const char *cpp_std, char *err, size_t err_size) {
    if(deps == NULL)
        return true;
    for(size_t i = 0; i < deps->unit_count; i++) {
        /* A package that ships only headers is a `Dependency` and not a target:
           there is nothing to compile, and an empty target would be a node the
           engine has to skip rather than one it never had. */
        if(str_list_count(&deps->units[i].sources) == 0)
            continue;
        if(!describe_target(doc, &deps->units[i], std, cpp_std, err, err_size))
            return false;
    }
    return true;
}

bool ir_transform_dependency_targets(ir_document *doc, const prepared_deps *deps,
                                     const prepared_deps *dev, const char *std, const char *cpp_std,
                                     char *err, size_t err_size) {
    if(doc == NULL)
        return true;
    return describe_targets(doc, deps, std, cpp_std, err, err_size) &&
           describe_targets(doc, dev, std, cpp_std, err, err_size);
}
