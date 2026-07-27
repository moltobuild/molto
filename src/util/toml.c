/*
 * toml — a small, safe TOML parser.
 *
 * Design, in plain terms:
 *   - We parse the whole document once into a flat list of "entries". Each entry
 *     is one (section, key) pair with a typed value (string, integer or bool).
 *     For example the line `opt_level = 3` under `[profile.release]` becomes the
 *     entry { section="profile.release", key="opt_level", int 3 }.
 *   - Parsing walks the text one line at a time. For each line we:
 *       1. cut off any inline `# comment` (but not a `#` inside quotes),
 *       2. trim surrounding whitespace,
 *       3. if it is `[section]` update the current section,
 *          otherwise split `key = value` and store a typed entry.
 *   - It "fails closed": any malformed input aborts the whole parse and returns
 *     NULL together with a "Project.toml:<line>: <reason>" message, instead of
 *     silently guessing. Nothing is ever half-parsed.
 *
 * The parser never modifies the caller's input text; it works on local copies.
 * On top of the flat model there are two ways to read values: dictionary-style
 * getters (toml_get_*) and schema binding into a struct (toml_bind).
 */

#include <molto/util/toml.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Capacity limits for the fields of a single entry. A section/key/value that
 * exceeds its limit is a parse error (never silently truncated). */
#define TOML_SECTION_MAX 128
#define TOML_KEY_MAX     64
#define TOML_VALUE_MAX   256
#define TOML_LINE_MAX    1024

/* Scratch buffer size for the digits of one integer value. */
#define TOML_DIGITS_MAX  64

/* strtol base for integer values (plain decimal). */
#define TOML_INTEGER_BASE 10

/* Growth policy for the document's entry array. */
#define TOML_DOC_INITIAL_CAPACITY 16
#define TOML_DOC_GROWTH_FACTOR    2

typedef struct {
    char section[TOML_SECTION_MAX];
    char key[TOML_KEY_MAX];
    toml_field_type type;
    union {
        char str[TOML_VALUE_MAX];
        long integer;
        bool boolean;
    } value;
} toml_entry;

struct toml_document {
    toml_entry *items;
    size_t count;
    size_t capacity;
};

static void set_err(char *err, size_t size, int line,
                    const char *reason, const char *text) {
    if (err == NULL || size == 0)
        return;
    if (line > 0 && text != NULL)
        snprintf(err, size, "Project.toml:%d: %s: '%s'", line, reason, text);
    else if (line > 0)
        snprintf(err, size, "Project.toml:%d: %s", line, reason);
    else if (text != NULL)
        snprintf(err, size, "%s: '%s'", reason, text);
    else
        snprintf(err, size, "%s", reason);
}

static char *trim(char *text) {
    while (*text == ' ' || *text == '\t')
        text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t'
                       || end[-1] == '\r' || end[-1] == '\n'))
        end--;
    *end = '\0';
    return text;
}

static bool is_bare_char(char c, bool allow_dot) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-'
        || (allow_dot && c == '.');
}

/* Cut the line at the first '#' that is not inside a string. */
static void strip_inline_comment(char *line) {
    bool in_basic = false;
    bool in_literal = false;
    for (char *p = line; *p != '\0'; p++) {
        char c = *p;
        if (in_basic) {
            if (c == '\\' && p[1] != '\0')
                p++;
            else if (c == '"')
                in_basic = false;
        } else if (in_literal) {
            if (c == '\'')
                in_literal = false;
        } else if (c == '"') {
            in_basic = true;
        } else if (c == '\'') {
            in_literal = true;
        } else if (c == '#') {
            *p = '\0';
            return;
        }
    }
}

static bool parse_basic_string(const char *text, char *out, size_t out_size,
                               const char **end) {
    size_t o = 0;
    const char *p = text + 1;
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char escaped = *p++;
            switch (escaped) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                default:   return false; /* dangling or unsupported escape */
            }
        }
        if (o + 1 >= out_size)
            return false; /* value too long */
        out[o++] = c;
    }
    if (*p != '"')
        return false; /* unterminated */
    out[o] = '\0';
    *end = p + 1;
    return true;
}

static bool parse_literal_string(const char *text, char *out, size_t out_size,
                                 const char **end) {
    size_t o = 0;
    const char *p = text + 1;
    while (*p != '\0' && *p != '\'') {
        if (o + 1 >= out_size)
            return false;
        out[o++] = *p++;
    }
    if (*p != '\'')
        return false;
    out[o] = '\0';
    *end = p + 1;
    return true;
}

/* Parse a decimal integer, allowing a leading sign and '_' digit separators
 * (e.g. "1_000", "-42"). Rejects overflow and any non-digit garbage. */
static bool parse_integer(const char *text, long *out) {
    char digits[TOML_DIGITS_MAX];
    size_t count = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '_')
            continue; /* separators are cosmetic; drop them */
        if (count + 1 >= sizeof digits)
            return false;
        digits[count++] = *p;
    }
    digits[count] = '\0';
    if (count == 0)
        return false;
    errno = 0;
    char *endptr;
    long value = strtol(digits, &endptr, TOML_INTEGER_BASE);
    if (errno == ERANGE || *endptr != '\0')
        return false; /* out of range, or not a pure integer */
    *out = value;
    return true;
}

/* Append a parsed entry to the document, growing the array as needed. */
static bool doc_push(toml_document *doc, const toml_entry *entry,
                     char *err, size_t err_size) {
    if (doc->count == doc->capacity) {
        size_t next = doc->capacity == 0 ? TOML_DOC_INITIAL_CAPACITY
                                         : doc->capacity * TOML_DOC_GROWTH_FACTOR;
        toml_entry *items = realloc(doc->items, next * sizeof(toml_entry));
        if (items == NULL) {
            set_err(err, err_size, 0, "out of memory", NULL);
            return false;
        }
        doc->items = items;
        doc->capacity = next;
    }
    doc->items[doc->count++] = *entry;
    return true;
}

static bool parse_header(char *text, char *section, size_t size,
                         char *err, size_t err_size, int line) {
    size_t len = strlen(text);
    if (text[len - 1] != ']') {
        set_err(err, err_size, line, "missing ']' in section header", text);
        return false;
    }
    text[len - 1] = '\0';
    char *inside = trim(text + 1);
    if (inside[0] == '\0') {
        set_err(err, err_size, line, "empty section header", NULL);
        return false;
    }
    if (strlen(inside) >= size) {
        set_err(err, err_size, line, "section name too long", inside);
        return false;
    }
    for (char *p = inside; *p != '\0'; p++) {
        if (!is_bare_char(*p, true)) {
            set_err(err, err_size, line, "invalid character in section name", inside);
            return false;
        }
    }
    snprintf(section, size, "%s", inside);
    return true;
}

static bool parse_key_value(toml_document *doc, const char *section, char *text,
                            char *err, size_t err_size, int line) {
    char *equals = strchr(text, '=');
    if (equals == NULL) {
        set_err(err, err_size, line, "expected '='", text);
        return false;
    }
    *equals = '\0';
    char *key = trim(text);
    char *value = trim(equals + 1);
    if (key[0] == '\0') {
        set_err(err, err_size, line, "empty key", NULL);
        return false;
    }
    if (strlen(key) >= TOML_KEY_MAX) {
        set_err(err, err_size, line, "key too long", key);
        return false;
    }
    for (char *p = key; *p != '\0'; p++) {
        if (!is_bare_char(*p, false)) {
            set_err(err, err_size, line, "invalid character in key", key);
            return false;
        }
    }
    if (value[0] == '\0') {
        set_err(err, err_size, line, "missing value", key);
        return false;
    }

    toml_entry entry;
    memset(&entry, 0, sizeof entry);
    snprintf(entry.section, sizeof entry.section, "%s", section);
    snprintf(entry.key, sizeof entry.key, "%s", key);

    if (value[0] == '"' || value[0] == '\'') {
        const char *end = value;
        bool ok = value[0] == '"'
            ? parse_basic_string(value, entry.value.str, sizeof entry.value.str, &end)
            : parse_literal_string(value, entry.value.str, sizeof entry.value.str, &end);
        if (!ok) {
            set_err(err, err_size, line, "invalid string value", key);
            return false;
        }
        if (*trim((char *)end) != '\0') {
            set_err(err, err_size, line, "trailing characters after value", key);
            return false;
        }
        entry.type = toml_field_string;
    } else if (value[0] == '[' || value[0] == '{') {
        /* Array or inline table: recognized but unsupported. Skip it so a
           [deps] section does not break parsing. */
        return true;
    } else if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
        entry.type = toml_field_bool;
        entry.value.boolean = strcmp(value, "true") == 0;
    } else {
        long integer;
        if (!parse_integer(value, &integer)) {
            set_err(err, err_size, line, "invalid value", value);
            return false;
        }
        entry.type = toml_field_int;
        entry.value.integer = integer;
    }
    return doc_push(doc, &entry, err, err_size);
}

toml_document *toml_parse(const char *text, char *err, size_t err_size) {
    if (text == NULL) {
        set_err(err, err_size, 0, "null input", NULL);
        return NULL;
    }
    toml_document *doc = calloc(1, sizeof *doc);
    if (doc == NULL) {
        set_err(err, err_size, 0, "out of memory", NULL);
        return NULL;
    }

    /* Walk the input one line at a time, tracking the current section. */
    char section[TOML_SECTION_MAX] = "";
    const char *cursor = text;
    int line_no = 0;
    while (*cursor != '\0') {
        line_no++;
        char line[TOML_LINE_MAX];
        size_t n = 0;
        while (*cursor != '\0' && *cursor != '\n') {
            if (n + 1 >= sizeof line) {
                set_err(err, err_size, line_no, "line too long", NULL);
                toml_free(doc);
                return NULL;
            }
            line[n++] = *cursor++;
        }
        line[n] = '\0';
        if (*cursor == '\n')
            cursor++;

        strip_inline_comment(line);
        char *trimmed = trim(line);
        if (trimmed[0] == '\0')
            continue;
        if (trimmed[0] == '[') {
            if (!parse_header(trimmed, section, sizeof section, err, err_size, line_no)) {
                toml_free(doc);
                return NULL;
            }
            continue;
        }
        if (!parse_key_value(doc, section, trimmed, err, err_size, line_no)) {
            toml_free(doc);
            return NULL;
        }
    }
    return doc;
}

void toml_free(toml_document *doc) {
    if (doc == NULL)
        return;
    free(doc->items);
    free(doc);
}

static const toml_entry *find_entry(const toml_document *doc,
                                    const char *section, const char *key) {
    for (size_t i = 0; i < doc->count; i++) {
        if (strcmp(doc->items[i].section, section) == 0
            && strcmp(doc->items[i].key, key) == 0)
            return &doc->items[i];
    }
    return NULL;
}

bool toml_get_string(const toml_document *doc, const char *section,
                     const char *key, char *out, size_t out_size) {
    const toml_entry *entry = find_entry(doc, section, key);
    if (entry == NULL || entry->type != toml_field_string)
        return false;
    snprintf(out, out_size, "%s", entry->value.str);
    return true;
}

bool toml_get_int(const toml_document *doc, const char *section,
                  const char *key, long *out) {
    const toml_entry *entry = find_entry(doc, section, key);
    if (entry == NULL || entry->type != toml_field_int)
        return false;
    *out = entry->value.integer;
    return true;
}

bool toml_get_bool(const toml_document *doc, const char *section,
                   const char *key, bool *out) {
    const toml_entry *entry = find_entry(doc, section, key);
    if (entry == NULL || entry->type != toml_field_bool)
        return false;
    *out = entry->value.boolean;
    return true;
}

bool toml_has_section(const toml_document *doc, const char *section) {
    for (size_t i = 0; i < doc->count; i++) {
        if (strcmp(doc->items[i].section, section) == 0)
            return true;
    }
    return false;
}

void toml_dump(const toml_document *doc, FILE *stream) {
    if (doc == NULL) {
        fprintf(stream, "(null document)\n");
        return;
    }
    for (size_t i = 0; i < doc->count; i++) {
        const toml_entry *e = &doc->items[i];
        const char *section = e->section[0] != '\0' ? e->section : "(root)";
        switch (e->type) {
            case toml_field_string:
                fprintf(stream, "[%s] %s = string \"%s\"\n", section, e->key, e->value.str);
                break;
            case toml_field_int:
                fprintf(stream, "[%s] %s = int %ld\n", section, e->key, e->value.integer);
                break;
            case toml_field_bool:
                fprintf(stream, "[%s] %s = bool %s\n", section, e->key,
                        e->value.boolean ? "true" : "false");
                break;
        }
    }
}

/* Populate a struct from the document following `schema`. Since C cannot look
 * up a struct's fields by name at runtime, each schema entry carries the field's
 * byte `offset` (from offsetof) so we can write to `(char *)out + offset`.
 * Absent keys are left untouched, so callers seed defaults before binding. */
bool toml_bind(const toml_document *doc, const toml_field *schema,
               size_t field_count, void *out, char *err, size_t err_size) {
    char *base = out;
    for (size_t i = 0; i < field_count; i++) {
        const toml_field *field = &schema[i];
        const toml_entry *entry = find_entry(doc, field->section, field->key);
        if (entry == NULL)
            continue; /* absent: keep the seeded default */
        if (entry->type != field->type) {
            set_err(err, err_size, 0, "type mismatch for key", field->key);
            return false;
        }
        void *destination = base + field->offset;
        switch (field->type) {
            case toml_field_string:
                if (field->size == 0) {
                    set_err(err, err_size, 0, "zero-size string field", field->key);
                    return false;
                }
                snprintf((char *)destination, field->size, "%s", entry->value.str);
                break;
            case toml_field_int: {
                int narrowed = (int)entry->value.integer;
                memcpy(destination, &narrowed, sizeof narrowed);
                break;
            }
            case toml_field_bool:
                memcpy(destination, &entry->value.boolean, sizeof(bool));
                break;
        }
    }
    return true;
}
