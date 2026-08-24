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

bool ir_transform_dependencies(ir_document *doc, const prepared_deps *deps, char *err,
                               size_t err_size) {
    if(doc == NULL || deps == NULL)
        return true; /* nothing to say */

    for(size_t i = 0; i < deps->unit_count; i++) {
        const prepared_unit *unit = &deps->units[i];
        /* A path dependency carries no version — its bytes are whatever is on
           disk — and the node says so by having none rather than by inventing
           one (RFC-0013). */
        const char *version = unit->version[0] != '\0' ? unit->version : NULL;
        ir_dependency *dep =
            ir_add_dependency(doc, unit->name, version, origin_of(unit->origin), unit->root);
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
