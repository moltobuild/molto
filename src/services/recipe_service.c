#include <molto/services/recipe_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ARTIFACTS_SECTION "artifacts"

/* The document's own top level, where a recipe's coordinate lives. */
#define ROOT_SECTION ""

static bool set_error(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool set_error(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

/* --- the coordinate --- */

static bool read_required(doc_view doc, const char *key, char *out, size_t size, char *err,
                          size_t err_size) {
    if(!doc_get_string(doc, ROOT_SECTION, key, out, size))
        return set_error(err, err_size, "the recipe has no '%s'", key);
    return true;
}

static bool read_schema(doc_view doc, long *out, char *err, size_t err_size) {
    /* Absent means 1: the key is new, and the recipes published before it
       existed cannot be made to declare it retroactively. */
    if(!doc_get_int(doc, ROOT_SECTION, "schema", out)) {
        if(doc_has_key(doc, ROOT_SECTION, "schema"))
            return set_error(err, err_size, "the recipe's 'schema' must be a positive integer");
        *out = 1;
        return true;
    }
    if(*out < 1)
        return set_error(err, err_size, "the recipe's 'schema' must be a positive integer");
    if(*out > RECIPE_SCHEMA_MAX)
        return set_error(err, err_size,
                         "this molto reads recipe schema %d, and the recipe declares %ld; upgrade "
                         "molto",
                         RECIPE_SCHEMA_MAX, *out);
    return true;
}

/* Declared rather than inferred from which tables are present: a source recipe
   with a misspelled [souce] would, by inference, be a perfectly valid binary
   one whose archive merely went missing. */
static bool read_form(doc_view doc, recipe_form *out, char *err, size_t err_size) {
    char form[32];
    if(!doc_get_string(doc, ROOT_SECTION, "form", form, sizeof form)) {
        if(doc_has_key(doc, ROOT_SECTION, "form"))
            return set_error(err, err_size, "the recipe's 'form' must be a string");
        *out = recipe_form_binary;
        return true;
    }
    if(strcmp(form, "binary") == 0)
        *out = recipe_form_binary;
    else if(strcmp(form, "source") == 0)
        *out = recipe_form_source;
    else
        return set_error(err, err_size, "unknown recipe form '%s'", form);
    return true;
}

bool recipe_read_coordinate(doc_view doc, recipe_coordinate *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);

    return read_schema(doc, &out->schema, err, err_size) &&
           read_form(doc, &out->form, err, err_size) &&
           read_required(doc, "kind", out->kind, sizeof out->kind, err, err_size) &&
           read_required(doc, "name", out->name, sizeof out->name, err, err_size) &&
           read_required(doc, "version", out->version, sizeof out->version, err, err_size) &&
           read_required(doc, "target", out->target, sizeof out->target, err, err_size);
}

/* --- [artifacts] --- */

static const struct {
    const char *name;
    recipe_artifact_type type;
} ARTIFACT_TYPES[] = {
    {"source", recipe_artifact_source},
    {"static", recipe_artifact_static},
    {"shared", recipe_artifact_shared},
};

static bool read_type(doc_view doc, recipe_artifact_type *out, char *err, size_t err_size) {
    char name[32];
    if(!doc_get_string(doc, ARTIFACTS_SECTION, "type", name, sizeof name)) {
        if(doc_has_key(doc, ARTIFACTS_SECTION, "type"))
            return set_error(err, err_size, "[artifacts].type must be a string");
        return true; /* the default is already seeded */
    }

    for(size_t i = 0; i < sizeof ARTIFACT_TYPES / sizeof ARTIFACT_TYPES[0]; i++) {
        if(strcmp(ARTIFACT_TYPES[i].name, name) == 0) {
            *out = ARTIFACT_TYPES[i].type;
            return true;
        }
    }
    return set_error(err, err_size, "[artifacts].type '%s' is not source, static or shared", name);
}

bool recipe_read_artifacts(doc_view doc, recipe_artifacts *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    /* RFC-0009's default, seeded before reading so an absent key keeps it. */
    out->type = recipe_artifact_static;

    if(!doc_has_table(doc, ARTIFACTS_SECTION))
        return true;

    return read_type(doc, &out->type, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "sources", out->sources[0], RECIPE_MAX_SOURCES,
                            RECIPE_SOURCE_MAX, &out->source_count, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "exclude", out->exclude[0], RECIPE_MAX_SOURCES,
                            RECIPE_SOURCE_MAX, &out->exclude_count, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "link", out->link[0], PROJECT_MAX_LINK,
                            PROJECT_LINK_NAME_MAX, &out->link_count, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "defines", out->options.defines[0],
                            PROJECT_MAX_OPTS, PROJECT_OPT_LEN, &out->options.define_count, err,
                            err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "include", out->options.include[0],
                            PROJECT_MAX_OPTS, PROJECT_OPT_LEN, &out->options.include_count, err,
                            err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "flags", out->options.flags[0],
                            PROJECT_MAX_OPTS, PROJECT_OPT_LEN, &out->options.flag_count, err,
                            err_size);
}

static bool listed_in(const char list[][RECIPE_SOURCE_MAX], size_t count, const char *name) {
    for(size_t i = 0; i < count; i++) {
        if(strcmp(list[i], name) == 0)
            return true;
    }
    return false;
}

bool recipe_artifacts_wants(const recipe_artifacts *artifacts, const char *name) {
    /* An empty `sources` means every source the drop happens to contain, which
       is what a recipe that never needed to narrow anything says by omission. */
    if(artifacts->source_count > 0 && !listed_in(artifacts->sources, artifacts->source_count, name))
        return false;
    return !listed_in(artifacts->exclude, artifacts->exclude_count, name);
}
