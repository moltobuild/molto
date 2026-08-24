#include <molto/services/frontend_service.h>

#include <molto/build/profile.h>
#include <molto/project/project_ctx.h>
#include <molto/services/fs_service.h>
#include <molto/services/source_discovery.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * `Project.toml` as a frontend, not as a privileged path.
 *
 * RFC-0013 is explicit that the native manifest produces an IR document exactly
 * as any other frontend does, and it argues the alternative down: leaving the
 * native build as it is and treating the IR as the thing plugins speak is less
 * work and it is wrong, because two paths diverge and they diverge in the
 * direction that punishes the newcomer. A representation whose own author's
 * tools do not produce it is a representation nobody has tested.
 *
 * So this is here first, before any plugin exists to test against, and it is
 * what `molto ir` prints in an ordinary project.
 */

#define MANIFEST_FILENAME "Project.toml"
#define NATIVE_PATH_MAX 4096

/* --- what belongs in the document, and what does not --- */

/*
 * The line is RFC-0013's: "The IR does not describe how to compile. It says what
 * a translation unit is and what options apply to it; the order those options
 * reach a command line, the driver chosen, the profile flags and the link
 * composition are RFC-0007's."
 *
 * So `-D` from the manifest is in, `flags` is in, `-I` is in as an IncludePath
 * node, `link` is in as a LinkOption, and `-std=` is in — a Meson project's
 * `c_std` has to land somewhere and a CompileOption is the only place it can.
 *
 * What is out: `-O` and `-g`. Those come from a profile's `opt_level` and
 * `debug_info`, which are the build's mechanics rather than anything the project
 * said, and writing them here would state the same thing in two places and
 * eventually in two ways.
 */

static bool push_defines(ir_option **array, size_t *count, const project_options *options,
                         ir_scope scope) {
    for(size_t i = 0; i < options->define_count; i++) {
        char flag[PROJECT_OPT_LEN + 4];
        snprintf(flag, sizeof flag, "-D%s", options->defines[i]);
        if(!ir_add_option(array, count, flag, scope))
            return false;
    }
    return true;
}

static bool push_flags(ir_option **array, size_t *count, const project_options *options,
                       ir_scope scope) {
    /* Verbatim, because RFC-0003 promises verbatim and this document is not the
       place to start rewriting them. */
    for(size_t i = 0; i < options->flag_count; i++) {
        if(!ir_add_option(array, count, options->flags[i], scope))
            return false;
    }
    return true;
}

static bool push_includes(ir_include **array, size_t *count, const project_options *options,
                          ir_scope scope) {
    for(size_t i = 0; i < options->include_count; i++) {
        /* Left relative when it was written relative: the node carries what the
           manifest said, and a relative include anchors at the project root by
           contract (RFC-0003). Anchoring it here would bake this machine's
           paths into a document meant to be diffable. */
        if(!ir_add_include(array, count, options->include[i], scope, false))
            return false;
    }
    return true;
}

static bool push_scope(ir_target *target, const project_options *options, ir_scope scope) {
    return push_defines(&target->options, &target->option_count, options, scope) &&
           push_flags(&target->options, &target->option_count, options, scope) &&
           push_includes(&target->includes, &target->include_count, options, scope);
}

/* The profile's own extra options, which are not decoration: the profile decides
   which defines are in force, and a `#ifdef` decides what even compiles. A
   document that left them out would describe code the build never sees. */
static const project_options *options_for(const project_ctx *ctx, build_profile profile) {
    switch(profile) {
    case profile_release:
        return &ctx->profile_options.release;
    case profile_bench:
        return &ctx->profile_options.bench;
    case profile_custom:
        return &ctx->profile_options.custom;
    case profile_debug:
        break;
    }
    return &ctx->profile_options.debug;
}

/* --- sources --- */

static bool push_sources(ir_target *target, const char *root) {
    char src_dir[NATIVE_PATH_MAX];
    if(!fs_format_path(src_dir, sizeof src_dir, "%s/src", root))
        return false;

    str_list found;
    str_list_init(&found);
    if(!source_discovery_collect(src_dir, &found)) {
        str_list_free(&found);
        /* No src/ is not a malformed project — `molto init` in an empty
           directory is one — and a document with no sources says so. */
        return true;
    }
    /* Sorted, so two runs over one project produce one byte-identical
       document however the filesystem felt about the order. */
    str_list_sort(&found);

    bool ok = true;
    for(size_t i = 0; ok && i < str_list_count(&found); i++) {
        const char *absolute = str_list_get(&found, i);
        const char *relative = fs_relative_to(absolute, root);
        ok = ir_add_source(target, relative,
                           source_is_cpp(relative) ? ir_language_cpp : ir_language_c) != NULL;
    }

    str_list_free(&found);
    return ok;
}

/* --- the document --- */

bool frontend_native(const char *root, const char *profile, ir_document *out, char *err,
                     size_t err_size) {
    if(root == NULL || out == NULL)
        return false;
    ir_document_init(out);

    build_profile which = profile_debug;
    if(profile != NULL && profile[0] != '\0' && !profile_parse(profile, &which)) {
        snprintf(err, err_size, "'%s' is not a profile", profile);
        return false;
    }

    char manifest[NATIVE_PATH_MAX];
    if(!fs_format_path(manifest, sizeof manifest, "%s/%s", root, MANIFEST_FILENAME)) {
        snprintf(err, err_size, "the path to %s does not fit", MANIFEST_FILENAME);
        return false;
    }

    project_ctx ctx;
    if(!project_load(manifest, &ctx, err, err_size))
        return false;

    /* Absolute, because every relative path in the document is anchored at it
       and a document anchored at a working directory means something different
       depending on where it was produced. */
    char absolute[NATIVE_PATH_MAX];
    char *real = realpath(root, NULL);
    const int written = snprintf(absolute, sizeof absolute, "%s", real != NULL ? real : root);
    free(real);
    if(written < 0 || (size_t)written >= sizeof absolute) {
        snprintf(err, err_size, "the project root does not fit in a path");
        return false;
    }

    if(!ir_set_project(out, ctx.project_name, ctx.version, absolute, IR_ORIGIN_NATIVE) ||
       !str_list_push(&out->files_read, MANIFEST_FILENAME)) {
        snprintf(err, err_size, "out of memory describing the project");
        goto failed;
    }

    /* One executable target, which is what a manifest can express today:
       RFC-0007 refuses `package.artifact`, so `static` and `shared` are nodes
       the IR carries and the native frontend never emits. `Target` as a real
       node is what will retire one-executable-per-package, and it retires it
       for a plugin's document before it retires it for this one. */
    ir_target *target = ir_add_target(out, ctx.project_name, ir_target_executable);
    if(target == NULL) {
        snprintf(err, err_size, "out of memory describing target '%s'", ctx.project_name);
        goto failed;
    }

    if(!push_sources(target, absolute)) {
        snprintf(err, err_size, "the sources under %s/src could not be described", absolute);
        goto failed;
    }

    if(ctx.target.std[0] != '\0') {
        char std_flag[32];
        snprintf(std_flag, sizeof std_flag, "-std=%s", ctx.target.std);
        if(!ir_add_option(&target->options, &target->option_count, std_flag, ir_scope_target))
            goto oom;
    }

    if(!push_scope(target, &ctx.target.options, ir_scope_target) ||
       !push_scope(target, options_for(&ctx, which), ir_scope_profile))
        goto oom;

    for(size_t i = 0; i < ctx.target.link_count; i++) {
        if(!ir_add_option(&target->links, &target->link_count, ctx.target.link[i], ir_scope_target))
            goto oom;
    }

    /* Relative to the profile's build directory, which is where the engine puts
       it and is the only anchor an artifact path has (RFC-0013). */
    if(!ir_set_artifact(target, ir_target_executable, ctx.project_name, NULL))
        goto oom;

    return true;

oom:
    snprintf(err, err_size, "out of memory describing target '%s'", ctx.project_name);
failed:
    ir_document_free(out);
    return false;
}
