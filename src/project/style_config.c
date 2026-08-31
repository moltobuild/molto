#include <molto/project/style_config.h>

#include <molto/services/fs_service.h>
#include <molto/util/glob.h>
#include <molto/util/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The two files, at the workspace root beside Project.toml. */
#define FORMAT_FILENAME "format.json"
#define LINTER_FILENAME "linter.json"

/* Keys both files share. */
#define KEY_BACKEND "backend"
#define KEY_PRESET "preset"
#define KEY_EXCLUDE "exclude"

/* format.json. */
#define KEY_STYLE "style"
#define KEY_INDENT_WIDTH "indent_width"
#define KEY_USE_TABS "use_tabs"
#define KEY_LINE_WIDTH "line_width"
#define KEY_BRACE_STYLE "brace_style"
#define KEY_POINTER_ALIGNMENT "pointer_alignment"
#define KEY_SORT_INCLUDES "sort_includes"
#define KEY_SPACE_BEFORE_PAREN "space_before_paren"
#define KEY_COLUMN_LIMIT_COMMENTS "column_limit_comments"

/* linter.json. */
#define KEY_RULES "rules"

/* The defaults of RFC-0005. */
#define DEFAULT_INDENT_WIDTH 4
#define DEFAULT_LINE_WIDTH 100

/* A width outside this is a mistake, not a preference. */
#define WIDTH_MIN 1
#define WIDTH_MAX 512

/* Suffix that should also cover the directory it names, not only what is
   under it. */
#define RECURSIVE_SUFFIX "/**"

static void set_err(char *err, size_t size, const char *file, const char *reason,
                    const char *what) {
    if(err == NULL || size == 0)
        return;
    if(what != NULL)
        snprintf(err, size, "%s: %s '%s'", file, reason, what);
    else
        snprintf(err, size, "%s: %s", file, reason);
}

void style_config_defaults(style_config *out) {
    memset(out, 0, sizeof *out);
    out->preset = style_preset_molto;
    out->style.indent_width = DEFAULT_INDENT_WIDTH;
    out->style.use_tabs = false;
    out->style.line_width = DEFAULT_LINE_WIDTH;
    out->style.braces = brace_style_attach;
    out->style.pointers = pointer_alignment_right;
    out->style.sort_includes = true;
    out->style.space_before_paren = false;
    out->style.column_limit_comments = true;
}

void lint_config_defaults(lint_config *out) {
    memset(out, 0, sizeof *out);
    out->preset = style_preset_molto;
}

bool style_excludes_match(const style_excludes *excludes, const char *relative_path) {
    for(size_t i = 0; i < excludes->exclude_count; i++) {
        const char *pattern = excludes->exclude[i];
        if(glob_match(pattern, relative_path))
            return true;

        /* A globstar pattern should cover the directory it names too, which the
           pattern alone does not match. */
        size_t length = strlen(pattern);
        size_t suffix = strlen(RECURSIVE_SUFFIX);
        if(length > suffix && strcmp(pattern + length - suffix, RECURSIVE_SUFFIX) == 0) {
            char directory[STYLE_EXCLUDE_MAX];
            snprintf(directory, sizeof directory, "%.*s", (int)(length - suffix), pattern);
            if(glob_match(directory, relative_path))
                return true;
        }
    }
    return false;
}

/* --- shared readers --- */

/* Read a string member, refusing a value of the wrong type rather than
   ignoring it. Absent leaves `out` untouched. */
static bool read_string(json_value root, const char *key, const char *file, char *out,
                        size_t out_size, char *err, size_t err_size) {
    json_value value = json_get(root, key);
    if(!json_is_valid(value))
        return true;
    const char *text = json_string(value);
    if(text == NULL) {
        set_err(err, err_size, file, "expected a string for", key);
        return false;
    }
    if(!fs_format_path(out, out_size, "%s", text)) {
        set_err(err, err_size, file, "value too long for", key);
        return false;
    }
    return true;
}

static bool read_bool(json_value root, const char *key, const char *file, bool *out, char *err,
                      size_t err_size) {
    json_value value = json_get(root, key);
    if(!json_is_valid(value))
        return true;
    if(!json_bool(value, out)) {
        set_err(err, err_size, file, "expected true or false for", key);
        return false;
    }
    return true;
}

static bool read_width(json_value root, const char *key, const char *file, int *out, char *err,
                       size_t err_size) {
    json_value value = json_get(root, key);
    if(!json_is_valid(value))
        return true;
    long long number = 0;
    if(!json_number(value, &number) || number < WIDTH_MIN || number > WIDTH_MAX) {
        set_err(err, err_size, file, "expected a column count for", key);
        return false;
    }
    *out = (int)number;
    return true;
}

/* Map a name onto a value from a table, so an unknown one is refused by name
   instead of falling back to a default the user did not ask for. */
typedef struct {
    const char *name;
    int value;
} name_map;

static bool read_mapped(json_value root, const char *key, const char *file, const name_map *names,
                        size_t name_count, int *out, char *err, size_t err_size) {
    char text[STYLE_BACKEND_MAX] = "";
    if(!read_string(root, key, file, text, sizeof text, err, err_size))
        return false;
    if(text[0] == '\0')
        return true; /* absent */

    for(size_t i = 0; i < name_count; i++) {
        if(strcmp(names[i].name, text) == 0) {
            *out = names[i].value;
            return true;
        }
    }
    set_err(err, err_size, file, "unknown value for", text);
    return false;
}

/* The presets RFC-0005 names. Two are specified but not implemented; saying so
   by name beats silently formatting as something else. */
static bool read_preset(json_value root, const char *file, style_preset *out, char *err,
                        size_t err_size) {
    char text[STYLE_BACKEND_MAX] = "";
    if(!read_string(root, KEY_PRESET, file, text, sizeof text, err, err_size))
        return false;
    if(text[0] == '\0')
        return true;

    if(strcmp(text, "molto") == 0) {
        *out = style_preset_molto;
        return true;
    }
    if(strcmp(text, "none") == 0) {
        *out = style_preset_none;
        return true;
    }
    if(strcmp(text, "kernel") == 0 || strcmp(text, "gnu") == 0) {
        set_err(err, err_size, file,
                "preset is not implemented yet; use \"molto\" "
                "or \"none\":",
                text);
        return false;
    }
    set_err(err, err_size, file, "unknown preset", text);
    return false;
}

static bool read_excludes(json_value root, const char *file, style_excludes *out, char *err,
                          size_t err_size) {
    json_value list = json_get(root, KEY_EXCLUDE);
    if(!json_is_valid(list))
        return true;
    if(json_type_of(list) != json_type_array) {
        set_err(err, err_size, file, "expected a list for", KEY_EXCLUDE);
        return false;
    }

    size_t count = json_count(list);
    if(count > STYLE_MAX_EXCLUDES) {
        set_err(err, err_size, file, "too many exclude patterns", NULL);
        return false;
    }
    for(size_t i = 0; i < count; i++) {
        const char *pattern = json_string(json_at(list, i));
        if(pattern == NULL) {
            set_err(err, err_size, file, "expected a string in", KEY_EXCLUDE);
            return false;
        }
        if(!fs_format_path(out->exclude[i], STYLE_EXCLUDE_MAX, "%s", pattern)) {
            set_err(err, err_size, file, "exclude pattern too long", pattern);
            return false;
        }
    }
    out->exclude_count = count;
    return true;
}

/* Refuse a top-level key that is not in `known`. A key that does nothing in
   silence is what fail-closed exists to prevent. */
static bool refuse_unknown_keys(json_value root, const char *file, const char *const *known,
                                size_t known_count, char *err, size_t err_size) {
    size_t count = json_count(root);
    for(size_t i = 0; i < count; i++) {
        const char *key = json_key_at(root, i);
        if(key == NULL)
            continue;
        bool recognised = false;
        for(size_t k = 0; !recognised && k < known_count; k++)
            recognised = strcmp(key, known[k]) == 0;
        if(!recognised) {
            set_err(err, err_size, file, "unknown key", key);
            return false;
        }
    }
    return true;
}

/* Parse a document, or report why it is not one. */
static json_document *parse_document(const char *json, const char *file, json_value *root,
                                     char *err, size_t err_size) {
    json_document *doc = json_parse(json);
    if(doc == NULL) {
        set_err(err, err_size, file, "is not valid JSON", NULL);
        return NULL;
    }
    *root = json_root(doc);
    if(json_type_of(*root) != json_type_object) {
        set_err(err, err_size, file, "must hold an object", NULL);
        json_free(doc);
        return NULL;
    }
    return doc;
}

/* --- format.json --- */

static const name_map brace_names[] = {
    {"attach", brace_style_attach},
    {"break", brace_style_break},
    {"linux", brace_style_linux},
    {"allman", brace_style_allman},
};

static const name_map pointer_names[] = {
    {"left", pointer_alignment_left},
    {"right", pointer_alignment_right},
};

static bool read_style(json_value root, const char *file, style_options *out, char *err,
                       size_t err_size) {
    json_value style = json_get(root, KEY_STYLE);
    if(!json_is_valid(style))
        return true;
    if(json_type_of(style) != json_type_object) {
        set_err(err, err_size, file, "expected an object for", KEY_STYLE);
        return false;
    }

    static const char *const known[] = {
        KEY_INDENT_WIDTH,      KEY_USE_TABS,      KEY_LINE_WIDTH,         KEY_BRACE_STYLE,
        KEY_POINTER_ALIGNMENT, KEY_SORT_INCLUDES, KEY_SPACE_BEFORE_PAREN, KEY_COLUMN_LIMIT_COMMENTS,
    };
    if(!refuse_unknown_keys(style, file, known, sizeof known / sizeof known[0], err, err_size))
        return false;

    int braces = (int)out->braces;
    int pointers = (int)out->pointers;
    bool ok =
        read_width(style, KEY_INDENT_WIDTH, file, &out->indent_width, err, err_size) &&
        read_width(style, KEY_LINE_WIDTH, file, &out->line_width, err, err_size) &&
        read_bool(style, KEY_USE_TABS, file, &out->use_tabs, err, err_size) &&
        read_bool(style, KEY_SORT_INCLUDES, file, &out->sort_includes, err, err_size) &&
        read_bool(style, KEY_SPACE_BEFORE_PAREN, file, &out->space_before_paren, err, err_size) &&
        read_bool(style, KEY_COLUMN_LIMIT_COMMENTS, file, &out->column_limit_comments, err,
                  err_size) &&
        read_mapped(style, KEY_BRACE_STYLE, file, brace_names,
                    sizeof brace_names / sizeof brace_names[0], &braces, err, err_size) &&
        read_mapped(style, KEY_POINTER_ALIGNMENT, file, pointer_names,
                    sizeof pointer_names / sizeof pointer_names[0], &pointers, err, err_size);
    if(!ok)
        return false;
    out->braces = (brace_style)braces;
    out->pointers = (pointer_alignment)pointers;
    return true;
}

bool style_config_parse(const char *json, style_config *out, char *err, size_t err_size) {
    json_value root;
    json_document *doc = parse_document(json, FORMAT_FILENAME, &root, err, err_size);
    if(doc == NULL)
        return false;

    static const char *const known[] = {KEY_BACKEND, KEY_PRESET, KEY_EXCLUDE, KEY_STYLE};
    bool ok = refuse_unknown_keys(root, FORMAT_FILENAME, known, sizeof known / sizeof known[0], err,
                                  err_size) &&
              read_string(root, KEY_BACKEND, FORMAT_FILENAME, out->backend, sizeof out->backend,
                          err, err_size) &&
              read_preset(root, FORMAT_FILENAME, &out->preset, err, err_size) &&
              read_excludes(root, FORMAT_FILENAME, &out->paths, err, err_size) &&
              read_style(root, FORMAT_FILENAME, &out->style, err, err_size);

    json_free(doc);
    return ok;
}

/* --- linter.json --- */

static bool read_severity(const char *text, lint_severity *out) {
    if(strcmp(text, "off") == 0)
        *out = lint_severity_off;
    else if(strcmp(text, "warn") == 0)
        *out = lint_severity_warn;
    else if(strcmp(text, "error") == 0)
        *out = lint_severity_error;
    else
        return false;
    return true;
}

/* The severity map names rules Molto does not know in advance, so this walks
   the members rather than asking for keys. */
static bool read_rules(json_value root, lint_config *out, char *err, size_t err_size) {
    json_value rules = json_get(root, KEY_RULES);
    if(!json_is_valid(rules))
        return true;
    if(json_type_of(rules) != json_type_object) {
        set_err(err, err_size, LINTER_FILENAME, "expected an object for", KEY_RULES);
        return false;
    }

    size_t count = json_count(rules);
    if(count > LINT_MAX_RULES) {
        set_err(err, err_size, LINTER_FILENAME, "too many rules", NULL);
        return false;
    }
    for(size_t i = 0; i < count; i++) {
        const char *name = json_key_at(rules, i);
        const char *severity = json_string(json_member_at(rules, i));
        if(name == NULL || severity == NULL) {
            set_err(err, err_size, LINTER_FILENAME,
                    "expected \"off\", \"warn\" or \"error\" for rule",
                    name != NULL ? name : KEY_RULES);
            return false;
        }
        if(!fs_format_path(out->rules[i].name, LINT_RULE_NAME_MAX, "%s", name)) {
            set_err(err, err_size, LINTER_FILENAME, "rule name too long", name);
            return false;
        }
        if(!read_severity(severity, &out->rules[i].severity)) {
            set_err(err, err_size, LINTER_FILENAME,
                    "severity must be \"off\", \"warn\" or \"error\", not", severity);
            return false;
        }
    }
    out->rule_count = count;
    return true;
}

bool lint_config_parse(const char *json, lint_config *out, char *err, size_t err_size) {
    json_value root;
    json_document *doc = parse_document(json, LINTER_FILENAME, &root, err, err_size);
    if(doc == NULL)
        return false;

    static const char *const known[] = {KEY_BACKEND, KEY_PRESET, KEY_EXCLUDE, KEY_RULES};
    bool ok = refuse_unknown_keys(root, LINTER_FILENAME, known, sizeof known / sizeof known[0], err,
                                  err_size) &&
              read_string(root, KEY_BACKEND, LINTER_FILENAME, out->backend, sizeof out->backend,
                          err, err_size) &&
              read_preset(root, LINTER_FILENAME, &out->preset, err, err_size) &&
              read_excludes(root, LINTER_FILENAME, &out->paths, err, err_size) &&
              read_rules(root, out, err, err_size);

    json_free(doc);
    return ok;
}

/* --- loading --- */

/* Read a configuration file, if it is there. Absent means the defaults, which
   is why a missing file leaves `out` as seeded and succeeds. */
static bool load_file(const char *root, const char *filename, char **text_out, char *err,
                      size_t err_size) {
    char path[4096];
    if(!fs_format_path(path, sizeof path, "%s/%s", root, filename)) {
        set_err(err, err_size, filename, "path too long to compose", root);
        return false;
    }
    if(!fs_path_exists(path)) {
        *text_out = NULL;
        return true;
    }
    *text_out = fs_read_file(path);
    if(*text_out == NULL) {
        set_err(err, err_size, filename, "could not be read", NULL);
        return false;
    }
    return true;
}

bool style_config_load(const char *root, style_config *out, char *err, size_t err_size) {
    style_config_defaults(out);
    char *text = NULL;
    if(!load_file(root, FORMAT_FILENAME, &text, err, err_size))
        return false;
    if(text == NULL)
        return true;
    bool ok = style_config_parse(text, out, err, err_size);
    free(text);
    return ok;
}

bool lint_config_load(const char *root, lint_config *out, char *err, size_t err_size) {
    lint_config_defaults(out);
    char *text = NULL;
    if(!load_file(root, LINTER_FILENAME, &text, err, err_size))
        return false;
    if(text == NULL)
        return true;
    bool ok = lint_config_parse(text, out, err, err_size);
    free(text);
    return ok;
}
