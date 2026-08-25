#include <molto/services/ir_service.h>

#include <molto/util/json.h>
#include <molto/util/text.h>

#include <stdio.h>
#include <string.h>

/*
 * Reading an IR document, in either encoding, under the directional rule of
 * RFC-0013:
 *
 *   an unknown attribute on a known node is ignored;
 *   an unknown node type is fatal, in both directions.
 *
 * Everything below is arranged around the second half. An attribute is safe to
 * ignore because it refines work that is already described; a node type is work
 * that is not described at all, and an engine that skips one builds something
 * other than what it was handed and reports success. Both directions matter
 * because a plugin reads what Molto sent as surely as Molto reads what the
 * plugin returned, and a plugin that drops a node type Molto added deletes it
 * from the document it gives back.
 *
 * This reader is built on the public builders alone. That is deliberate: if the
 * only way to construct a document were the internals, then a producer outside
 * this file — the native frontend, a test, a fixture — would be a second
 * constructor, and the two would drift.
 */

#define IR_ERR(err, size, ...)                                                                     \
    do {                                                                                           \
        if((err) != NULL && (size) > 0)                                                            \
            snprintf((err), (size), __VA_ARGS__);                                                  \
    } while(0)

/* --- unknown node types --- */

/* The two node types RFC-0013 defines and this revision does not carry, refused
   by name so the message says which one rather than "unknown key".

   Named rather than left to the generic scan below because the generic scan
   cannot see them in every encoding, and because a GeneratedSource is not an
   array at all: it is a Source carrying `produced_by`, which reads as an
   ordinary attribute and would otherwise be ignored — the one case where the
   "ignore what you don't know" half of the rule points the wrong way. */
static bool reject_named_absences(doc_view project, char *err, size_t err_size) {
    if(doc_array_len(project, "steps") > 0) {
        IR_ERR(err, err_size,
               "node type 'BuildStep' is not carried by IR schema %d: it lowers to a command "
               "and needs the 'generator' capability, which nothing grants yet",
               IR_SCHEMA);
        return false;
    }
    return true;
}

static bool reject_generated_source(doc_view source, char *err, size_t err_size) {
    if(doc_has_key(source, "", "produced_by") || doc_has_key(source, "", "deterministic")) {
        IR_ERR(err, err_size,
               "node type 'GeneratedSource' is not carried by IR schema %d: a source that does "
               "not exist until something ran needs a BuildStep to produce it",
               IR_SCHEMA);
        return false;
    }
    return true;
}

/* Refuse any *other* member of `node` that is a list of tables.
 *
 * The rule needs a way to tell an unknown attribute from an unknown node type,
 * and this is it: a node type arrives as a list of tables, an attribute as a
 * scalar or a list of strings. It follows the reasoning rather than a hardcoded
 * list of future names — a schema 2 that adds `Toolchain` nodes is refused here
 * without this file having heard of them.
 *
 * It only bites on the JSON side, and that is stated rather than hidden: the
 * TOML backend stores `[[targets]]` as the sections `targets[0]`, `targets[1]`
 * and deliberately does not report them as members of their parent, so there is
 * nothing to enumerate. JSON is the wire — what a plugin returns and what
 * crosses a version boundary — and TOML is a fixture somebody wrote by hand for
 * a test, where the author is the reviewer. */
static bool reject_unknown_nodes(doc_view node, const char *const *known, const char *where,
                                 char *err, size_t err_size) {
    str_list members;
    str_list_init(&members);
    if(!doc_table_members(node, "", &members)) {
        str_list_free(&members);
        IR_ERR(err, err_size, "out of memory reading %s", where);
        return false;
    }

    bool ok = true;
    for(size_t i = 0; ok && i < str_list_count(&members); i++) {
        const char *name = str_list_get(&members, i);

        bool recognised = false;
        for(size_t k = 0; !recognised && known[k] != NULL; k++)
            recognised = strcmp(known[k], name) == 0;
        if(recognised)
            continue;

        doc_view element;
        if(doc_array_len(node, name) > 0 && doc_array_at(node, name, 0, &element)) {
            IR_ERR(err, err_size,
                   "%s carries '%s', a list of nodes IR schema %d does not know: an unknown node "
                   "type is refused, never skipped",
                   where, name, IR_SCHEMA);
            ok = false;
        }
    }

    str_list_free(&members);
    return ok;
}

/* --- scalars --- */

/* A required string, long or not: the document owns its own lengths, so this
   reads into a caller buffer sized for what a node can hold rather than into
   one sized for what a manifest may write. */
#define IR_TEXT_MAX 4096

/* --- naming the node a message is about --- */

/*
 * A label is composed from a *bounded excerpt* of the name, not from the name.
 *
 * Two reasons that turn out to be one. A name is as long as the producer made
 * it — the document owns its own lengths — so a message quoting one whole can
 * be four kilobytes of unreadable, and the buffer holding "target '<name>'"
 * would have to be larger than the largest name to be provably safe. Eliding
 * first fixes the message and the buffer at once, which is why gcc's
 * -Wformat-truncation is right to object to the version that did not.
 */
#define IR_LABEL_MAX 256
#define IR_NAME_SHOWN_MAX 96

static void label_for(char *out, size_t size, const char *what, const char *name) {
    char shown[IR_NAME_SHOWN_MAX];
    text_elide_middle(name, shown, sizeof shown);
    snprintf(out, size, "%s '%s'", what, shown);
}

static bool read_required(doc_view node, const char *key, char *out, size_t size, const char *where,
                          char *err, size_t err_size) {
    if(!doc_get_string(node, "", key, out, size)) {
        IR_ERR(err, err_size, "%s is missing a '%s'", where, key);
        return false;
    }
    return true;
}

/* --- option nodes --- */

/* `CompileOption` and `LinkOption`. `scope` is vocabulary, so an unfamiliar one
   is refused: an option landing in the wrong scope reaches the command line in
   the wrong place, and RFC-0007 fixes that order as contract because a compiler
   takes the last of two contradictory flags. */
static bool read_options(doc_view node, const char *key, ir_option **array, size_t *count,
                         const char *where, char *err, size_t err_size) {
    static const char *const KNOWN[] = {"value", "scope", NULL};

    const size_t total = doc_array_len(node, key);
    for(size_t i = 0; i < total; i++) {
        doc_view option;
        if(!doc_array_at(node, key, i, &option)) {
            IR_ERR(err, err_size, "%s has a '%s' entry that is not a table", where, key);
            return false;
        }
        if(!reject_unknown_nodes(option, KNOWN, "an option", err, err_size))
            return false;

        char value[IR_TEXT_MAX];
        if(!read_required(option, "value", value, sizeof value, where, err, err_size))
            return false;

        char scope_name[32] = "";
        ir_scope scope = ir_scope_target;
        if(doc_get_string(option, "", "scope", scope_name, sizeof scope_name) &&
           !ir_scope_from_name(scope_name, &scope)) {
            IR_ERR(err, err_size,
                   "%s has an option in scope '%s', which is not target, profile "
                   "or unit",
                   where, scope_name);
            return false;
        }

        if(!ir_add_option(array, count, value, scope)) {
            IR_ERR(err, err_size, "out of memory reading %s", where);
            return false;
        }
    }
    return true;
}

/* `IncludePath`. `system` defaults to false — a plain -I — because that is what
   a producer that says nothing about it means. */
static bool read_includes(doc_view node, const char *key, ir_include **array, size_t *count,
                          const char *where, char *err, size_t err_size) {
    static const char *const KNOWN[] = {"value", "scope", "system", NULL};

    const size_t total = doc_array_len(node, key);
    for(size_t i = 0; i < total; i++) {
        doc_view include;
        if(!doc_array_at(node, key, i, &include)) {
            IR_ERR(err, err_size, "%s has an '%s' entry that is not a table", where, key);
            return false;
        }
        if(!reject_unknown_nodes(include, KNOWN, "an include path", err, err_size))
            return false;

        char value[IR_TEXT_MAX];
        if(!read_required(include, "value", value, sizeof value, where, err, err_size))
            return false;

        char scope_name[32] = "";
        ir_scope scope = ir_scope_target;
        if(doc_get_string(include, "", "scope", scope_name, sizeof scope_name) &&
           !ir_scope_from_name(scope_name, &scope)) {
            IR_ERR(err, err_size,
                   "%s has an include path in scope '%s', which is not target, profile or unit",
                   where, scope_name);
            return false;
        }

        bool system = false;
        (void)doc_get_bool(include, "", "system", &system);

        if(!ir_add_include(array, count, value, scope, system)) {
            IR_ERR(err, err_size, "out of memory reading %s", where);
            return false;
        }
    }
    return true;
}

/* --- sources --- */

static bool read_sources(doc_view target_node, ir_target *target, char *err, size_t err_size) {
    static const char *const KNOWN[] = {"path", "language", "options", NULL};

    const size_t total = doc_array_len(target_node, "sources");
    for(size_t i = 0; i < total; i++) {
        doc_view node;
        if(!doc_array_at(target_node, "sources", i, &node)) {
            IR_ERR(err, err_size, "target '%s' has a source that is not a table", target->name);
            return false;
        }
        if(!reject_generated_source(node, err, err_size) ||
           !reject_unknown_nodes(node, KNOWN, "a source", err, err_size))
            return false;

        char path[IR_TEXT_MAX];
        char where[IR_LABEL_MAX];
        label_for(where, sizeof where, "a source of target", target->name);
        if(!read_required(node, "path", path, sizeof path, where, err, err_size))
            return false;

        char language_name[32] = "";
        ir_language language = ir_language_c;
        if(!doc_get_string(node, "", "language", language_name, sizeof language_name)) {
            IR_ERR(err, err_size, "%s is missing a 'language'", where);
            return false;
        }
        if(!ir_language_from_name(language_name, &language)) {
            IR_ERR(err, err_size, "%s is in language '%s', which is not c or cpp", where,
                   language_name);
            return false;
        }

        ir_source *source = ir_add_source(target, path, language);
        if(source == NULL) {
            IR_ERR(err, err_size, "out of memory reading %s", where);
            return false;
        }
        if(!read_options(node, "options", &source->options, &source->option_count, where, err,
                         err_size))
            return false;
    }
    return true;
}

/* --- targets --- */

static bool read_artifact(doc_view target_node, ir_target *target, char *err, size_t err_size) {
    static const char *const KNOWN[] = {"kind", "path", "install", NULL};

    doc_view node;
    /* An absent artifact is absent, not empty: a target that names none is one
       whose output the engine composes, which is the ordinary case. */
    if(!doc_table_at(target_node, "artifact", &node))
        return true;

    char where[IR_LABEL_MAX];
    label_for(where, sizeof where, "the artifact of target", target->name);

    if(!reject_unknown_nodes(node, KNOWN, where, err, err_size))
        return false;

    /* An artifact that names no kind is of its target's kind, which is the only
       thing it could mean and saves every document repeating itself. */
    char kind_name[32] = "";
    ir_target_kind kind = target->kind;
    if(doc_get_string(node, "", "kind", kind_name, sizeof kind_name) &&
       !ir_target_kind_from_name(kind_name, &kind)) {
        IR_ERR(err, err_size,
               "%s is of kind '%s', which is not executable, static, shared, object or test", where,
               kind_name);
        return false;
    }

    char path[IR_TEXT_MAX];
    if(!read_required(node, "path", path, sizeof path, where, err, err_size))
        return false;

    char install[IR_TEXT_MAX] = "";
    const bool has_install = doc_get_string(node, "", "install", install, sizeof install);

    if(!ir_set_artifact(target, kind, path, has_install ? install : NULL)) {
        IR_ERR(err, err_size, "out of memory reading %s", where);
        return false;
    }
    return true;
}

static bool read_target(doc_view node, ir_document *out, char *err, size_t err_size) {
    static const char *const KNOWN[] = {"name",  "kind",       "sources",  "options", "includes",
                                        "links", "depends_on", "artifact", NULL};

    if(!reject_unknown_nodes(node, KNOWN, "a target", err, err_size))
        return false;

    char name[IR_TEXT_MAX];
    if(!read_required(node, "name", name, sizeof name, "a target", err, err_size))
        return false;

    char kind_name[32] = "";
    ir_target_kind kind = ir_target_executable;
    if(!doc_get_string(node, "", "kind", kind_name, sizeof kind_name)) {
        IR_ERR(err, err_size, "target '%s' is missing a 'kind'", name);
        return false;
    }
    if(!ir_target_kind_from_name(kind_name, &kind)) {
        IR_ERR(err, err_size,
               "target '%s' is of kind '%s', which is not executable, static, shared, object "
               "or test",
               name, kind_name);
        return false;
    }

    ir_target *target = ir_add_target(out, name, kind);
    if(target == NULL) {
        IR_ERR(err, err_size, "out of memory reading target '%s'", name);
        return false;
    }

    char where[IR_LABEL_MAX];
    label_for(where, sizeof where, "target", name);

    if(!read_sources(node, target, err, err_size) ||
       !read_options(node, "options", &target->options, &target->option_count, where, err,
                     err_size) ||
       !read_includes(node, "includes", &target->includes, &target->include_count, where, err,
                      err_size) ||
       !read_options(node, "links", &target->links, &target->link_count, where, err, err_size))
        return false;

    if(doc_has_key(node, "", "depends_on") &&
       !doc_get_array(node, "", "depends_on", &target->depends_on)) {
        IR_ERR(err, err_size, "%s has a 'depends_on' that is not a list of target names", where);
        return false;
    }

    return read_artifact(node, target, err, err_size);
}

/* --- dependencies --- */

static bool read_dependency(doc_view node, ir_document *out, char *err, size_t err_size) {
    static const char *const KNOWN[] = {"name", "version",   "origin", "scope",
                                        "root", "interface", NULL};
    static const char *const INTERFACE_KNOWN[] = {"includes", "options", "links", NULL};

    if(!reject_unknown_nodes(node, KNOWN, "a dependency", err, err_size))
        return false;

    char name[IR_TEXT_MAX];
    if(!read_required(node, "name", name, sizeof name, "a dependency", err, err_size))
        return false;

    char where[IR_LABEL_MAX];
    label_for(where, sizeof where, "dependency", name);

    char origin_name[32] = "";
    ir_dep_origin origin = ir_dep_registry;
    if(!doc_get_string(node, "", "origin", origin_name, sizeof origin_name)) {
        IR_ERR(err, err_size, "%s is missing an 'origin'", where);
        return false;
    }
    if(!ir_dep_origin_from_name(origin_name, &origin)) {
        IR_ERR(err, err_size, "%s comes from '%s', which is not registry, path, git or archive",
               where, origin_name);
        return false;
    }

    /* Required, and deliberately not defaulted to `runtime`. A missing scope
       has two readings — "everything compiles against this" and "the producer
       did not say" — and picking the first silently grants a development
       dependency to `src/` (RFC-0008). Refusing costs a producer one key. */
    char scope_name[32] = "";
    ir_dep_scope scope = ir_dep_scope_runtime;
    if(!doc_get_string(node, "", "scope", scope_name, sizeof scope_name)) {
        IR_ERR(err, err_size, "%s is missing a 'scope'", where);
        return false;
    }
    if(!ir_dep_scope_from_name(scope_name, &scope)) {
        IR_ERR(err, err_size, "%s is scoped '%s', which is not runtime or dev", where, scope_name);
        return false;
    }

    char root[IR_TEXT_MAX];
    if(!read_required(node, "root", root, sizeof root, where, err, err_size))
        return false;

    /* A path dependency has no version — its bytes are whatever is on disk —
       so an absent one is read as absent rather than as an error. */
    char version[IR_TEXT_MAX] = "";
    const bool has_version = doc_get_string(node, "", "version", version, sizeof version);

    ir_dependency *dep =
        ir_add_dependency(out, name, has_version ? version : NULL, origin, scope, root);
    if(dep == NULL) {
        IR_ERR(err, err_size, "out of memory reading %s", where);
        return false;
    }

    /* A dependency that exports nothing is a dependency with no interface, not
       a malformed one: a header-only drop with no include path is odd and it is
       the producer's business, not this reader's. */
    doc_view interface;
    if(!doc_table_at(node, "interface", &interface))
        return true;

    return reject_unknown_nodes(interface, INTERFACE_KNOWN, where, err, err_size) &&
           read_includes(interface, "includes", &dep->includes, &dep->include_count, where, err,
                         err_size) &&
           read_options(interface, "options", &dep->options, &dep->option_count, where, err,
                        err_size) &&
           read_options(interface, "links", &dep->links, &dep->link_count, where, err, err_size);
}

/* --- the document --- */

static bool read_project(doc_view project, ir_document *out, char *err, size_t err_size) {
    static const char *const KNOWN[] = {"name",    "version",      "root", "origin",
                                        "targets", "dependencies", NULL};

    if(!reject_named_absences(project, err, err_size) ||
       !reject_unknown_nodes(project, KNOWN, "the project", err, err_size))
        return false;

    char name[IR_TEXT_MAX];
    char version[IR_TEXT_MAX];
    char root[IR_TEXT_MAX];
    char origin[IR_TEXT_MAX];
    if(!read_required(project, "name", name, sizeof name, "the project", err, err_size) ||
       !read_required(project, "version", version, sizeof version, "the project", err, err_size) ||
       !read_required(project, "root", root, sizeof root, "the project", err, err_size) ||
       !read_required(project, "origin", origin, sizeof origin, "the project", err, err_size))
        return false;

    if(!ir_set_project(out, name, version, root, origin)) {
        IR_ERR(err, err_size, "out of memory reading the project");
        return false;
    }

    const size_t targets = doc_array_len(project, "targets");
    for(size_t i = 0; i < targets; i++) {
        doc_view node;
        if(!doc_array_at(project, "targets", i, &node)) {
            IR_ERR(err, err_size, "the project has a target that is not a table");
            return false;
        }
        if(!read_target(node, out, err, err_size))
            return false;
    }

    const size_t deps = doc_array_len(project, "dependencies");
    for(size_t i = 0; i < deps; i++) {
        doc_view node;
        if(!doc_array_at(project, "dependencies", i, &node)) {
            IR_ERR(err, err_size, "the project has a dependency that is not a table");
            return false;
        }
        if(!read_dependency(node, out, err, err_size))
            return false;
    }

    return true;
}

bool ir_read(doc_view view, ir_document *out, char *err, size_t err_size) {
    static const char *const KNOWN[] = {"schema", "files_read", "projects", NULL};

    if(out == NULL)
        return false;
    ir_document_init(out);

    long schema = 0;
    if(!doc_get_int(view, "", "schema", &schema)) {
        IR_ERR(err, err_size, "the document declares no 'schema'");
        goto failed;
    }
    /* Refused in both directions, and by equality rather than by "at most":
       a document from an older schema may be missing a node type this reader
       requires, and one from a newer schema may carry a node type it would have
       to skip. Either way the answer is a refusal before anything is built. */
    if(schema != IR_SCHEMA) {
        IR_ERR(err, err_size, "the document is IR schema %ld and this molto speaks schema %d",
               schema, IR_SCHEMA);
        goto failed;
    }
    out->schema = schema;

    if(!reject_unknown_nodes(view, KNOWN, "the document", err, err_size))
        goto failed;

    if(doc_has_key(view, "", "files_read") &&
       !doc_get_array(view, "", "files_read", &out->files_read)) {
        IR_ERR(err, err_size, "'files_read' is not a list of paths");
        goto failed;
    }

    /* One project per document in this revision, in a single-element array so
       that the day [workspace] is specified the schema widens without one. */
    const size_t projects = doc_array_len(view, "projects");
    if(projects != 1) {
        IR_ERR(err, err_size, "the document holds %zu projects and IR schema %d holds exactly one",
               projects, IR_SCHEMA);
        goto failed;
    }

    doc_view project;
    if(!doc_array_at(view, "projects", 0, &project)) {
        IR_ERR(err, err_size, "the document's project is not a table");
        goto failed;
    }
    if(!read_project(project, out, err, err_size))
        goto failed;

    return true;

failed:
    /* A partial document is not interpreted, ever: valid JSON prefix and
       invalid meaning is worse than nothing, and a build planned from half a
       description is the failure RFC-0013 refuses by name. */
    ir_document_free(out);
    return false;
}

bool ir_read_json(const char *text, ir_document *out, char *err, size_t err_size) {
    if(out == NULL)
        return false;
    ir_document_init(out);

    if(text == NULL) {
        IR_ERR(err, err_size, "there is no document to read");
        return false;
    }

    json_document *parsed = json_parse(text);
    if(parsed == NULL) {
        IR_ERR(err, err_size, "the document is not well-formed JSON");
        return false;
    }

    const bool ok = ir_read(doc_from_json(json_root(parsed)), out, err, err_size);
    json_free(parsed);
    return ok;
}
