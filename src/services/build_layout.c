#include "build_internal.h"

#include <molto/build/compile_flags.h>
#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/project/project_ctx.h>
#include <molto/services/fs_service.h>
#include <molto/services/ir_service.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Where a build puts a thing, and what it calls it.
 *
 * Every question here is answered from a path and a document and nothing else:
 * no compiler runs, no file is read, nothing is locked. That is what makes this
 * the one file the others may all depend on and that depends on none of them —
 * it was already a leaf in the call graph before it was a file.
 *
 * It is one file because the two halves are one decision. A path that says
 * where an object goes and a name that says how a source is printed have to
 * agree about what a dependency's root is, and when they were spread through
 * two thousand lines they agreed by two matching conditions rather than by
 * construction.
 */

/* The project's own code, and the code that tests it. Neither is a package, so
   neither carries a name or a version: a line about one names the source. */
static const build_unit_label project_label = {.origin = build_origin_project};
static const build_unit_label tests_label = {.origin = build_origin_tests};

/* The dependency a target belongs to, or NULL when it belongs to the project. */
static const ir_dependency *package_of(const ir_document *doc, const ir_target *target) {
    if(target->package == NULL)
        return NULL;
    for(size_t i = 0; i < doc->dependency_count; i++) {
        if(doc->dependencies[i].name != NULL &&
           strcmp(doc->dependencies[i].name, target->package) == 0)
            return &doc->dependencies[i];
    }
    /* Unreachable for a document the transforms produced: the same pass writes
       the `Target` and the `Dependency` it names. A plugin's document is held
       to it by ir_validate, which refuses an unresolvable name outright. */
    return NULL;
}

bool build_in_set(const ir_document *doc, const ir_target *target, doc_target_set set) {
    const ir_dependency *package = package_of(doc, target);
    if(package != NULL) {
        return package->scope == ir_dep_scope_dev ? set == doc_targets_dev_packages
                                                  : set == doc_targets_runtime_packages;
    }
    return set == doc_targets_tests ? target->kind == ir_target_test
                                    : set == doc_targets_project && target->kind != ir_target_test;
}

/* What a target's paths are relative to: its package's root where it has one,
   and the project's otherwise (RFC-0013). */
const char *build_target_root(const ir_document *doc, const ir_target *target, const char *root) {
    const ir_dependency *package = package_of(doc, target);
    return package != NULL && package->root != NULL ? package->root : root;
}

/* How the report names one target's units.
 *
 * Only two answers reach a line for a package — a registry package states a
 * version someone can verify, and everything else is a module whose bytes are
 * wherever the manifest said. Which of git, path or archive it was stays in the
 * lock, where it can be acted on.
 *
 * It borrows the document's own strings, so the labels die with the document
 * they were built from. */
static build_unit_label label_for_target(const ir_document *doc, const ir_target *target) {
    const ir_dependency *package = package_of(doc, target);
    if(package == NULL)
        return target->kind == ir_target_test ? tests_label : project_label;
    return (build_unit_label){
        .origin = package->origin == ir_dep_registry ? build_origin_registry : build_origin_module,
        .name = package->name,
        .version = package->version,
        .source = package->root,
    };
}

/* One label per target, indexed by the target's position in the document.

   Built once every transform has finished adding targets, and that ordering is
   the contract: the index is a position, so a target added afterwards would
   have no label and the units of the one before it would borrow the wrong name.

   Caller frees. NULL means the allocation failed. */
[[nodiscard]] build_unit_label *build_labels_for(const ir_document *doc) {
    build_unit_label *labels = calloc(doc->target_count + 1, sizeof *labels);
    if(labels == NULL)
        return NULL;
    for(size_t t = 0; t < doc->target_count; t++)
        labels[t] = label_for_target(doc, &doc->targets[t]);
    return labels;
}

/* Compose the output executable path for a package. */
/*
 * Where this build's output goes under `build/`: the profile, with the target
 * in front of it when one was named.
 *
 * A build for elsewhere cannot share a directory with the host's: the objects
 * are called the same and hold different code. Absent a target the segment is
 * what it has always been, so no project that never asked for one sees a path
 * move.
 */
[[nodiscard]] bool build_segment(build_profile profile, const char *platform, char *out,
                                 size_t out_size) {
    if(platform == NULL)
        return fs_format_path(out, out_size, "%s", profile_name(profile));
    return fs_format_path(out, out_size, "%s/%s", platform, profile_name(profile));
}

/*
 * Where the artifact lands, which is the one place that wants a filename
 * rather than a name.
 *
 * `name` comes from `library_names_of` and is portable on purpose -- it is
 * also what the IR records. Only an executable gains anything here, and only
 * on Windows: a library already carries its own extension, and adding `.exe`
 * to `libgreet.a` would name a file no linker looks for.
 */
[[nodiscard]] bool build_compose_binary_path(const char *root, const char *segment,
                                             const char *name, ir_target_kind kind, char *out,
                                             size_t out_size) {
    const char *suffix = kind == ir_target_executable ? FS_EXECUTABLE_SUFFIX : "";
    return fs_format_path(out, out_size, "%s/" DIR_BUILD "/%s/%s%s", root, segment, name, suffix) ||
           fs_report_long_path(name);
}

/* Select the settings for `profile` from a parsed project context. */
manifest_profile build_profile_settings(const project_ctx *ctx, build_profile profile) {
    switch(profile) {
    case profile_release:
        return ctx->profile.release;
    case profile_bench:
        return ctx->profile.bench;
    case profile_custom:
        return ctx->profile.custom;
    case profile_debug:
    default:
        return ctx->profile.debug;
    }
}

/* Map a source path to its object path, mirroring the source tree under
   `root/build/<profile_dir>/obj`. */
[[nodiscard]] bool build_object_path_for(const char *root, const char *profile_dir,
                                         const char *source, char *out, size_t out_size) {
    size_t root_len = strlen(root);
    const char *relative = source;
    if(strncmp(source, root, root_len) == 0 && source[root_len] == '/')
        relative = source + root_len + 1;

    /* A dependency lives outside the project, so `relative` is still absolute
       for every source one brings -- and an absolute path cannot name a place
       inside `obj/`. On POSIX the join happens to work and hides that: the
       object for `/tmp/greet/greet.c` lands in `obj//tmp/greet`, which is a
       directory like any other. On Windows the same join produces
       `obj/D:/tmp/greet`, and `D:` is not a name a directory can have. */
    char inside[PATH_BUFFER_SIZE];
    if(!fs_path_without_root(relative, inside, sizeof inside))
        return fs_report_long_path(source);

    return fs_format_path(out, out_size, "%s/" DIR_BUILD "/%s/" DIR_OBJ "/%s" OBJECT_SUFFIX, root,
                          profile_dir, inside) ||
           fs_report_long_path(source);
}

/* The dependency file gcc writes next to an object: "<object>.d". Used both to
   tell the compiler where to write it and to read it back when deciding whether
   to rebuild, so the two paths always match. */
[[nodiscard]] bool build_depfile_path_for(const char *object, char *out, size_t out_size) {
    return fs_format_path(out, out_size, "%s" DEPFILE_SUFFIX, object) ||
           fs_report_long_path(object);
}

/* Create the parent directory chain for `path`. */
bool build_make_parent_dirs(const char *path) {
    char directory[PATH_BUFFER_SIZE];
    if(!fs_format_path(directory, sizeof directory, "%s", path))
        return fs_report_long_path(path);
    char *slash = strrchr(directory, '/');
    if(slash == NULL)
        return true;
    *slash = '\0';
    return fs_make_dirs(directory);
}

/* Join argv items into one space-separated heap string (caller frees). The
   buffer is sized from the same strings, so a short write means something went
   wrong and the fingerprint would be wrong too: report it as a failure. */
char *build_join_args(const str_list *argv) {
    size_t total = 1;
    for(size_t i = 0; i < str_list_count(argv); i++)
        total += strlen(str_list_get(argv, i)) + 1;
    char *out = malloc(total);
    if(out == NULL)
        return NULL;
    size_t pos = 0;
    for(size_t i = 0; i < str_list_count(argv); i++) {
        int written =
            snprintf(out + pos, total - pos, "%s%s", i > 0 ? " " : "", str_list_get(argv, i));
        if(written < 0 || (size_t)written >= total - pos) {
            free(out);
            return NULL;
        }
        pos += (size_t)written;
    }
    return out;
}

/* Portion of `path` relative to `root` (drops a leading "root/"), or `path`
   unchanged if it is not under root. */
const char *build_relative_to_root(const char *root, const char *path) {
    size_t root_len = strlen(root);
    if(strncmp(path, root, root_len) == 0 && path[root_len] == '/')
        return path + root_len + 1;
    return path;
}

/* Which compiler was asked, as a person would name it.
 *
 * Under C_COMPILER there is no vendor and no version to give — the compiler
 * was chosen by hand and never asked what it was — so the binary itself is the
 * honest answer, and a better one than an empty line. */
void build_describe_compiler(const resolved_toolchain *chain, bool is_cpp, char *out,
                             size_t out_size) {
    if(chain->vendor[0] != '\0' && chain->version[0] != '\0') {
        snprintf(out, out_size, "%s %s", chain->vendor, chain->version);
        return;
    }
    const char *driver = compile_flags_driver(chain, is_cpp);
    if(driver == NULL) {
        snprintf(out, out_size, "%s", chain->vendor);
        return;
    }
    const char *base = strrchr(driver, '/');
    snprintf(out, out_size, "%s", base != NULL ? base + 1 : driver);
}
