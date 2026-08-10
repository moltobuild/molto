#include <molto/util/doc.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Longest dotted table path this reader walks. Matches the TOML side's own
   limit, because a path that a section name cannot hold is a path no document
   can declare. */
#define DOC_PATH_MAX TOML_SECTION_MAX

doc_view doc_from_toml(const toml_document *doc) {
    doc_view view = {.backend = doc_backend_toml, .toml = doc, .json = {NULL, 0}};
    return view;
}

doc_view doc_from_json(json_value object) {
    doc_view view = {.backend = doc_backend_json, .toml = NULL, .json = object};
    return view;
}

/* --- the JSON side --- */

/* The object a dotted path names, starting from the document's root. An empty
   path is the root itself, which is where a recipe's own coordinate lives. */
static json_value json_table_at(json_value root, const char *table) {
    if(table == NULL || table[0] == '\0')
        return root;

    json_value current = root;
    for(const char *segment = table; *segment != '\0';) {
        const size_t length = strcspn(segment, ".");
        char name[DOC_PATH_MAX];
        if(length == 0 || length >= sizeof name)
            return (json_value){NULL, 0};

        snprintf(name, sizeof name, "%.*s", (int)length, segment);
        current = json_get(current, name);

        segment += length;
        if(*segment == '.')
            segment++;
    }
    return current;
}

static json_value json_member(doc_view doc, const char *table, const char *key) {
    return json_get(json_table_at(doc.json, table), key);
}

/* --- reading --- */

bool doc_has_table(doc_view doc, const char *table) {
    if(doc.backend == doc_backend_toml) {
        /* The root always exists as a table, and toml_has_section says nothing
           useful about "": a document with only root keys stores them under an
           empty section, and one with none is still a document. */
        return table == NULL || table[0] == '\0' ? doc.toml != NULL
                                                 : toml_has_section(doc.toml, table);
    }
    return json_type_of(json_table_at(doc.json, table)) == json_type_object;
}

bool doc_has_key(doc_view doc, const char *table, const char *key) {
    if(doc.backend == doc_backend_json)
        return json_is_valid(json_member(doc, table, key));

    /* The TOML side has no typeless probe, so the section's key list is the
       one place that answers without caring what the value turned out to be. */
    str_list keys;
    str_list_init(&keys);
    bool found = false;
    if(toml_section_keys(doc.toml, table == NULL ? "" : table, &keys)) {
        for(size_t i = 0; !found && i < str_list_count(&keys); i++)
            found = strcmp(str_list_get(&keys, i), key) == 0;
    }
    str_list_free(&keys);
    return found;
}

bool doc_get_string(doc_view doc, const char *table, const char *key, char *out, size_t out_size) {
    if(doc.backend == doc_backend_toml)
        return toml_get_string(doc.toml, table == NULL ? "" : table, key, out, out_size);

    const char *text = json_string(json_member(doc, table, key));
    if(text == NULL || strlen(text) >= out_size)
        return false;
    snprintf(out, out_size, "%s", text);
    return true;
}

bool doc_get_int(doc_view doc, const char *table, const char *key, long *out) {
    if(doc.backend == doc_backend_toml)
        return toml_get_int(doc.toml, table == NULL ? "" : table, key, out);

    long long wide = 0;
    if(!json_number(json_member(doc, table, key), &wide))
        return false;
    /* Refused rather than narrowed, for the same reason json_number refuses to
       hand back an int: a reader that silently truncates is worse than one
       that will not fit. */
    if(wide < LONG_MIN || wide > LONG_MAX)
        return false;
    *out = (long)wide;
    return true;
}

bool doc_get_bool(doc_view doc, const char *table, const char *key, bool *out) {
    if(doc.backend == doc_backend_toml)
        return toml_get_bool(doc.toml, table == NULL ? "" : table, key, out);
    return json_bool(json_member(doc, table, key), out);
}

bool doc_get_array(doc_view doc, const char *table, const char *key, str_list *out) {
    if(doc.backend == doc_backend_toml)
        return toml_get_array(doc.toml, table == NULL ? "" : table, key, out);

    const json_value array = json_member(doc, table, key);
    if(json_type_of(array) != json_type_array)
        return false;

    const size_t total = json_count(array);
    for(size_t i = 0; i < total; i++) {
        const char *text = json_string(json_at(array, i));
        /* One element that is not a string fails the whole list: half a list
           is worse than none, because half a list compiles. */
        if(text == NULL || !str_list_push(out, text))
            return false;
    }
    return true;
}

bool doc_table_members(doc_view doc, const char *table, str_list *out) {
    if(doc.backend == doc_backend_toml)
        return toml_section_members(doc.toml, table == NULL ? "" : table, out);

    const json_value object = json_table_at(doc.json, table);
    if(json_type_of(object) != json_type_object)
        return true; /* an undeclared table has no members, and that is not an error */

    const size_t total = json_count(object);
    for(size_t i = 0; i < total; i++) {
        const char *name = json_key_at(object, i);
        if(name == NULL || !str_list_push(out, name))
            return false;
    }
    return true;
}

bool doc_read_strings(doc_view doc, const char *table, const char *key, char *dest, size_t capacity,
                      size_t stride, size_t *count, char *err, size_t err_size) {
    str_list values;
    str_list_init(&values);

    if(!doc_get_array(doc, table, key, &values)) {
        str_list_free(&values);
        /* Absent is fine; declared and not a list of strings is not. Without
           the probe the two look alike, and the second one would compile
           nothing without saying so. */
        if(!doc_has_key(doc, table, key))
            return true;
        if(err != NULL && err_size > 0)
            snprintf(err, err_size, "[%s].%s must be a list of strings", table == NULL ? "" : table,
                     key);
        return false;
    }

    bool ok = true;
    const size_t total = str_list_count(&values);
    for(size_t i = 0; ok && i < total; i++) {
        const char *value = str_list_get(&values, i);
        if(*count >= capacity) {
            if(err != NULL && err_size > 0)
                snprintf(err, err_size, "[%s].%s has more than %zu entries",
                         table == NULL ? "" : table, key, capacity);
            ok = false;
        } else if(strlen(value) >= stride) {
            if(err != NULL && err_size > 0)
                snprintf(err, err_size, "[%s].%s entry '%s' is longer than %zu characters",
                         table == NULL ? "" : table, key, value, stride - 1);
            ok = false;
        } else {
            snprintf(dest + (*count * stride), stride, "%s", value);
            (*count)++;
        }
    }

    str_list_free(&values);
    return ok;
}
