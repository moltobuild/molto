#include "build_internal.h"

#include <molto/build/compile_flags.h>
#include <molto/build/diagnostic.h>
#include <molto/build/diagnostic_view.h>
#include <molto/build/library.h>
#include <molto/build/report.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Turning objects into the thing that ships: a program, a static archive, or a
 * shared library and the two links that make it loadable.
 *
 * Separate from compiling because the two fail differently and are read
 * differently. A compiler names a file and a line and says one thing about it;
 * a linker names a symbol, says it is undefined, and leaves the reader to find
 * which of forty objects wanted it. `report_link_diagnostics` exists for that
 * asymmetry alone, and keeping it beside the compile pass invited the two
 * grammars to drift into each other.
 */

/* Return true if the executable must be re-linked: it is missing or older
   than at least one object file. */
static bool link_needed(const str_list *objects, const char *binary) {
    for(size_t i = 0; i < str_list_count(objects); i++) {
        if(fs_source_newer(str_list_get(objects, i), binary))
            return true;
    }
    return false;
}

/* One scope's link options, in the order the producer wrote them. */
static bool push_links(str_list *argv, const ir_target *node, ir_scope scope) {
    bool ok = true;
    for(size_t i = 0; ok && i < node->link_count; i++) {
        if(node->links[i].scope == scope)
            ok = str_list_push(argv, node->links[i].value);
    }
    return ok;
}

/* Build the link command into `argv`: linker, objects, -o binary, and then
   everything the document says reaches this target's link line.
 *
 * All of it comes off the node. A `LinkOption` is what reaches the line — the
 * `-l` is already on a library and `-flto` is just another value — so nothing
 * here tells one from the other, which is what lets the same loop carry both.
 *
 * Scope order is the compile line's, for the same reason: it is the only
 * ordering the document expresses, and a linker takes the last of two
 * contradictory flags exactly as a compiler does. `-o` moves ahead of them all,
 * where the manifest path put it in the middle — a linker does not care where
 * its output is named, and putting it before means the scopes stay contiguous.
 *
 * What still does not come off the node: the objects, which the engine composes,
 * and which driver runs, which is the toolchain's answer and not a document's
 * opinion. */
static bool build_link_argv(str_list *argv, bool any_cpp, const str_list *objects,
                            const char *binary, const ir_target *node,
                            const resolved_toolchain *chain, const library_names *names) {
    const char *driver = compile_flags_driver(chain, any_cpp);
    if(driver == NULL) {
        fprintf(stderr, "molto: '%s' needs a C++ compiler and none was resolved\n", binary);
        return false;
    }
    bool ok = str_list_push(argv, driver);
    /* Read off the node rather than passed in: the document already says this
       is a shared library, and a second way of saying it could disagree with
       the first. The soname that goes with it is a LinkOption the frontend
       wrote, and arrives with the rest of them below. */
    if(ok && node->kind == ir_target_shared) {
        ok = str_list_push(argv, ARG_SHARED);
        /* And the name to record inside it, composed by `library_names_of` for
           the platform this build targets. It is not read off the node: the
           document is not told which platform it is for, so it cannot hold this
           and used to hold GNU ld's spelling of it on every machine. */
        if(ok && names != NULL && names->name_option[0] != '\0')
            ok = str_list_push(argv, names->name_option);
    }
    for(size_t i = 0; ok && i < str_list_count(objects); i++)
        ok = str_list_push(argv, str_list_get(objects, i));
    if(ok)
        ok = str_list_push(argv, ARG_OUTPUT) && str_list_push(argv, binary);
    return ok && push_links(argv, node, ir_scope_target) &&
           push_links(argv, node, ir_scope_profile) && push_links(argv, node, ir_scope_unit);
}

/* Everything a link's own report needs that the link itself does not. */
typedef struct {
    const char *root;
    const char *binary;
    const resolved_toolchain *chain;
    build_report *report;
    bool any_cpp;
} link_env;

/* What the linker said about one binary, framed the way a compiler's output is.
 *
 * Usually without an excerpt: a linker names a place in anyone's source only
 * when the objects it was given carry debug information, so an undefined
 * symbol can be pointed at under `debug` and never under a profile that turned
 * it off. What it always names is the symbol, which is the thing to go and
 * look for. */
static void report_link_diagnostics(const link_env *where, const char *output, bool truncated,
                                    int status) {
    diagnostic_list found;
    diagnostic_list_init(&found);
    /* A linker that failed names no severity because it has only the one; a
       linker that succeeded and still spoke was warning, and saying otherwise
       would fail a build that stands. */
    const bool parsed =
        status == 0 ? diagnostic_parse(output, &found) : diagnostic_parse_link(output, &found);
    if(!parsed) {
        diagnostic_list_free(&found);
        return;
    }
    diagnostic_list_set_columns(&found, diagnostic_columns_of_vendor(where->chain->vendor));
    if(truncated)
        build_push_own(&found, where->binary, diagnostic_severity_note,
                       "there was more of this than Molto kept");
    if(status != 0 && diagnostic_count_severity(&found, diagnostic_severity_error) == 0)
        build_push_own(&found, where->binary, diagnostic_severity_error,
                       "the linker failed with nothing to say about it");

    char compiler[TOOLCHAIN_DESCRIPTION_MAX];
    build_describe_compiler(where->chain, where->any_cpp, compiler, sizeof compiler);
    const diagnostic_context ctx = {
        .unit = fs_relative_to(where->binary, where->root),
        .action = diagnostic_view_linking,
        .compiler = compiler,
        .root = where->root,
    };
    char *block = diagnostic_view_render(&found, &ctx, build_report_wants_colour(where->report));
    diagnostic_list_free(&found);
    if(block == NULL)
        return;
    build_report_message(where->report, "%s\n", block);
    free(block);
}

/* Link `objects` into `binary` when needed — `force` (something recompiled),
   a stale/missing binary, or a changed link command (per the WSDB). Records the
   link command in the WSDB. Returns false only if a needed link failed. */
bool build_link_project(bool any_cpp, const str_list *objects, const char *binary,
                        const ir_target *node, const library_names *names, const project_env *env,
                        const resolved_toolchain *chain, bool force, wsdb *db, const char *root,
                        build_report *report) {
    str_list argv;
    str_list_init(&argv);
    if(!build_link_argv(&argv, any_cpp, objects, binary, node, chain, names)) {
        str_list_free(&argv);
        return false;
    }
    /* The environment belongs in the link fingerprint for the same reason it
       belongs in the compile one: it reaches the linker, so a different
       LIBRARY_PATH is a different binary. Relying on `force` to catch that
       would be correct only by accident — it is true when something was
       recompiled, and every object could have come from the shared cache. */
    char *command = build_command_fingerprint(&argv, env);

    bool ok = true;
    if(force || command == NULL || !wsdb_binary_fresh(db, binary, command) ||
       link_needed(objects, binary)) {
        char *output = malloc(BUILD_OUTPUT_SIZE);
        bool truncated = false;
        const int status = build_run_str_argv(&argv, env, output,
                                              output != NULL ? BUILD_OUTPUT_SIZE : 0, &truncated);
        ok = status == 0;
        if(output != NULL) {
            const link_env where = {.root = root,
                                    .binary = binary,
                                    .chain = chain,
                                    .report = report,
                                    .any_cpp = any_cpp};
            report_link_diagnostics(&where, output, truncated, status);
        } else if(!ok) {
            build_report_message(report, "molto: failed to link '%s'\n", binary);
        }
        free(output);
        if(ok && (command == NULL || !wsdb_record_binary(db, binary, command)))
            fprintf(stderr, "molto: warning: could not record '%s' as up to date\n", binary);
    }
    free(command);
    str_list_free(&argv);
    return ok;
}

/*
 * A static library: the objects, with an index, and nothing else.
 *
 * `ar` is not a linker and this is not a link. Nothing is resolved, no symbol
 * is looked up and no other library is consulted — which is why the node's link
 * options are not on this line. They belong to whoever links the program that
 * finally uses this archive, and putting them here would be recording an
 * intention `ar` has no way to honour.
 *
 * The archive is removed first rather than updated in place. `ar r` replaces
 * the members it is given and leaves the rest, so an object whose source was
 * deleted would stay in the archive across every later build — present at link
 * time, absent from the sources, and impossible to account for.
 */
static bool build_archive_argv(str_list *argv, const char *archiver, const str_list *objects,
                               const char *archive) {
    bool ok =
        str_list_push(argv, archiver) && str_list_push(argv, "rcs") && str_list_push(argv, archive);
    for(size_t i = 0; ok && i < str_list_count(objects); i++)
        ok = str_list_push(argv, str_list_get(objects, i));
    return ok;
}

/* The same freshness discipline the link has, for the same reason: an archive
   whose objects have not moved is an archive that does not need making, and
   remaking it would give every consumer a new mtime to react to. */
bool build_archive_project(const str_list *objects, const char *archive, const project_env *env,
                           const resolved_toolchain *chain, bool force, wsdb *db,
                           build_report *report) {
    char archiver[TOOLCHAIN_PATH_MAX];
    if(!library_archiver(chain->cc, archiver, sizeof archiver)) {
        build_report_message(report, "molto: the path to an archiver does not fit\n");
        return false;
    }

    str_list argv;
    str_list_init(&argv);
    if(!build_archive_argv(&argv, archiver, objects, archive)) {
        str_list_free(&argv);
        return false;
    }
    char *command = build_command_fingerprint(&argv, env);

    bool ok = true;
    if(force || command == NULL || !wsdb_binary_fresh(db, archive, command) ||
       link_needed(objects, archive)) {
        (void)remove(archive);
        char *output = malloc(BUILD_OUTPUT_SIZE);
        bool truncated = false;
        const int status = build_run_str_argv(&argv, env, output,
                                              output != NULL ? BUILD_OUTPUT_SIZE : 0, &truncated);
        ok = status == 0;
        /* An archiver says almost nothing, and what it does say is not a
           compiler diagnostic — so it is repeated as it came rather than framed
           as one. */
        if(!ok)
            build_report_message(report, "molto: %s could not archive '%s'%s%s\n", archiver,
                                 archive, output != NULL && output[0] != '\0' ? ": " : "",
                                 output != NULL ? output : "");
        free(output);
        (void)truncated;
        if(ok && (command == NULL || !wsdb_record_binary(db, archive, command)))
            fprintf(stderr, "molto: warning: could not record '%s' as up to date\n", archive);
    }
    free(command);
    str_list_free(&argv);
    return ok;
}

/*
 * The two names that point at a shared library, beside it.
 *
 * Relative, naming only the file: both links sit in the same directory as their
 * target, and an absolute link would write this machine's build path inside an
 * artifact whose whole purpose is to be copied somewhere else.
 *
 * A failure here is a warning and not a failed build. The library itself is
 * built and correct; what is missing is the convenience of linking against
 * `-lfoo`, and refusing a build over a symlink would be refusing the thing that
 * worked because of the thing that did not.
 */
void build_place_shared_links(const char *directory, const library_names *names,
                              build_report *report) {
    const char *const links[] = {names->soname, names->devlink};
    for(size_t i = 0; i < sizeof links / sizeof links[0]; i++) {
        if(links[i][0] == '\0' || strcmp(links[i], names->file) == 0)
            continue;
        char path[PATH_BUFFER_SIZE];
        if(!fs_format_path(path, sizeof path, "%s/%s", directory, links[i])) {
            (void)fs_report_long_path(links[i]);
            continue;
        }
        /* Removed first: symlink refuses to replace what is already there, and
           what is already there is a link to a version that has moved on. */
        (void)remove(path);
        if(!fs_link(names->file, path))
            build_report_message(report, "molto: warning: could not link '%s' to '%s'\n", links[i],
                                 names->file);
    }
}
