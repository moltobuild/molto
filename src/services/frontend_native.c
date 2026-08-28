#include <molto/services/frontend_service.h>
#include <molto/services/host_service.h>

#include <molto/build/library.h>
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

/* The language standard, on the unit and not on the target.
 *
 * `[target]` declares two — `std` for C and `cpp_std` for C++ — and a target
 * holds units of both languages, so a single target-scope `-std=` would state
 * one of them for all of them. A C++ unit compiled with `-std=c17` is not a
 * detail: it is the wrong language.
 *
 * Unit scope is what RFC-0013 has for exactly this, and a Meson frontend's
 * `c_std` / `cpp_std` lands the same way. A target that declares only one
 * standard still says it once per unit; the document is machine-written and
 * one option per source is what says the truth. */
static bool push_std(ir_source *source, const project_ctx *ctx, bool is_cpp) {
    const char *value = is_cpp ? ctx->target.cpp_std : ctx->target.std;
    if(value[0] == '\0')
        return true; /* none declared for this language */
    char flag[32];
    snprintf(flag, sizeof flag, "-std=%s", value);
    return ir_add_option(&source->options, &source->option_count, flag, ir_scope_unit);
}

/* Add one absolute path to `target`, said relative to the project root: the
   document anchors every path at `Project.root`, so an absolute one would
   describe this machine rather than this project. */
static bool push_source(ir_target *target, const char *root, const project_ctx *ctx,
                        const char *absolute) {
    const char *relative = fs_relative_to(absolute, root);
    const bool is_cpp = source_is_cpp(relative);
    ir_source *source = ir_add_source(target, relative, is_cpp ? ir_language_cpp : ir_language_c);
    return source != NULL && push_std(source, ctx, is_cpp);
}

static bool push_sources(ir_target *target, const char *root, const project_ctx *ctx,
                         const str_list *paths) {
    for(size_t i = 0; i < str_list_count(paths); i++) {
        if(!push_source(target, root, ctx, str_list_get(paths, i)))
            return false;
    }
    return true;
}

/* Everything under `src/`. Sorted, so two runs over one project produce one
   byte-identical document however the filesystem felt about the order. */
static bool collect_project_sources(const char *root, str_list *out) {
    char src_dir[NATIVE_PATH_MAX];
    if(!fs_format_path(src_dir, sizeof src_dir, "%s/src", root))
        return false;
    /* No src/ is not a malformed project — `molto init` in an empty directory
       is one — and a document with no sources says so. */
    if(source_discovery_collect(src_dir, out))
        str_list_sort(out);
    return true;
}

/* --- what every target carries --- */

/* Everything that reaches the link line, in the order it reaches it.
 *
 * Three things do, and the manifest keeps them in three places: the raw `flags`
 * of `[target]` and of the profile, which are passed to the linker as well as to
 * the compiler so that `-flto` and `-fsanitize` work at all, and `[target].link`,
 * which names libraries. A `LinkOption` is what reaches the line, so the
 * libraries get their `-l` here rather than at composition — and the engine is
 * then spared telling a library from a flag, which it could not do anyway.
 *
 * `defines` are deliberately absent. They are a `CompileOption` and nothing
 * else: a define reaching a linker is noise in a fingerprint that decides
 * whether a binary is relinked. */
static bool push_link_scope(ir_target *target, const project_ctx *ctx, build_profile profile) {
    const project_options *profile_opts = options_for(ctx, profile);
    for(size_t i = 0; i < ctx->target.options.flag_count; i++) {
        if(!ir_add_option(&target->links, &target->link_count, ctx->target.options.flags[i],
                          ir_scope_target))
            return false;
    }
    for(size_t i = 0; i < profile_opts->flag_count; i++) {
        if(!ir_add_option(&target->links, &target->link_count, profile_opts->flags[i],
                          ir_scope_profile))
            return false;
    }
    for(size_t i = 0; i < ctx->target.link_count; i++) {
        char flag[PROJECT_LINK_NAME_MAX + 4];
        const int written = snprintf(flag, sizeof flag, "-l%s", ctx->target.link[i]);
        if(written < 0 || (size_t)written >= sizeof flag)
            return false;
        if(!ir_add_option(&target->links, &target->link_count, flag, ir_scope_target))
            return false;
    }
    return true;
}

/* What the host answered, onto the target that asked (RFC-0016).
 *
 * The answer is resolved here rather than at load time so it lands in the
 * document like everything else — an include is an `IncludePath` and a `-l` is
 * a `LinkOption`, and nothing downstream needs to learn that a directory came
 * from pkg-config rather than from the manifest. It is also what makes the
 * paths visible in `molto ir`, which is where someone debugging a build looks.
 *
 * Marked as system includes: these are headers the machine owns, and a warning
 * from inside `gtk/gtk.h` is not something a consumer can fix. */
static bool push_host_scope(ir_target *target, const project_ctx *ctx, char *err, size_t err_size) {
    for(size_t i = 0; i < ctx->target.host_count; i++) {
        host_answer answer;
        if(!host_resolve(ctx->target.host[i], &answer, err, err_size))
            return false;
        for(size_t j = 0; j < answer.include_count; j++) {
            if(!ir_add_include(&target->includes, &target->include_count, answer.includes[j],
                               ir_scope_target, true))
                return false;
        }
        for(size_t j = 0; j < answer.link_count; j++) {
            if(!ir_add_option(&target->links, &target->link_count, answer.links[j],
                              ir_scope_target))
                return false;
        }
    }
    return true;
}

/* `[target]`'s own scope, the profile's scope and the link line. Every target
   the native frontend emits carries all three, so they are composed once: an executable and a test
   that filled their scopes in two places would drift, and the drift would show up as a test
   compiled against options the code under it was not. */
static bool fill_common(ir_target *target, const project_ctx *ctx, build_profile profile, char *err,
                        size_t err_size) {
    return push_scope(target, &ctx->target.options, ir_scope_target) &&
           push_scope(target, options_for(ctx, profile), ir_scope_profile) &&
           push_link_scope(target, ctx, profile) && push_host_scope(target, ctx, err, err_size);
}

/* `src/` on the include path.
 *
 * Every Molto build has had it there and the manifest never says so: it is what
 * lets a source under `src/` include a sibling by name. A document that left it
 * out would describe a build whose every `#include "x.h"` between siblings
 * fails — and it would fail only once the engine started taking includes from
 * here, which is the worst moment to find out.
 *
 * Last, after everything the manifest named, because that is where the build
 * puts it: it reaches the compile line from the composed `-I` list, which comes
 * after every option a scope contributed. Include order decides which header
 * wins when two directories carry the same name, so this is placement, not
 * tidiness. */
static bool fill_src_include(ir_target *target) {
    return ir_add_include(&target->includes, &target->include_count, "src", ir_scope_target, false);
}

/* --- the tests --- */

/* The name and the artifact path of a per-file test target: the source's path
   relative to the root, with its extension dropped. `tests/test_json.c` becomes
   `tests/test_json`, which is where `molto test` already puts the binary.

   The whole stem and not the basename, because a name has to say which target
   it means: `tests/a/io.c` and `tests/b/io.c` are two tests and one basename,
   and ir_validate would refuse the document for the collision rather than
   build either. It is also what keeps a test from colliding with the
   executable, whose name never contains a slash. */
static bool test_stem(const char *root, const char *source, char *out, size_t out_size) {
    const int written = snprintf(out, out_size, "%s", fs_relative_to(source, root));
    if(written < 0 || (size_t)written >= out_size)
        return false;
    char *dot = strrchr(out, '.');
    const char *slash = strrchr(out, '/');
    if(dot != NULL && (slash == NULL || dot > slash))
        *dot = '\0';
    return true;
}

/* One test target, filled: the common scope, then `[test]`'s own on top of it,
   then the edge to the executable.
 *
 * `[test].options` sit at target scope because that is what they are — options
 * that apply to a whole target — and RFC-0013 orders the three scopes, so the
 * profile's still land after them on the composed line. Today's build applies
 * `[test].options` last instead; the two agree unless a project contradicts a
 * profile define from `[test].defines`, and the engine is where that is settled
 * when it starts lowering this document (RFC-0015).
 *
 * `depends_on` names the executable, and that edge is what says where the rest
 * of a test binary's objects come from. It deliberately does not say "minus the
 * entry point": two `main()` do not link, so the engine drops the executable's
 * own entry point because a linker would refuse the alternative. That is a law
 * rather than a policy, which is why the document does not restate it. When
 * RFC-0015's target graph lands, the library objects become an `object` target
 * that both the executable and the tests depend on, and the law stops needing
 * to be applied at all. */
static bool fill_test(ir_target *target, const project_ctx *ctx, build_profile profile,
                      const char *artifact, char *err, size_t err_size) {
    return fill_common(target, ctx, profile, err, err_size) &&
           push_scope(target, &ctx->test.options, ir_scope_target) && fill_src_include(target) &&
           str_list_push(&target->depends_on, ctx->project_name) &&
           ir_set_artifact(target, ir_target_test, artifact, NULL);
}

/* One target per test file, each bringing its own `main()`. The default, and
   what a suite of independent cases wants. */
static bool add_tests_per_file(ir_document *out, const char *root, const project_ctx *ctx,
                               build_profile profile, const str_list *sources, char *err,
                               size_t err_size) {
    for(size_t i = 0; i < str_list_count(sources); i++) {
        const char *source = str_list_get(sources, i);
        char stem[NATIVE_PATH_MAX];
        if(!test_stem(root, source, stem, sizeof stem))
            return false;
        ir_target *target = ir_add_target(out, stem, ir_target_test);
        if(target == NULL || !push_source(target, root, ctx, source) ||
           !fill_test(target, ctx, profile, stem, err, err_size))
            return false;
    }
    return true;
}

/* One target for the whole suite. The `main()` comes from the framework in
   `[test].sources`, which is why a framework that owns it needs this mode. */
static bool add_tests_single(ir_document *out, const char *root, const project_ctx *ctx,
                             build_profile profile, const str_list *sources, char *err,
                             size_t err_size) {
    char name[NATIVE_PATH_MAX];
    char artifact[NATIVE_PATH_MAX];
    if(snprintf(name, sizeof name, "%s_tests", ctx->project_name) < 0 ||
       !fs_format_path(artifact, sizeof artifact, "tests/%s", name))
        return false;
    ir_target *target = ir_add_target(out, name, ir_target_test);
    return target != NULL && push_sources(target, root, ctx, sources) &&
           fill_test(target, ctx, profile, artifact, err, err_size);
}

/* The tests as targets, or nothing at all.
 *
 * A project with no tests gets no test target rather than an empty one: a
 * target that builds nothing is a target a consumer has to special-case, and
 * "there are no tests" is said by there being none. */
static bool add_test_targets(ir_document *out, const char *root, const project_ctx *ctx,
                             build_profile profile, char *err, size_t err_size) {
    str_list extra;
    str_list sources;
    str_list_init(&extra);
    str_list_init(&sources);
    if(!project_test_sources(&ctx->test, &extra)) {
        snprintf(err, err_size, "out of memory reading [test].sources");
        str_list_free(&extra);
        str_list_free(&sources);
        return false;
    }
    if(!source_discovery_collect_tests(root, &extra, &sources, err, err_size)) {
        str_list_free(&extra);
        str_list_free(&sources);
        return false;
    }
    str_list_free(&extra);

    bool ok = true;
    if(str_list_count(&sources) > 0) {
        err[0] = '\0';
        ok = ctx->test.mode == test_mode_single
                 ? add_tests_single(out, root, ctx, profile, &sources, err, err_size)
                 : add_tests_per_file(out, root, ctx, profile, &sources, err, err_size);
        /* Only when nothing said anything: a host library that could not be
           resolved has already written the message worth reading, and out of
           memory is what is left when a helper failed and stayed quiet. */
        if(!ok && err[0] == '\0')
            snprintf(err, err_size, "out of memory describing the tests");
    }
    str_list_free(&sources);
    return ok;
}

/* --- the document --- */
/* The document's word for what `[package].artifact` asked for. */
static ir_target_kind target_kind_of(artifact_kind artifact) {
    switch(artifact) {
    case artifact_static:
        return ir_target_static;
    case artifact_shared:
        return ir_target_shared;
    case artifact_executable:
    case artifact_source:
        break;
    }
    return ir_target_executable;
}

/*
 * What being a shared library adds to this target's own command lines.
 *
 * Both go on the target rather than being applied by the engine, because a
 * document is meant to say everything that reaches a command line (RFC-0013).
 * Added by the engine instead they would be invisible to `molto ir` and absent
 * from `compile_commands.json`, where an editor reads what a file is compiled
 * with — and a file analysed without `-fPIC` is analysed as a different
 * translation unit than the one that was built.
 */
static bool push_library_scope(ir_target *target, artifact_kind artifact,
                               const library_names *names) {
    if(artifact != artifact_shared)
        return true;
    char soname[LIBRARY_NAME_MAX + 16];
    const int written = snprintf(soname, sizeof soname, "-Wl,-soname,%s", names->soname);
    if(written < 0 || (size_t)written >= sizeof soname)
        return false;
    return ir_add_option(&target->options, &target->option_count, "-fPIC", ir_scope_target) &&
           ir_add_option(&target->links, &target->link_count, soname, ir_scope_target);
}

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

    /* Before anything is collected, because a version that cannot name a shared
       library is a manifest problem and should be reported as one rather than
       after a directory walk that was never going to matter. */
    library_names names;
    if(!library_names_of(ctx.artifact, ctx.project_name, ctx.version, &names, err, err_size))
        goto failed;
    const ir_target_kind kind = target_kind_of(ctx.artifact);

    str_list sources;
    str_list_init(&sources);
    if(!collect_project_sources(absolute, &sources)) {
        str_list_free(&sources);
        snprintf(err, err_size, "the sources under %s/src could not be described", absolute);
        goto failed;
    }

    /* One target for the project, of whatever kind `[package].artifact` asked
       for. A manifest still describes exactly one thing built from `src/`; what
       changed is that the thing may be a library. Several targets in one
       document is a plugin frontend's business, and the engine already builds
       as many as a document carries — that is how the test binaries are made.

       It is filled to completion before the first test target is added: a
       target pointer is only valid until the next ir_add_target on the same
       document. */
    ir_target *target = ir_add_target(out, ctx.project_name, kind);
    /* Cleared so the fallback below can tell "a helper explained itself" from
       "a helper failed and did not". */
    err[0] = '\0';
    const bool described = target != NULL && push_sources(target, absolute, &ctx, &sources) &&
                           fill_common(target, &ctx, which, err, err_size) &&
                           fill_src_include(target) &&
                           push_library_scope(target, ctx.artifact, &names) &&
                           /* The artifact is relative to the profile's build directory, which is
                              where the engine puts it and is the only anchor an artifact path has
                              (RFC-0013). */
                           ir_set_artifact(target, kind, names.file, NULL);
    str_list_free(&sources);
    if(!described) {
        /* Only when nothing said anything. A host library that could not be
           resolved has already written the message worth reading, and out of
           memory is what is left when a helper failed and stayed quiet. */
        if(err[0] == '\0')
            snprintf(err, err_size, "out of memory describing target '%s'", ctx.project_name);
        goto failed;
    }

    if(!add_test_targets(out, absolute, &ctx, which, err, err_size))
        goto failed;

    return true;

failed:
    ir_document_free(out);
    return false;
}
