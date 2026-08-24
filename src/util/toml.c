/*
 * toml — a small, safe TOML parser.
 *
 * Design, in plain terms:
 *   - We parse the whole document once into a flat list of "entries". Each entry
 *     is one (section, key) pair with a typed value (string, integer or bool).
 *     For example the line `opt_level = 3` under `[profile.release]` becomes the
 *     entry { section="profile.release", key="opt_level", int 3 }.
 *     An inline table is flattened the same way: `sqlite = { tag = "3.53.4" }`
 *     under `[deps]` becomes { section="deps.sqlite", key="tag", str "3.53.4" }.
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
 * exceeds its limit is a parse error (never silently truncated).
 * TOML_SECTION_MAX lives in the header: reading an array of tables means
 * holding a section name. */
#define TOML_KEY_MAX 64
/* Distinct [[name]] arrays one document may declare. Each element of a nested
   array counts as its own name — the sources of the first target and those of
   the second are two arrays, not one — so this is a count of arrays in the
   document rather than of array names in its schema. */
#define TOML_MAX_TABLE_ARRAYS 64
#define TOML_VALUE_MAX 256
#define TOML_LINE_MAX 1024

/* Scratch buffer size for the digits of one integer value. */
#define TOML_DIGITS_MAX 64

/* strtol base for integer values (plain decimal). */
#define TOML_INTEGER_BASE 10

/* Growth policy for the document's entry array. */
#define TOML_DOC_INITIAL_CAPACITY 16
#define TOML_DOC_GROWTH_FACTOR 2

typedef struct {
    char section[TOML_SECTION_MAX];
    char key[TOML_KEY_MAX];
    toml_field_type type;
    union {
        char str[TOML_VALUE_MAX];
        long integer;
        bool boolean;
        str_list array; /* owned when type == toml_field_array */
    } value;
} toml_entry;

struct toml_document {
    toml_entry *items;
    size_t count;
    size_t capacity;
};

static void set_err(char *err, size_t size, int line, const char *reason, const char *text) {
    if(err == NULL || size == 0)
        return;
    if(line > 0 && text != NULL)
        snprintf(err, size, "Project.toml:%d: %s: '%s'", line, reason, text);
    else if(line > 0)
        snprintf(err, size, "Project.toml:%d: %s", line, reason);
    else if(text != NULL)
        snprintf(err, size, "%s: '%s'", reason, text);
    else
        snprintf(err, size, "%s", reason);
}

static char *trim(char *text) {
    while(*text == ' ' || *text == '\t')
        text++;
    char *end = text + strlen(text);
    while(end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        end--;
    *end = '\0';
    return text;
}

static bool is_bare_char(char c, bool allow_dot) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || (allow_dot && c == '.');
}

/* Cut the line at the first '#' that is not inside a string. */
static void strip_inline_comment(char *line) {
    bool in_basic = false;
    bool in_literal = false;
    for(char *p = line; *p != '\0'; p++) {
        char c = *p;
        if(in_basic) {
            if(c == '\\' && p[1] != '\0')
                p++;
            else if(c == '"')
                in_basic = false;
        } else if(in_literal) {
            if(c == '\'')
                in_literal = false;
        } else if(c == '"') {
            in_basic = true;
        } else if(c == '\'') {
            in_literal = true;
        } else if(c == '#') {
            *p = '\0';
            return;
        }
    }
}

static bool parse_basic_string(const char *text, char *out, size_t out_size, const char **end) {
    size_t o = 0;
    const char *p = text + 1;
    while(*p != '\0' && *p != '"') {
        char c = *p++;
        if(c == '\\') {
            char escaped = *p++;
            switch(escaped) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            default:
                return false; /* dangling or unsupported escape */
            }
        }
        if(o + 1 >= out_size)
            return false; /* value too long */
        out[o++] = c;
    }
    if(*p != '"')
        return false; /* unterminated */
    out[o] = '\0';
    *end = p + 1;
    return true;
}

static bool parse_literal_string(const char *text, char *out, size_t out_size, const char **end) {
    size_t o = 0;
    const char *p = text + 1;
    while(*p != '\0' && *p != '\'') {
        if(o + 1 >= out_size)
            return false;
        out[o++] = *p++;
    }
    if(*p != '\'')
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
    for(const char *p = text; *p != '\0'; p++) {
        if(*p == '_')
            continue; /* separators are cosmetic; drop them */
        if(count + 1 >= sizeof digits)
            return false;
        digits[count++] = *p;
    }
    digits[count] = '\0';
    if(count == 0)
        return false;
    errno = 0;
    char *endptr;
    long value = strtol(digits, &endptr, TOML_INTEGER_BASE);
    if(errno == ERANGE || *endptr != '\0')
        return false; /* out of range, or not a pure integer */
    *out = value;
    return true;
}

/* Append a parsed entry to the document, growing the array as needed. */
static bool doc_push(toml_document *doc, const toml_entry *entry, char *err, size_t err_size) {
    if(doc->count == doc->capacity) {
        size_t next =
            doc->capacity == 0 ? TOML_DOC_INITIAL_CAPACITY : doc->capacity * TOML_DOC_GROWTH_FACTOR;
        toml_entry *items = realloc(doc->items, next * sizeof(toml_entry));
        if(items == NULL) {
            set_err(err, err_size, 0, "out of memory", NULL);
            return false;
        }
        doc->items = items;
        doc->capacity = next;
    }
    doc->items[doc->count++] = *entry;
    return true;
}

/* One [[name]] seen so far, and how many times. Several arrays of tables may
   interleave in one document, so each name counts on its own. */
typedef struct {
    char name[TOML_SECTION_MAX];
    size_t seen;
} table_array_counter;

/* Next free index for [[name]], bumping its counter. Returns false when the
   document declares more distinct array names than TOML_MAX_TABLE_ARRAYS —
   refused rather than folded onto another name's counter. */
static bool next_table_array_index(table_array_counter *counters, size_t *counter_count,
                                   const char *name, size_t *out_index, char *err, size_t err_size,
                                   int line) {
    for(size_t i = 0; i < *counter_count; i++) {
        if(strcmp(counters[i].name, name) == 0) {
            *out_index = counters[i].seen++;
            return true;
        }
    }
    if(*counter_count >= TOML_MAX_TABLE_ARRAYS) {
        set_err(err, err_size, line, "too many arrays of tables", name);
        return false;
    }
    snprintf(counters[*counter_count].name, TOML_SECTION_MAX, "%s", name);
    counters[*counter_count].seen = 1;
    (*counter_count)++;
    *out_index = 0;
    return true;
}

bool toml_table_array_section(const char *name, size_t index, char *out, size_t out_size) {
    if(name == NULL || out == NULL)
        return false;
    int written = snprintf(out, out_size, "%s[%zu]", name, index);
    return written > 0 && (size_t)written < out_size;
}

/* Append `text` to `out`, or fail rather than truncate. A header silently cut
   short would name a different section than the one written. */
static bool append(char *out, size_t out_size, const char *text) {
    const size_t used = strlen(out);
    const size_t needed = strlen(text);
    if(used + needed >= out_size)
        return false;
    memcpy(out + used, text, needed + 1);
    return true;
}

/* The current index of the array named `path`, or false when no [[path]] has
   been seen. "Current" is the element the last [[path]] opened, which is what
   a header nested inside it belongs to. */
static bool current_index(const table_array_counter *counters, size_t counter_count,
                          const char *path, size_t *out) {
    for(size_t i = 0; i < counter_count; i++) {
        if(strcmp(counters[i].name, path) == 0 && counters[i].seen > 0) {
            *out = counters[i].seen - 1;
            return true;
        }
    }
    return false;
}

/* Qualify a dotted header with the element index of every ancestor that is an
   array of tables: `targets.sources` becomes `targets[0].sources` while the
   first [[targets]] is open.
 *
 * TOML says [[a.b]] appends to the array `b` inside the *current* element of
 * `a`, and [a.b] declares a table inside it. Storing either under the bare name
 * `a.b` loses which element it belonged to, so the sources of every target pile
 * into one array and the second target's artifact replaces the first's — a
 * document read as something other than what it says, which is the failure the
 * flat (section, key) model has to be defended against once tables nest.
 *
 * Only ancestors are qualified. The last segment is the array being appended to
 * or the table being opened, and the caller is what decides its index. */
static bool qualify_header(const table_array_counter *counters, size_t counter_count,
                           const char *name, char *out, size_t out_size) {
    out[0] = '\0';
    for(const char *segment = name; *segment != '\0';) {
        const size_t length = strcspn(segment, ".");
        if(length == 0)
            return false;

        char part[TOML_SECTION_MAX];
        if(length >= sizeof part)
            return false;
        snprintf(part, sizeof part, "%.*s", (int)length, segment);

        if(out[0] != '\0' && !append(out, out_size, "."))
            return false;
        if(!append(out, out_size, part))
            return false;

        segment += length;
        const bool is_last = *segment == '\0';
        if(*segment == '.')
            segment++;

        /* The final segment is the caller's to index; an ancestor's index is
           whatever element of it is currently open. */
        size_t index = 0;
        if(!is_last && current_index(counters, counter_count, out, &index)) {
            char suffix[32];
            snprintf(suffix, sizeof suffix, "[%zu]", index);
            if(!append(out, out_size, suffix))
                return false;
        }
    }
    return out[0] != '\0';
}

/* Parse a `[[name]]` header. Each occurrence becomes its own section — name[0],
   name[1], ... — so the flat (section, key) model holds unchanged and every
   existing accessor reads one of these tables with no special case. Without
   this the second [[tool]] would land in the same section as the first and
   silently replace it. */
static bool parse_table_array_header(char *text, table_array_counter *counters,
                                     size_t *counter_count, char *section, size_t size, char *err,
                                     size_t err_size, int line) {
    size_t len = strlen(text);
    if(len < 4 || text[len - 1] != ']' || text[len - 2] != ']') {
        set_err(err, err_size, line, "missing ']]' in array-of-tables header", text);
        return false;
    }
    text[len - 2] = '\0';
    char *inside = trim(text + 2);
    if(inside[0] == '\0') {
        set_err(err, err_size, line, "empty array-of-tables header", NULL);
        return false;
    }
    for(char *p = inside; *p != '\0'; p++) {
        if(!is_bare_char(*p, true)) {
            set_err(err, err_size, line, "invalid character in section name", inside);
            return false;
        }
    }

    /* Qualified before it is counted, so that [[targets.sources]] under the
       second [[targets]] counts as `targets[1].sources` and not as more of the
       first target's list. */
    char qualified[TOML_SECTION_MAX];
    if(!qualify_header(counters, *counter_count, inside, qualified, sizeof qualified)) {
        set_err(err, err_size, line, "section name too long", inside);
        return false;
    }

    size_t index = 0;
    if(!next_table_array_index(counters, counter_count, qualified, &index, err, err_size, line))
        return false;
    if(!toml_table_array_section(qualified, index, section, size)) {
        set_err(err, err_size, line, "section name too long", inside);
        return false;
    }
    return true;
}

static bool parse_header(char *text, const table_array_counter *counters, size_t counter_count,
                         char *section, size_t size, char *err, size_t err_size, int line) {
    size_t len = strlen(text);
    if(text[len - 1] != ']') {
        set_err(err, err_size, line, "missing ']' in section header", text);
        return false;
    }
    text[len - 1] = '\0';
    char *inside = trim(text + 1);
    if(inside[0] == '\0') {
        set_err(err, err_size, line, "empty section header", NULL);
        return false;
    }
    for(char *p = inside; *p != '\0'; p++) {
        if(!is_bare_char(*p, true)) {
            set_err(err, err_size, line, "invalid character in section name", inside);
            return false;
        }
    }
    /* [targets.artifact] under an open [[targets]] is that target's artifact,
       so the ancestor's index belongs in the stored name — the same rule the
       array-of-tables header follows, and the length is checked after it rather
       than before, because qualifying is what makes the name longer. */
    if(!qualify_header(counters, counter_count, inside, section, size)) {
        set_err(err, err_size, line, "section name too long", inside);
        return false;
    }
    return true;
}

/* Parse a single-line array of strings, e.g. ["m", "pthread"], into `out`.
   Elements may be basic or literal strings; a missing comma is tolerated. A
   non-string element or a missing closing ']' is an error. */
static bool parse_string_array(const char *text, str_list *out) {
    const char *p = text + 1; /* skip '[' */
    while(*p != '\0') {
        while(*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if(*p == ']')
            return true;
        if(*p != '"' && *p != '\'')
            return false;
        char element[TOML_VALUE_MAX];
        const char *end = p;
        bool ok = *p == '"' ? parse_basic_string(p, element, sizeof element, &end)
                            : parse_literal_string(p, element, sizeof element, &end);
        if(!ok)
            return false;
        if(!str_list_push(out, element))
            return false;
        p = end;
    }
    return false; /* no closing ']' */
}

/* Nesting an inline table inside an inline table is legal TOML. The limit is
   not a judgement about style: it bounds the recursion below, so a pathological
   document is a parse error rather than a stack overflow. */
#define TOML_INLINE_DEPTH_MAX 4

static bool parse_key_value(toml_document *doc, const char *section, char *text, char *err,
                            size_t err_size, int line, int depth);

/* Past a quoted string, starting at its opening quote. Answers the closing
   quote, or the terminating NUL when the string never closes. */
static char *skip_quoted(char *text) {
    const char quote = *text;
    for(char *p = text + 1; *p != '\0'; p++) {
        /* A basic string may escape its own quote; a literal one may not. */
        if(quote == '"' && *p == '\\' && p[1] != '\0')
            p++;
        else if(*p == quote)
            return p;
    }
    return text + strlen(text);
}

/* The '}' closing the inline table that starts at `text`, or NULL when there
   is none. Braces inside a string are text, not structure. */
static char *inline_table_end(char *text) {
    int depth = 0;
    for(char *p = text; *p != '\0'; p++) {
        if(*p == '"' || *p == '\'') {
            p = skip_quoted(p);
            if(*p == '\0')
                return NULL;
        } else if(*p == '{') {
            depth++;
        } else if(*p == '}' && --depth == 0) {
            return p;
        }
    }
    return NULL;
}

/* The comma ending one member of an inline table, or NULL when this is the
   last one. A comma inside a string, an array or a nested table belongs to the
   value and does not end anything. */
static char *member_end(char *text) {
    int braces = 0;
    int brackets = 0;
    for(char *p = text; *p != '\0'; p++) {
        if(*p == '"' || *p == '\'')
            p = skip_quoted(p);
        else if(*p == '{')
            braces++;
        else if(*p == '}' && braces > 0)
            braces--;
        else if(*p == '[')
            brackets++;
        else if(*p == ']' && brackets > 0)
            brackets--;
        else if(*p == ',' && braces == 0 && brackets == 0)
            return p;
    }
    return NULL;
}

/* The section an inline table's members are stored under: the key, qualified
   by whatever section the table itself was declared in. */
static bool nested_section(const char *section, const char *key, char *out, size_t size, char *err,
                           size_t err_size, int line) {
    const int written = section[0] == '\0' ? snprintf(out, size, "%s", key)
                                           : snprintf(out, size, "%s.%s", section, key);
    if(written < 0 || (size_t)written >= size) {
        set_err(err, err_size, line, "section name too long", key);
        return false;
    }
    return true;
}

/* An inline table becomes a subsection, which is what the flat model already
   is: `sqlite = { git = "…", tag = "v1" }` under [deps] reads back exactly as
   a `[deps.sqlite]` header with two keys would.

   It used to be skipped in silence, which is the failure this whole ecosystem
   exists to avoid: the user wrote a dependency, the file parsed without
   complaint, and the dependency was not there. */
static bool parse_inline_table(toml_document *doc, const char *section, const char *key,
                               char *value, char *err, size_t err_size, int line, int depth) {
    if(depth >= TOML_INLINE_DEPTH_MAX) {
        set_err(err, err_size, line, "inline tables nested too deeply", key);
        return false;
    }

    /* TOML forbids a newline inside an inline table, so an unclosed one is not
       a value continued on the next line: it is a value that never ends. */
    char *close = inline_table_end(value);
    if(close == NULL) {
        set_err(err, err_size, line, "unterminated inline table", key);
        return false;
    }
    if(*trim(close + 1) != '\0') {
        set_err(err, err_size, line, "trailing characters after value", key);
        return false;
    }
    *close = '\0';

    char nested[TOML_SECTION_MAX];
    if(!nested_section(section, key, nested, sizeof nested, err, err_size, line))
        return false;

    for(char *p = value + 1;;) {
        char *end = member_end(p);
        if(end != NULL)
            *end = '\0';
        char *member = trim(p);
        /* An empty table declares nothing, and a table that declares nothing
           leaves no entry behind — which makes it invisible to every reader,
           including the one that lists what a section contains. Real TOML
           allows it; this parser is a subset that fails closed, and the only
           thing `sqlite = {}` has ever been is a half-written dependency. */
        if(member[0] == '\0' && end == NULL) {
            if(p == value + 1) {
                set_err(err, err_size, line, "empty inline table", key);
                return false;
            }
            break;
        }
        if(!parse_key_value(doc, nested, member, err, err_size, line, depth + 1))
            return false;
        if(end == NULL)
            break;
        p = end + 1;
    }
    return true;
}

static bool parse_key_value(toml_document *doc, const char *section, char *text, char *err,
                            size_t err_size, int line, int depth) {
    char *equals = strchr(text, '=');
    if(equals == NULL) {
        set_err(err, err_size, line, "expected '='", text);
        return false;
    }
    *equals = '\0';
    char *key = trim(text);
    char *value = trim(equals + 1);
    if(key[0] == '\0') {
        set_err(err, err_size, line, "empty key", NULL);
        return false;
    }
    if(strlen(key) >= TOML_KEY_MAX) {
        set_err(err, err_size, line, "key too long", key);
        return false;
    }
    for(char *p = key; *p != '\0'; p++) {
        if(!is_bare_char(*p, false)) {
            set_err(err, err_size, line, "invalid character in key", key);
            return false;
        }
    }
    if(value[0] == '\0') {
        set_err(err, err_size, line, "missing value", key);
        return false;
    }

    toml_entry entry;
    memset(&entry, 0, sizeof entry);
    snprintf(entry.section, sizeof entry.section, "%s", section);
    snprintf(entry.key, sizeof entry.key, "%s", key);

    if(value[0] == '"' || value[0] == '\'') {
        const char *end = value;
        bool ok = value[0] == '"'
                      ? parse_basic_string(value, entry.value.str, sizeof entry.value.str, &end)
                      : parse_literal_string(value, entry.value.str, sizeof entry.value.str, &end);
        if(!ok) {
            set_err(err, err_size, line, "invalid string value", key);
            return false;
        }
        if(*trim((char *)end) != '\0') {
            set_err(err, err_size, line, "trailing characters after value", key);
            return false;
        }
        entry.type = toml_field_string;
    } else if(value[0] == '[') {
        str_list array;
        str_list_init(&array);
        if(!parse_string_array(value, &array)) {
            str_list_free(&array);
            set_err(err, err_size, line, "invalid array value", key);
            return false;
        }
        entry.type = toml_field_array;
        entry.value.array = array; /* ownership moves into the document */
        if(!doc_push(doc, &entry, err, err_size)) {
            str_list_free(&array);
            return false;
        }
        return true;
    } else if(value[0] == '{') {
        return parse_inline_table(doc, section, key, value, err, err_size, line, depth);
    } else if(strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
        entry.type = toml_field_bool;
        entry.value.boolean = strcmp(value, "true") == 0;
    } else {
        long integer;
        if(!parse_integer(value, &integer)) {
            set_err(err, err_size, line, "invalid value", value);
            return false;
        }
        entry.type = toml_field_int;
        entry.value.integer = integer;
    }
    return doc_push(doc, &entry, err, err_size);
}

/* How many '[' are still open at the end of `text`, ignoring anything inside a
   string: `name = "a [ b"` opens nothing. Negative counts are clamped to zero
   because a stray ']' is the value parser's error to report, not this one's. */
static int open_bracket_count(const char *text) {
    int depth = 0;
    for(const char *p = text; *p != '\0'; p++) {
        if(*p == '"' || *p == '\'') {
            char quote = *p;
            /* A basic string may escape its own quote; a literal one may not. */
            for(p++; *p != '\0' && *p != quote; p++) {
                if(quote == '"' && *p == '\\' && p[1] != '\0')
                    p++;
            }
            if(*p == '\0')
                break;
            continue;
        }
        if(*p == '[')
            depth++;
        else if(*p == ']' && depth > 0)
            depth--;
    }
    return depth;
}

/* Append the lines that continue an unterminated array onto `line`, until the
   brackets balance. Advances `*cursor` and `*line_no` past what it consumed.

   TOML lets an array span lines, and a recipe or a manifest with more than a
   handful of entries is written that way by every formatter. Reading only the
   first line would leave the rest to be parsed as keys. */
static bool gather_array_lines(char *line, size_t size, const char **cursor, int *line_no,
                               char *err, size_t err_size) {
    const int opened_at = *line_no;
    size_t length = strlen(line);

    while(open_bracket_count(line) > 0) {
        if(**cursor == '\0') {
            set_err(err, err_size, opened_at, "unterminated array", NULL);
            return false;
        }

        char next[TOML_LINE_MAX];
        size_t n = 0;
        (*line_no)++;
        while(**cursor != '\0' && **cursor != '\n') {
            if(n + 1 >= sizeof next) {
                set_err(err, err_size, *line_no, "line too long", NULL);
                return false;
            }
            next[n++] = *(*cursor)++;
        }
        next[n] = '\0';
        if(**cursor == '\n')
            (*cursor)++;

        strip_inline_comment(next);
        const char *piece = trim(next);
        if(*piece == '\0')
            continue;

        /* A space keeps two elements from being glued into one token when a
           line ends without its comma. */
        if(length + strlen(piece) + 2 > size) {
            set_err(err, err_size, opened_at, "array too long", NULL);
            return false;
        }
        line[length++] = ' ';
        snprintf(line + length, size - length, "%s", piece);
        length += strlen(piece);
    }
    return true;
}

toml_document *toml_parse(const char *text, char *err, size_t err_size) {
    if(text == NULL) {
        set_err(err, err_size, 0, "null input", NULL);
        return NULL;
    }
    toml_document *doc = calloc(1, sizeof *doc);
    if(doc == NULL) {
        set_err(err, err_size, 0, "out of memory", NULL);
        return NULL;
    }

    /* Walk the input one line at a time, tracking the current section. */
    char section[TOML_SECTION_MAX] = "";
    table_array_counter counters[TOML_MAX_TABLE_ARRAYS];
    size_t counter_count = 0;
    const char *cursor = text;
    int line_no = 0;
    while(*cursor != '\0') {
        line_no++;
        char line[TOML_LINE_MAX];
        size_t n = 0;
        while(*cursor != '\0' && *cursor != '\n') {
            if(n + 1 >= sizeof line) {
                set_err(err, err_size, line_no, "line too long", NULL);
                toml_free(doc);
                return NULL;
            }
            line[n++] = *cursor++;
        }
        line[n] = '\0';
        if(*cursor == '\n')
            cursor++;

        strip_inline_comment(line);
        char *trimmed = trim(line);
        if(trimmed[0] == '\0')
            continue;

        /* An array may be written across several lines. Gather them into one
           before anything else looks at the value: a continuation line starts
           with a string or a ']' and would otherwise be read as a key without
           an '=', or worse, as a section header. */
        if(open_bracket_count(trimmed) > 0) {
            if(!gather_array_lines(line, sizeof line, &cursor, &line_no, err, err_size)) {
                toml_free(doc);
                return NULL;
            }
            trimmed = trim(line);
        }

        if(trimmed[0] == '[') {
            bool ok = trimmed[1] == '['
                          ? parse_table_array_header(trimmed, counters, &counter_count, section,
                                                     sizeof section, err, err_size, line_no)
                          : parse_header(trimmed, counters, counter_count, section, sizeof section,
                                         err, err_size, line_no);
            if(!ok) {
                toml_free(doc);
                return NULL;
            }
            continue;
        }
        if(!parse_key_value(doc, section, trimmed, err, err_size, line_no, 0)) {
            toml_free(doc);
            return NULL;
        }
    }
    return doc;
}

void toml_free(toml_document *doc) {
    if(doc == NULL)
        return;
    for(size_t i = 0; i < doc->count; i++) {
        if(doc->items[i].type == toml_field_array)
            str_list_free(&doc->items[i].value.array);
    }
    free(doc->items);
    free(doc);
}

static const toml_entry *find_entry(const toml_document *doc, const char *section,
                                    const char *key) {
    for(size_t i = 0; i < doc->count; i++) {
        if(strcmp(doc->items[i].section, section) == 0 && strcmp(doc->items[i].key, key) == 0)
            return &doc->items[i];
    }
    return NULL;
}

bool toml_get_string(const toml_document *doc, const char *section, const char *key, char *out,
                     size_t out_size) {
    const toml_entry *entry = find_entry(doc, section, key);
    if(entry == NULL || entry->type != toml_field_string)
        return false;
    snprintf(out, out_size, "%s", entry->value.str);
    return true;
}

bool toml_get_int(const toml_document *doc, const char *section, const char *key, long *out) {
    const toml_entry *entry = find_entry(doc, section, key);
    if(entry == NULL || entry->type != toml_field_int)
        return false;
    *out = entry->value.integer;
    return true;
}

bool toml_get_bool(const toml_document *doc, const char *section, const char *key, bool *out) {
    const toml_entry *entry = find_entry(doc, section, key);
    if(entry == NULL || entry->type != toml_field_bool)
        return false;
    *out = entry->value.boolean;
    return true;
}

bool toml_has_section(const toml_document *doc, const char *section) {
    for(size_t i = 0; i < doc->count; i++) {
        if(strcmp(doc->items[i].section, section) == 0)
            return true;
    }
    return false;
}

size_t toml_table_array_count(const toml_document *doc, const char *name) {
    if(doc == NULL || name == NULL)
        return 0;
    /* The parser numbers them from zero without gaps, so the first name[i] that
       is not there is the end. */
    size_t count = 0;
    for(;;) {
        char section[TOML_SECTION_MAX];
        if(!toml_table_array_section(name, count, section, sizeof section) ||
           !toml_has_section(doc, section))
            return count;
        count++;
    }
}

bool toml_section_keys(const toml_document *doc, const char *section, str_list *out) {
    for(size_t i = 0; i < doc->count; i++) {
        if(strcmp(doc->items[i].section, section) != 0)
            continue;
        if(!str_list_push(out, doc->items[i].key))
            return false;
    }
    return true;
}

static bool already_listed(const str_list *list, const char *value) {
    for(size_t i = 0; i < str_list_count(list); i++) {
        if(strcmp(str_list_get(list, i), value) == 0)
            return true;
    }
    return false;
}

/* The part of `entry_section` that lies immediately inside `section`, written
   into `out`. False when the entry is not inside it at all.

   "deps.http.opts" inside "deps" is "http" — the immediate child, never the
   grandchild, because a caller listing a table wants the members it declares
   and not the shape of everything beneath them. */
static bool immediate_child(const char *entry_section, const char *section, char *out,
                            size_t size) {
    const char *rest = entry_section;
    if(section[0] != '\0') {
        const size_t length = strlen(section);
        if(strncmp(entry_section, section, length) != 0 || entry_section[length] != '.')
            return false;
        rest = entry_section + length + 1;
    } else if(entry_section[0] == '\0') {
        return false; /* the root's own values are keys, handled by the caller */
    }

    const size_t segment = strcspn(rest, ".");
    if(segment == 0 || segment >= size)
        return false;

    /* An element of a [[name]] array is stored as "name[0]", and a bare key
       cannot contain '[' — so a segment that does is one of those, and it is
       walked with toml_table_array_count rather than listed here. */
    if(memchr(rest, '[', segment) != NULL)
        return false;

    snprintf(out, size, "%.*s", (int)segment, rest);
    return true;
}

bool toml_section_members(const toml_document *doc, const char *section, str_list *out) {
    if(doc == NULL || section == NULL)
        return true;

    /* One pass, so the answer is in declaration order across both spellings: a
       reader that reports on what it found should name things in the order the
       user wrote them, and merging two ordered lists afterwards cannot. */
    for(size_t i = 0; i < doc->count; i++) {
        const char *entry_section = doc->items[i].section;
        char member[TOML_SECTION_MAX];

        if(strcmp(entry_section, section) == 0)
            snprintf(member, sizeof member, "%s", doc->items[i].key);
        else if(!immediate_child(entry_section, section, member, sizeof member))
            continue;

        if(!already_listed(out, member) && !str_list_push(out, member))
            return false;
    }
    return true;
}

bool toml_get_array(const toml_document *doc, const char *section, const char *key, str_list *out) {
    const toml_entry *entry = find_entry(doc, section, key);
    if(entry == NULL || entry->type != toml_field_array)
        return false;
    for(size_t i = 0; i < str_list_count(&entry->value.array); i++) {
        if(!str_list_push(out, str_list_get(&entry->value.array, i)))
            return false;
    }
    return true;
}

void toml_dump(const toml_document *doc, FILE *stream) {
    if(doc == NULL) {
        fprintf(stream, "(null document)\n");
        return;
    }
    for(size_t i = 0; i < doc->count; i++) {
        const toml_entry *e = &doc->items[i];
        const char *section = e->section[0] != '\0' ? e->section : "(root)";
        switch(e->type) {
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
        case toml_field_array:
            fprintf(stream, "[%s] %s = array[%zu]\n", section, e->key,
                    str_list_count(&e->value.array));
            break;
        }
    }
}

/* Populate a struct from the document following `schema`. Since C cannot look
 * up a struct's fields by name at runtime, each schema entry carries the field's
 * byte `offset` (from offsetof) so we can write to `(char *)out + offset`.
 * Absent keys are left untouched, so callers seed defaults before binding. */
bool toml_bind(const toml_document *doc, const toml_field *schema, size_t field_count, void *out,
               char *err, size_t err_size) {
    char *base = out;
    for(size_t i = 0; i < field_count; i++) {
        const toml_field *field = &schema[i];
        const toml_entry *entry = find_entry(doc, field->section, field->key);
        if(entry == NULL)
            continue; /* absent: keep the seeded default */
        if(entry->type != field->type) {
            set_err(err, err_size, 0, "type mismatch for key", field->key);
            return false;
        }
        void *destination = base + field->offset;
        switch(field->type) {
        case toml_field_string:
            if(field->size == 0) {
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
        case toml_field_array:
            /* Arrays are not bindable to scalar fields; read them with
               toml_get_array instead. (Unreachable: schema fields are never
               arrays, but kept for exhaustiveness.) */
            set_err(err, err_size, 0, "cannot bind array field", field->key);
            return false;
        }
    }
    return true;
}
