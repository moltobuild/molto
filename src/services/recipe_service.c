#include <molto/services/recipe_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ARTIFACTS_SECTION "artifacts"

/* What a package compiles itself with and nobody else sees. A dotted path in
   both encodings: a TOML `[artifacts.private]` header and a JSON object nested
   inside `artifacts` read back the same way. */
#define ARTIFACTS_PRIVATE_SECTION "artifacts.private"

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

/*
 * The language standards molto will put behind `-std=`.
 *
 * Checked here rather than left to the compiler because of who writes a recipe
 * and who reads it. A misspelled `[target].std` in a manifest fails in the
 * build of the person who typed it, which is the shortest feedback loop there
 * is; a misspelled `std` in a published recipe fails in the build of everyone
 * who depends on it, and the message names a compiler option none of them
 * wrote. So the manifest is taken at its word and a recipe is not.
 *
 * Two lists rather than one, so a `c++20` written under `std` is caught as
 * well: it is a real standard and still the wrong answer to that key.
 */
static const char *const C_STANDARDS[] = {
    "c89",   "c90",   "c94",   "c99",   "c11",   "c17",   "c18",   "c23",   "c2x",
    "gnu89", "gnu90", "gnu99", "gnu11", "gnu17", "gnu18", "gnu23", "gnu2x",
};

static const char *const CPP_STANDARDS[] = {
    "c++98",   "c++03",   "c++11",   "c++14",   "c++17",   "c++20",   "c++23",
    "c++26",   "c++2a",   "c++2b",   "c++2c",   "gnu++98", "gnu++03", "gnu++11",
    "gnu++14", "gnu++17", "gnu++20", "gnu++23", "gnu++26",
};

/* One standard, against the list for its language. An absent key leaves `out`
   alone, and `out` starts empty — which is what says "whatever the consumer
   compiles with". */
static bool read_std(doc_view doc, const char *key, const char *const *known, size_t count,
                     char *out, size_t out_size, char *err, size_t err_size) {
    /* Read into more room than a standard needs, so an overlong value is
       reported as what was written rather than as a truncation of it. */
    char value[RECIPE_STD_MAX * 2];
    if(!doc_get_string(doc, ARTIFACTS_SECTION, key, value, sizeof value)) {
        if(doc_has_key(doc, ARTIFACTS_SECTION, key))
            return set_error(err, err_size, "[artifacts].%s must be a string", key);
        return true;
    }

    for(size_t i = 0; i < count; i++) {
        if(strcmp(known[i], value) == 0) {
            /* The match is what proves this fits — every entry in `known` is a
               standard's name, and `out` is sized for one. The bound is spelled
               out anyway because the reader of this line cannot see the table
               from here, and neither can the compiler. */
            snprintf(out, out_size, "%.*s", (int)(out_size - 1), value);
            return true;
        }
    }
    return set_error(err, err_size,
                     "[artifacts].%s '%s' is not a language standard molto knows about", key,
                     value);
}

/* The three option lists a table carries. `[artifacts]` and
   `[artifacts.private]` hold the same ones: what separates them is who they
   reach, not what they can say. */
static bool read_options(doc_view doc, const char *table, project_options *out, char *err,
                         size_t err_size) {
    return doc_read_strings(doc, table, "defines", out->defines[0], PROJECT_MAX_OPTS,
                            PROJECT_OPT_LEN, &out->define_count, err, err_size) &&
           doc_read_strings(doc, table, "include", out->include[0], PROJECT_MAX_OPTS,
                            PROJECT_OPT_LEN, &out->include_count, err, err_size) &&
           doc_read_strings(doc, table, "flags", out->flags[0], PROJECT_MAX_OPTS, PROJECT_OPT_LEN,
                            &out->flag_count, err, err_size);
}

bool recipe_read_artifacts(doc_view doc, recipe_artifacts *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    /* RFC-0009's default, seeded before reading so an absent key keeps it. */
    out->type = recipe_artifact_static;

    /* Either table on its own is enough. A recipe whose only statement is a
       private flag declares nothing directly under `[artifacts]`, and asking
       about that table alone would skip the whole read in silence. */
    if(!doc_has_table(doc, ARTIFACTS_SECTION) && !doc_has_table(doc, ARTIFACTS_PRIVATE_SECTION))
        return true;

    return read_type(doc, &out->type, err, err_size) &&
           read_std(doc, "std", C_STANDARDS, sizeof C_STANDARDS / sizeof C_STANDARDS[0], out->std,
                    sizeof out->std, err, err_size) &&
           read_std(doc, "cpp_std", CPP_STANDARDS, sizeof CPP_STANDARDS / sizeof CPP_STANDARDS[0],
                    out->cpp_std, sizeof out->cpp_std, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "sources", out->sources[0], RECIPE_MAX_SOURCES,
                            RECIPE_SOURCE_MAX, &out->source_count, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "exclude", out->exclude[0], RECIPE_MAX_SOURCES,
                            RECIPE_SOURCE_MAX, &out->exclude_count, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "link", out->link[0], PROJECT_MAX_LINK,
                            PROJECT_LINK_NAME_MAX, &out->link_count, err, err_size) &&
           read_options(doc, ARTIFACTS_SECTION, &out->options, err, err_size) &&
           read_options(doc, ARTIFACTS_PRIVATE_SECTION, &out->private_options, err, err_size);
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
