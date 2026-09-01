#include <molto/services/frontend_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/source_service.h>
#include <molto/util/json_write.h>
#include <molto/util/semver.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Choosing a frontend, and asking it.
 *
 * Every refusal below happens before the process starts where it possibly can.
 * That is not tidiness: RFC-0013 makes a schema mismatch found halfway through
 * a document a half-read document, and RFC-0014 puts the version a plugin speaks
 * in a recipe precisely so the incompatibility is found while it is still a
 * refusal.
 */

#define FRONTEND_PATH_MAX 4096
#define FRONTEND_REQUEST_MAX 8192

/* Exit 3 is a frontend declining: the file is not one it understands. It is not
   an error, and it is what lets Molto offer a directory to the next
   candidate (RFC-0014). */
#define FRONTEND_DECLINED 3

#define FRONTEND_ERR(err, size, ...)                                                               \
    do {                                                                                           \
        if((err) != NULL && (size) > 0)                                                            \
            snprintf((err), (size), __VA_ARGS__);                                                  \
    } while(0)

/* --- capabilities --- */

bool frontend_declares(const recipe_plugin *plugin, const char *capability) {
    if(plugin == NULL || capability == NULL)
        return false;
    for(size_t i = 0; i < plugin->capability_count; i++) {
        if(strcmp(plugin->capabilities[i], capability) == 0)
            return true;
    }
    return false;
}

/* --- compatibility --- */

bool frontend_compatible(const frontend_choice *choice, char *err, size_t err_size) {
    if(choice == NULL)
        return false;

    /* Equality, not "at most": a plugin speaking an older schema may be missing
       a node type this Molto requires, and one speaking a newer schema may
       return a node type Molto would have to skip — and skipping one is a green
       build of something that was never asked for. */
    if(choice->plugin.ir_schema != IR_SCHEMA) {
        FRONTEND_ERR(err, err_size, "'%s' speaks IR schema %ld and this molto speaks schema %d",
                     choice->name, choice->plugin.ir_schema, IR_SCHEMA);
        return false;
    }

    if(choice->plugin.molto_min[0] == '\0')
        return true;

    semver needed;
    semver running;
    if(!semver_parse(choice->plugin.molto_min, &needed)) {
        FRONTEND_ERR(err, err_size, "'%s' asks for molto '%s', which is not a version",
                     choice->name, choice->plugin.molto_min);
        return false;
    }
    if(!semver_parse(MOLTO_PKG_VERSION, &running))
        return true; /* nothing to compare against; not the plugin's fault */

    if(semver_compare(&running, &needed) < 0) {
        FRONTEND_ERR(err, err_size, "'%s' needs molto %s or newer, and this is %s", choice->name,
                     choice->plugin.molto_min, MOLTO_PKG_VERSION);
        return false;
    }
    return true;
}

/* --- selection --- */

/* The first of `plugin`'s extensions that names a file present at `root`.
   Extensions are filenames rather than suffixes — `meson.build`, `CMakeLists.txt`
   — which is what RFC-0014's "the filenames that select it as a frontend"
   says, and what makes the test a stat rather than a directory walk. */
static bool entry_present(const char *root, const recipe_plugin *plugin, char *out, size_t size) {
    for(size_t i = 0; i < plugin->extension_count; i++) {
        char candidate[FRONTEND_PATH_MAX];
        if(!fs_format_path(candidate, sizeof candidate, "%s/%s", root, plugin->extensions[i]))
            continue;
        if(!fs_path_exists(candidate))
            continue;
        const int written = snprintf(out, size, "%s", plugin->extensions[i]);
        return written >= 0 && (size_t)written < size;
    }
    return false;
}

bool frontend_candidates(const char *root, frontend_choice *out, size_t capacity, size_t *count) {
    if(root == NULL || out == NULL || count == NULL)
        return false;
    *count = 0;

    /* On the heap, and the size is the reason: a plugin_entry carries two whole
       paths, so sixty-four of them are a quarter of a megabyte. Linux hands a
       thread eight megabytes and never noticed; Windows hands it one, and this
       array plus the frames above it walked off the end. The fault landed on
       the guard page at 0x200000, which reads as a wild pointer and is nothing
       of the kind. A limit both platforms have, met on only one of them. */
    plugin_entry *installed = malloc(sizeof *installed * PLUGIN_MAX_LISTED);
    if(installed == NULL)
        return false;

    size_t total = 0;
    if(!plugin_list(installed, PLUGIN_MAX_LISTED, &total)) {
        free(installed);
        return false;
    }

    /* plugin_list already sorts by name within each origin, so the order a
       directory's candidates are offered in is the same on every machine. An
       order that depended on readdir would be a build that differs between
       them. */
    for(size_t i = 0; i < total; i++) {
        if(!installed[i].has_recipe)
            continue; /* nothing recorded what it asked for; not a candidate */

        frontend_choice choice;
        memset(&choice, 0, sizeof choice);
        char discarded[256] = "";
        if(!plugin_read_recipe(installed[i].name, &choice.coordinate, &choice.plugin, discarded,
                               sizeof discarded))
            continue;
        if(!frontend_declares(&choice.plugin, FRONTEND_REQUEST_KIND))
            continue;
        if(!entry_present(root, &choice.plugin, choice.entry, sizeof choice.entry))
            continue;

        if(*count >= capacity) {
            free(installed);
            return false;
        }

        /* Bounded explicitly rather than left to the destination's size. The
           two buffers are the same width and plugin_list validated the name, so
           neither copy can truncate in fact — but a compiler cannot see that,
           and at -O3 gcc assumes the source may run to the end of the whole
           entry array. Saying the bound is also the honest statement: this
           copies at most as much of the name as a name can hold. */
        snprintf(choice.name, sizeof choice.name, "%.*s", (int)(sizeof choice.name - 1),
                 installed[i].name);
        snprintf(choice.path, sizeof choice.path, "%.*s", (int)(sizeof choice.path - 1),
                 installed[i].path);
        out[(*count)++] = choice;
    }
    free(installed);
    return true;
}

/* --- the request --- */

/* The document a frontend reads on its standard input. Small on purpose: a
   frontend is asked which directory, and told which of its own extensions made
   it the candidate so it does not have to go looking. */
static bool compose_request(const frontend_choice *choice, const char *root, char *out,
                            size_t size) {
    json_writer writer;
    json_writer_init_buffer(&writer, out, size);
    json_object_open(&writer, NULL);
    json_write_raw(&writer, "schema", "1");
    json_write_field(&writer, "request", FRONTEND_REQUEST_KIND);
    json_write_field(&writer, "root", root);
    json_write_field(&writer, "entry", choice->entry);
    json_object_close(&writer);
    json_writer_finish(&writer);

    return !json_writer_overflowed(&writer);
}

/* --- asking one --- */

/* What a plugin's answer has to satisfy beyond parsing.
 *
 * The origin check is the load-bearing one and it is easy to miss: every
 * stricter lowering rule in ir_validate keys on `Project.origin` not being
 * "native", so a plugin that named itself native would be handed the rules
 * written for a file in the user's own repository. The document says who
 * produced it, and Molto is the one that knows. */
static bool answer_is_sound(const frontend_choice *choice, const ir_document *doc, char *err,
                            size_t err_size) {
    if(doc->origin == NULL || strcmp(doc->origin, choice->name) != 0) {
        FRONTEND_ERR(err, err_size,
                     "'%s' returned a document whose origin is '%s': a plugin's document names "
                     "the plugin, and claiming another origin would claim another set of rules",
                     choice->name, doc->origin == NULL ? "" : doc->origin);
        return false;
    }

    /* RFC-0013 makes this the invalidation key of a cached document: a frontend
       that reads meson.build and four subdir() files and reports only the first
       has produced a cache entry that is silently wrong. Required now, before
       anything caches, because a frontend written against a Molto that did not
       ask would be a frontend that never learned to answer. */
    if(str_list_count(&doc->files_read) == 0) {
        FRONTEND_ERR(err, err_size,
                     "'%s' returned a document reporting no files read, and a frontend has to "
                     "report every file it opened",
                     choice->name);
        return false;
    }

    /* A frontend describes a project. It does not describe the graph.
     *
       `Dependency` is not a declaration of a need — it carries the version that
       was resolved, the origin it came from, and the directory the bytes landed
       in on this machine. All three are answers `resolve` gives, and `resolve`
       is the one phase RFC-0015 closes to plugins, because a plugin that could
       influence which versions a build uses would make a lock file a
       suggestion. A frontend cannot know any of the three, and a document that
       stated them would be stating them from somewhere.

       Refused here rather than in ir_validate, which sees the same document
       again after the transforms have added the real ones: this is a rule about
       what a plugin may *answer*, not about what the engine may lower.

       The day a frontend needs to say "this project wants zlib", that is a node
       this schema does not have, and adding it is a decision about RFC-0013 —
       not a reuse of the one that means something else. */
    if(doc->dependency_count > 0) {
        FRONTEND_ERR(err, err_size,
                     "'%s' returned a document naming %zu %s: a frontend describes a project and "
                     "not its graph, and resolving is not a plugin's to do",
                     choice->name, doc->dependency_count,
                     doc->dependency_count == 1 ? "dependency" : "dependencies");
        return false;
    }
    return true;
}

frontend_result frontend_ask(const frontend_choice *choice, const char *root,
                             const ir_bounds *bounds, ir_document *out, char *err,
                             size_t err_size) {
    return frontend_ask_with(choice, root, bounds, FRONTEND_TIMEOUT_MS, out, err, err_size);
}

frontend_result frontend_ask_with(const frontend_choice *choice, const char *root,
                                  const ir_bounds *bounds, unsigned timeout_ms, ir_document *out,
                                  char *err, size_t err_size) {
    if(choice == NULL || root == NULL || bounds == NULL || out == NULL)
        return frontend_failed;
    ir_document_init(out);

    if(!frontend_compatible(choice, err, err_size))
        return frontend_failed;

    char request[FRONTEND_REQUEST_MAX];
    if(!compose_request(choice, root, request, sizeof request)) {
        FRONTEND_ERR(err, err_size, "the request for '%s' could not be composed", choice->name);
        return frontend_failed;
    }

    /* `molto-<name> frontend`: the subcommand names the capability, so a plugin
       providing several does not have to infer which one from the document. */
    const char *const argv[] = {choice->path, FRONTEND_REQUEST_KIND, NULL};
    process_exchange io = {
        .request = request,
        .answer_max = FRONTEND_ANSWER_MAX,
        .timeout_ms = timeout_ms,
    };

    const process_exchange_result ran = process_exchange_run(argv, &io);
    switch(ran) {
    case process_exchange_timed_out:
        free(io.answer);
        FRONTEND_ERR(err, err_size, "'%s' did not answer within %u ms and was stopped",
                     choice->name, timeout_ms);
        return frontend_failed;
    case process_exchange_too_large:
        free(io.answer);
        FRONTEND_ERR(err, err_size, "'%s' returned more than %zu bytes, and was refused mid-read",
                     choice->name, (size_t)FRONTEND_ANSWER_MAX);
        return frontend_failed;
    case process_exchange_not_started:
        free(io.answer);
        FRONTEND_ERR(err, err_size, "'%s' could not be run", choice->name);
        return frontend_failed;
    case process_exchange_failed:
        free(io.answer);
        FRONTEND_ERR(err, err_size, "the exchange with '%s' broke", choice->name);
        return frontend_failed;
    case process_exchange_ok:
        break;
    }

    if(io.code == FRONTEND_DECLINED) {
        free(io.answer);
        return frontend_none;
    }
    if(io.code != 0) {
        free(io.answer);
        FRONTEND_ERR(err, err_size, "'%s' exited %d", choice->name, io.code);
        return frontend_failed;
    }

    /* Standard output MUST be a document and nothing else. A plugin that prints
       a banner there has produced an unparseable document, and that is what it
       is reported as — anything it wants to say goes to standard error, which
       reached the user already. */
    const bool read = ir_read_json(io.answer, out, err, err_size);
    free(io.answer);
    if(!read)
        return frontend_failed;

    if(!answer_is_sound(choice, out, err, err_size) || !ir_validate(out, bounds, err, err_size)) {
        ir_document_free(out);
        return frontend_failed;
    }
    return frontend_ok;
}

/* --- the whole selection --- */

frontend_result frontend_run(const char *root, const char *profile, ir_document *out, char *err,
                             size_t err_size) {
    if(root == NULL || out == NULL)
        return frontend_failed;
    ir_document_init(out);

    /* Absolute before anything else uses it. The request handed to a plugin
       says every relative path in its answer is anchored at this, and the
       bounds a document is validated against are built from it — so a relative
       root would compare an absolute source path against a relative bound and
       reject a document that was correct. Resolved here rather than asked of
       every caller, because a caller that forgot would fail in a way that reads
       like the plugin's fault. */
    char absolute[FRONTEND_PATH_MAX];
    char resolved[FRONTEND_PATH_MAX];
    const bool got = fs_real_path(root, resolved, sizeof resolved);
    const int written = snprintf(absolute, sizeof absolute, "%s", got ? resolved : root);
    if(written < 0 || (size_t)written >= sizeof absolute) {
        FRONTEND_ERR(err, err_size, "the directory to describe does not fit in a path");
        return frontend_failed;
    }
    root = absolute;

    /* The native frontend first, always. A plugin cannot take over a directory
       Molto already understands, which is the same rule the CLI applies to a
       command name and for the same reason. */
    char manifest[FRONTEND_PATH_MAX];
    if(fs_format_path(manifest, sizeof manifest, "%s/Project.toml", root) &&
       fs_path_exists(manifest))
        return frontend_native(root, profile, out, err, err_size) ? frontend_ok
                                                                  : frontend_bad_manifest;

    frontend_choice candidates[FRONTEND_MAX_CANDIDATES];
    size_t count = 0;
    if(!frontend_candidates(root, candidates, FRONTEND_MAX_CANDIDATES, &count)) {
        FRONTEND_ERR(err, err_size, "more than %d frontends claim this directory",
                     FRONTEND_MAX_CANDIDATES);
        return frontend_failed;
    }
    if(count == 0) {
        FRONTEND_ERR(err, err_size,
                     "no Project.toml here, and no installed plugin understands this directory");
        return frontend_none;
    }

    char build_dir[FRONTEND_PATH_MAX];
    if(!fs_format_path(build_dir, sizeof build_dir, "%s/build/%s", root,
                       profile == NULL || profile[0] == '\0' ? "debug" : profile)) {
        FRONTEND_ERR(err, err_size, "the build directory does not fit in a path");
        return frontend_failed;
    }
    /* The cache is a bound and not an omission: a dependency's bytes live there,
       and a document validated without it would refuse the first one that
       named them. No roots — nothing has been resolved when a frontend
       answers, and a producer does not get to widen its own bounds. */
    char cache[FRONTEND_PATH_MAX];
    const bool has_cache = source_cache_root(cache, sizeof cache);
    const ir_bounds bounds = {.workspace = root,
                              .build_dir = build_dir,
                              .cache = has_cache ? cache : NULL,
                              .roots = NULL,
                              .root_count = 0};

    for(size_t i = 0; i < count; i++) {
        const frontend_result asked =
            frontend_ask(&candidates[i], root, &bounds, out, err, err_size);
        /* Declining is not an error and not the end: it is what lets a
           directory with two candidate files reach the one that understands
           it. A failure is the end, because a plugin that broke has said
           something about itself that trying the next one would hide. */
        if(asked != frontend_none)
            return asked;
    }

    FRONTEND_ERR(err, err_size, "every frontend that could have read this directory declined");
    return frontend_none;
}
