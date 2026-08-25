#include <molto/services/ir_transform.h>

#include <stdio.h>

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
   separation real rather than documented (RFC-0008). */
static bool reaches(ir_dep_scope scope, ir_target_kind kind) {
    return scope == ir_dep_scope_runtime || kind == ir_target_test;
}

bool ir_transform_fold_dependencies(ir_document *doc, char *err, size_t err_size) {
    if(doc == NULL)
        return true;

    for(size_t t = 0; t < doc->target_count; t++) {
        ir_target *target = &doc->targets[t];
        for(size_t i = 0; i < doc->dependency_count; i++) {
            const ir_dependency *dep = &doc->dependencies[i];
            if(!reaches(dep->scope, target->kind))
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
