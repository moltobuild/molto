#include <molto/services/ir_service.h>

#include <molto/util/json.h>
#include <molto/util/json_write.h>

#include <stdlib.h>
#include <string.h>

/* --- spelling --- */

/* One table per vocabulary, read in both directions, so the writer and the
   reader cannot disagree about a name. A value absent from a table is refused
   rather than defaulted: `kind`, `language`, `scope` and a dependency's
   `origin` name what a node *is*, and a reader that defaulted an unfamiliar one
   would build something other than what the document said (RFC-0013). */
typedef struct {
    const char *name;
    int value;
} ir_word;

static const ir_word SCOPES[] = {
    {"target", ir_scope_target},
    {"profile", ir_scope_profile},
    {"unit", ir_scope_unit},
};

static const ir_word LANGUAGES[] = {
    {"c", ir_language_c},
    {"cpp", ir_language_cpp},
};

static const ir_word TARGET_KINDS[] = {
    {"executable", ir_target_executable},
    {"static", ir_target_static},
    {"shared", ir_target_shared},
    {"object", ir_target_object},
    {"test", ir_target_test},
};

static const ir_word DEP_ORIGINS[] = {
    {"registry", ir_dep_registry},
    {"path", ir_dep_path},
    {"git", ir_dep_git},
    {"archive", ir_dep_archive},
};

static const char *word_name(const ir_word *words, size_t count, int value) {
    for(size_t i = 0; i < count; i++) {
        if(words[i].value == value)
            return words[i].name;
    }
    return NULL;
}

static bool word_value(const ir_word *words, size_t count, const char *name, int *out) {
    if(name == NULL)
        return false;
    for(size_t i = 0; i < count; i++) {
        if(strcmp(words[i].name, name) == 0) {
            *out = words[i].value;
            return true;
        }
    }
    return false;
}

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

const char *ir_scope_name(ir_scope scope) {
    return word_name(SCOPES, COUNT_OF(SCOPES), (int)scope);
}

const char *ir_language_name(ir_language language) {
    return word_name(LANGUAGES, COUNT_OF(LANGUAGES), (int)language);
}

const char *ir_target_kind_name(ir_target_kind kind) {
    return word_name(TARGET_KINDS, COUNT_OF(TARGET_KINDS), (int)kind);
}

const char *ir_dep_origin_name(ir_dep_origin origin) {
    return word_name(DEP_ORIGINS, COUNT_OF(DEP_ORIGINS), (int)origin);
}

/* The four readers share a shape and differ only in which table they consult;
   spelling them out keeps the enum types honest at every call site. */
bool ir_scope_from_name(const char *name, ir_scope *out) {
    int value = 0;
    if(!word_value(SCOPES, COUNT_OF(SCOPES), name, &value))
        return false;
    *out = (ir_scope)value;
    return true;
}

bool ir_language_from_name(const char *name, ir_language *out) {
    int value = 0;
    if(!word_value(LANGUAGES, COUNT_OF(LANGUAGES), name, &value))
        return false;
    *out = (ir_language)value;
    return true;
}

bool ir_target_kind_from_name(const char *name, ir_target_kind *out) {
    int value = 0;
    if(!word_value(TARGET_KINDS, COUNT_OF(TARGET_KINDS), name, &value))
        return false;
    *out = (ir_target_kind)value;
    return true;
}

bool ir_dep_origin_from_name(const char *name, ir_dep_origin *out) {
    int value = 0;
    if(!word_value(DEP_ORIGINS, COUNT_OF(DEP_ORIGINS), name, &value))
        return false;
    *out = (ir_dep_origin)value;
    return true;
}

/* --- the model --- */

/* strdup is not in C23's freestanding set and the tree does not lean on it
   elsewhere; this also lets NULL through, which is what an absent optional
   string means. */
static char *dup_string(const char *text) {
    if(text == NULL)
        return NULL;
    const size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if(copy != NULL)
        memcpy(copy, text, size);
    return copy;
}

/* Append one zeroed element of `stride` to `*array` and hand back its address.
 *
 * The arrays carry a count and no capacity, because a node with a spare size_t
 * per list is a node that is mostly bookkeeping. What replaces it is the
 * invariant that an array of `n` elements is always allocated at the next power
 * of two at or above `n`, so the count alone says whether there is room — and
 * the doubling still matters, because a Meson project with two thousand sources
 * would otherwise reallocate two thousand times. */
static void *push(void **array, size_t *count, size_t stride) {
    size_t capacity = 0;
    while(capacity < *count)
        capacity = capacity == 0 ? 4 : capacity * 2;

    if(*count == capacity) {
        const size_t next = capacity == 0 ? 4 : capacity * 2;
        void *bigger = realloc(*array, next * stride);
        if(bigger == NULL)
            return NULL;
        *array = bigger;
    }

    char *slot = (char *)*array + (*count * stride);
    memset(slot, 0, stride);
    (*count)++;
    return slot;
}

void ir_document_init(ir_document *doc) {
    if(doc == NULL)
        return;
    memset(doc, 0, sizeof *doc);
    doc->schema = IR_SCHEMA;
    str_list_init(&doc->files_read);
}

static void free_options(ir_option *options, size_t count) {
    for(size_t i = 0; i < count; i++)
        free(options[i].value);
    free(options);
}

static void free_includes(ir_include *includes, size_t count) {
    for(size_t i = 0; i < count; i++)
        free(includes[i].value);
    free(includes);
}

static void free_target(ir_target *target) {
    free(target->name);
    for(size_t i = 0; i < target->source_count; i++) {
        free(target->sources[i].path);
        free_options(target->sources[i].options, target->sources[i].option_count);
    }
    free(target->sources);
    free_options(target->options, target->option_count);
    free_includes(target->includes, target->include_count);
    free_options(target->links, target->link_count);
    str_list_free(&target->depends_on);
    free(target->artifact.path);
    free(target->artifact.install);
}

static void free_dependency(ir_dependency *dep) {
    free(dep->name);
    free(dep->version);
    free(dep->root);
    free_includes(dep->includes, dep->include_count);
    free_options(dep->options, dep->option_count);
    free_options(dep->links, dep->link_count);
}

void ir_document_free(ir_document *doc) {
    if(doc == NULL)
        return;

    free(doc->name);
    free(doc->version);
    free(doc->root);
    free(doc->origin);
    for(size_t i = 0; i < doc->target_count; i++)
        free_target(&doc->targets[i]);
    free(doc->targets);
    for(size_t i = 0; i < doc->dependency_count; i++)
        free_dependency(&doc->dependencies[i]);
    free(doc->dependencies);
    str_list_free(&doc->files_read);

    /* Re-initialised rather than merely emptied, so a caller that frees a
       document and keeps reading it reads an empty one instead of a freed
       one, and a second free is a no-op. */
    ir_document_init(doc);
}

bool ir_is_from_plugin(const ir_document *doc) {
    return doc != NULL && doc->origin != NULL && strcmp(doc->origin, IR_ORIGIN_NATIVE) != 0;
}

/* --- building one --- */

/* Replace `*slot` with a copy of `text`, freeing what was there. False on
   allocation failure, with `*slot` left as it was. */
static bool set_string(char **slot, const char *text) {
    char *copy = dup_string(text);
    if(text != NULL && copy == NULL)
        return false;
    free(*slot);
    *slot = copy;
    return true;
}

bool ir_set_project(ir_document *doc, const char *name, const char *version, const char *root,
                    const char *origin) {
    if(doc == NULL)
        return false;
    return set_string(&doc->name, name) && set_string(&doc->version, version) &&
           set_string(&doc->root, root) && set_string(&doc->origin, origin);
}

ir_target *ir_add_target(ir_document *doc, const char *name, ir_target_kind kind) {
    if(doc == NULL)
        return NULL;

    ir_target *target = push((void **)&doc->targets, &doc->target_count, sizeof *doc->targets);
    if(target == NULL)
        return NULL;

    str_list_init(&target->depends_on);
    target->kind = kind;
    if(!set_string(&target->name, name)) {
        /* A target with no name describes nothing while reading like a target
           that does, so the half-added node goes rather than staying. */
        doc->target_count--;
        return NULL;
    }
    return target;
}

ir_source *ir_add_source(ir_target *target, const char *path, ir_language language) {
    if(target == NULL)
        return NULL;

    ir_source *source =
        push((void **)&target->sources, &target->source_count, sizeof *target->sources);
    if(source == NULL)
        return NULL;

    source->language = language;
    if(!set_string(&source->path, path)) {
        target->source_count--;
        return NULL;
    }
    return source;
}

bool ir_add_option(ir_option **array, size_t *count, const char *value, ir_scope scope) {
    if(array == NULL || count == NULL || value == NULL)
        return false;

    ir_option *option = push((void **)array, count, sizeof **array);
    if(option == NULL)
        return false;

    option->scope = scope;
    if(!set_string(&option->value, value)) {
        (*count)--;
        return false;
    }
    return true;
}

bool ir_add_include(ir_include **array, size_t *count, const char *value, ir_scope scope,
                    bool system) {
    if(array == NULL || count == NULL || value == NULL)
        return false;

    ir_include *include = push((void **)array, count, sizeof **array);
    if(include == NULL)
        return false;

    include->scope = scope;
    include->system = system;
    if(!set_string(&include->value, value)) {
        (*count)--;
        return false;
    }
    return true;
}

bool ir_set_artifact(ir_target *target, ir_target_kind kind, const char *path,
                     const char *install) {
    if(target == NULL || path == NULL)
        return false;
    if(!set_string(&target->artifact.path, path) || !set_string(&target->artifact.install, install))
        return false;
    target->artifact.kind = kind;
    target->has_artifact = true;
    return true;
}

ir_dependency *ir_add_dependency(ir_document *doc, const char *name, const char *version,
                                 ir_dep_origin origin, const char *root) {
    if(doc == NULL)
        return NULL;

    ir_dependency *dep =
        push((void **)&doc->dependencies, &doc->dependency_count, sizeof *doc->dependencies);
    if(dep == NULL)
        return NULL;

    dep->origin = origin;
    if(!set_string(&dep->name, name) || !set_string(&dep->version, version) ||
       !set_string(&dep->root, root)) {
        free_dependency(dep);
        doc->dependency_count--;
        return NULL;
    }
    return dep;
}

/* --- the wire: writing --- */

/* A single-element array rather than a scalar, and deliberately so: the day
   `[workspace]` is specified (RFC-0003) the schema widens to several projects
   without a revision. RFC-0013 fixes exactly one per document today. */
#define IR_PROJECTS_KEY "projects"

static void write_options(json_writer *writer, const char *key, const ir_option *options,
                          size_t count) {
    json_array_open(writer, key);
    for(size_t i = 0; i < count; i++) {
        json_object_open(writer, NULL);
        json_write_field(writer, "value", options[i].value);
        json_write_field(writer, "scope", ir_scope_name(options[i].scope));
        json_object_close(writer);
    }
    json_array_close(writer);
}

static void write_includes(json_writer *writer, const char *key, const ir_include *includes,
                           size_t count) {
    json_array_open(writer, key);
    for(size_t i = 0; i < count; i++) {
        json_object_open(writer, NULL);
        json_write_field(writer, "value", includes[i].value);
        json_write_field(writer, "scope", ir_scope_name(includes[i].scope));
        json_write_raw(writer, "system", includes[i].system ? "true" : "false");
        json_object_close(writer);
    }
    json_array_close(writer);
}

static void write_sources(json_writer *writer, const ir_target *target) {
    json_array_open(writer, "sources");
    for(size_t i = 0; i < target->source_count; i++) {
        const ir_source *source = &target->sources[i];
        json_object_open(writer, NULL);
        json_write_field(writer, "path", source->path);
        json_write_field(writer, "language", ir_language_name(source->language));
        write_options(writer, "options", source->options, source->option_count);
        json_object_close(writer);
    }
    json_array_close(writer);
}

static void write_target(json_writer *writer, const ir_target *target) {
    json_object_open(writer, NULL);
    json_write_field(writer, "name", target->name);
    json_write_field(writer, "kind", ir_target_kind_name(target->kind));
    write_sources(writer, target);
    write_options(writer, "options", target->options, target->option_count);
    write_includes(writer, "includes", target->includes, target->include_count);
    write_options(writer, "links", target->links, target->link_count);

    json_array_open(writer, "depends_on");
    for(size_t i = 0; i < str_list_count(&target->depends_on); i++)
        json_write_element(writer, str_list_get(&target->depends_on, i));
    json_array_close(writer);

    /* An absent artifact is omitted rather than written empty: what is not
       there and what is there and blank are different documents. */
    if(target->has_artifact) {
        json_object_open(writer, "artifact");
        json_write_field(writer, "kind", ir_target_kind_name(target->artifact.kind));
        json_write_field(writer, "path", target->artifact.path);
        if(target->artifact.install != NULL)
            json_write_field(writer, "install", target->artifact.install);
        json_object_close(writer);
    }
    json_object_close(writer);
}

static void write_dependency(json_writer *writer, const ir_dependency *dep) {
    json_object_open(writer, NULL);
    json_write_field(writer, "name", dep->name);
    /* A path dependency has no version — its bytes are whatever is on disk —
       and saying so by omission is what `molto metadata` already does. */
    if(dep->version != NULL)
        json_write_field(writer, "version", dep->version);
    json_write_field(writer, "origin", ir_dep_origin_name(dep->origin));
    json_write_field(writer, "root", dep->root);

    json_object_open(writer, "interface");
    write_includes(writer, "includes", dep->includes, dep->include_count);
    write_options(writer, "options", dep->options, dep->option_count);
    write_options(writer, "links", dep->links, dep->link_count);
    json_object_close(writer);
    json_object_close(writer);
}

bool ir_write(const ir_document *doc, FILE *stream) {
    if(doc == NULL || stream == NULL)
        return false;

    json_writer writer;
    json_writer_init(&writer, stream);

    json_object_open(&writer, NULL);

    char schema[32];
    snprintf(schema, sizeof schema, "%ld", doc->schema);
    json_write_raw(&writer, "schema", schema);

    /* Every file the frontend opened, at the document's own level rather than
       inside the project, because it describes the act of producing the
       document and not the thing described (RFC-0013). */
    json_array_open(&writer, "files_read");
    for(size_t i = 0; i < str_list_count(&doc->files_read); i++)
        json_write_element(&writer, str_list_get(&doc->files_read, i));
    json_array_close(&writer);

    json_array_open(&writer, IR_PROJECTS_KEY);
    json_object_open(&writer, NULL);
    json_write_field(&writer, "name", doc->name);
    json_write_field(&writer, "version", doc->version);
    json_write_field(&writer, "root", doc->root);
    json_write_field(&writer, "origin", doc->origin);

    json_array_open(&writer, "targets");
    for(size_t i = 0; i < doc->target_count; i++)
        write_target(&writer, &doc->targets[i]);
    json_array_close(&writer);

    json_array_open(&writer, "dependencies");
    for(size_t i = 0; i < doc->dependency_count; i++)
        write_dependency(&writer, &doc->dependencies[i]);
    json_array_close(&writer);

    json_object_close(&writer);
    json_array_close(&writer);
    json_object_close(&writer);
    json_writer_finish(&writer);

    /* The writer does not report, so the stream is what is asked. A document
       half-written is not a document, and a caller that ignored this would ship
       a truncated one as an answer. */
    return ferror(stream) == 0;
}
