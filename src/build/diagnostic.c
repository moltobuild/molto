#include <molto/build/diagnostic.h>

#include <molto/services/fs_service.h>
#include <molto/util/json_write.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Growth policy for the list. */
#define DIAGNOSTIC_INITIAL_CAPACITY 16
#define DIAGNOSTIC_GROWTH_FACTOR 2

/* Separator between a location, a severity and the message. */
#define FIELD_SEPARATOR ':'

/* clang-tidy appends the reason a check was promoted, as in
   "[bugprone-assignment-in-if-condition,-warnings-as-errors]". The rule is what
   comes before the comma. */
#define RULE_SEPARATOR ','

/* Prefixes stripped when canonicalizing a compiler warning flag. */
#define WERROR_PREFIX "-Werror="
#define WARNING_PREFIX "-W"

void diagnostic_list_init(diagnostic_list *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

bool diagnostic_list_push(diagnostic_list *list, const diagnostic *item) {
    if(list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? DIAGNOSTIC_INITIAL_CAPACITY
                                              : list->capacity * DIAGNOSTIC_GROWTH_FACTOR;
        diagnostic *grown = realloc(list->items, capacity * sizeof *grown);
        if(grown == NULL)
            return false;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = *item;
    return true;
}

size_t diagnostic_list_count(const diagnostic_list *list) { return list->count; }

const diagnostic *diagnostic_list_get(const diagnostic_list *list, size_t index) {
    return index < list->count ? &list->items[index] : NULL;
}

bool diagnostic_list_append(diagnostic_list *list, const diagnostic_list *other) {
    for(size_t i = 0; i < other->count; i++) {
        if(!diagnostic_list_push(list, &other->items[i]))
            return false;
    }
    return true;
}

void diagnostic_list_free(diagnostic_list *list) {
    free(list->items);
    diagnostic_list_init(list);
}

const char *diagnostic_severity_name(diagnostic_severity severity) {
    switch(severity) {
    case diagnostic_severity_error:
        return "error";
    case diagnostic_severity_warning:
        return "warning";
    /* An unparsed line is context, and context is a note: it is never a
       reason to fail, and it has to be called something. */
    case diagnostic_severity_note:
    case diagnostic_severity_unknown:
    default:
        return "note";
    }
}

/* What a tool calls each severity. "fatal error" is two words, which is why
   this is a table over the text between two colons and not a word compare. */
static const struct {
    const char *text;
    diagnostic_severity severity;
} severity_names[] = {
    {"error", diagnostic_severity_error},     {"fatal error", diagnostic_severity_error},
    {"warning", diagnostic_severity_warning}, {"note", diagnostic_severity_note},
    {"remark", diagnostic_severity_note},
};
#define SEVERITY_NAME_COUNT (sizeof severity_names / sizeof severity_names[0])

/* Read a severity word, or false for anything that is not one. */
static bool severity_of(const char *text, size_t length, diagnostic_severity *out) {
    for(size_t i = 0; i < SEVERITY_NAME_COUNT; i++) {
        if(strlen(severity_names[i].text) == length &&
           strncmp(severity_names[i].text, text, length) == 0) {
            *out = severity_names[i].severity;
            return true;
        }
    }
    return false;
}

/* Copy at most `size - 1` bytes of `text` into `out`, NUL-terminated. */
static void copy_bounded(char *out, size_t size, const char *text, size_t length) {
    if(length >= size)
        length = size - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

/* Everything a tool prints that is not a diagnostic of its own: clang-tidy's
   tally at the end of a run. Recognised so `molto lint` does not change shape
   depending on which backend produced the output. */
static bool is_summary_line(const char *line) {
    static const char *const suffixes[] = {
        " warnings generated.", " warning generated.",         " errors generated.",
        " error generated.",    " warnings treated as errors", " warning treated as error",
    };
    size_t count = sizeof suffixes / sizeof suffixes[0];
    size_t length = strlen(line);
    for(size_t i = 0; i < count; i++) {
        size_t suffix_length = strlen(suffixes[i]);
        if(length >= suffix_length && strcmp(line + length - suffix_length, suffixes[i]) == 0 &&
           isdigit((unsigned char)line[0]))
            return true;
    }
    /* "Suppressed N warnings (...)" does not end in a fixed suffix. */
    return strncmp(line, "Suppressed ", 11) == 0;
}

/* Canonicalize a native rule name: cut at the comma clang-tidy appends, drop a
   -Werror= or -W prefix, and turn hyphens into underscores. */
static void canonicalize_rule(const char *native, char *out, size_t out_size) {
    const char *start = native;
    if(strncmp(start, WERROR_PREFIX, strlen(WERROR_PREFIX)) == 0)
        start += strlen(WERROR_PREFIX);
    else if(strncmp(start, WARNING_PREFIX, strlen(WARNING_PREFIX)) == 0)
        start += strlen(WARNING_PREFIX);

    const char *comma = strchr(start, RULE_SEPARATOR);
    size_t length = comma != NULL ? (size_t)(comma - start) : strlen(start);
    copy_bounded(out, out_size, start, length);
    for(char *p = out; *p != '\0'; p++) {
        if(*p == '-')
            *p = '_';
    }
}

/* Split a trailing "[rule]" off the message, into both spellings. */
static void split_rule(diagnostic *out) {
    size_t length = strlen(out->message);
    if(length < 3 || out->message[length - 1] != ']')
        return;

    char *open = strrchr(out->message, '[');
    if(open == NULL || open == out->message)
        return;
    /* Only a bracket that opens the last word: "[x] in the middle" is text. */
    if(open[-1] != ' ')
        return;

    out->message[length - 1] = '\0';
    copy_bounded(out->rule_native, sizeof out->rule_native, open + 1, strlen(open + 1));
    canonicalize_rule(out->rule_native, out->rule, sizeof out->rule);
    out->message[length - 1] = ']';

    /* Trim the "[rule]" and the space before it off the message. */
    open[-1] = '\0';
}

/* Read a run of digits, returning how many were consumed. */
static size_t read_number(const char *text, long *out) {
    size_t at = 0;
    long value = 0;
    while(isdigit((unsigned char)text[at])) {
        value = value * 10 + (text[at] - '0');
        at++;
    }
    if(at > 0)
        *out = value;
    return at;
}

/* Find where the location ends: the first ':' followed by digits followed by
   another ':'. Scanning for that rather than for the first ':' is what lets a
   path containing a colon, or a "note:" prefix, fall through cleanly.
   Returns false when the line carries no location. */
static bool find_location(const char *line, size_t *path_length, size_t *after) {
    for(size_t i = 0; line[i] != '\0'; i++) {
        if(line[i] != FIELD_SEPARATOR)
            continue;
        long ignored = 0;
        size_t digits = read_number(line + i + 1, &ignored);
        if(digits > 0 && line[i + 1 + digits] == FIELD_SEPARATOR) {
            *path_length = i;
            *after = i;
            return true;
        }
    }
    return false;
}

void diagnostic_parse_line(const char *line, diagnostic *out) {
    memset(out, 0, sizeof *out);
    out->severity = diagnostic_severity_unknown;
    copy_bounded(out->message, sizeof out->message, line, strlen(line));

    size_t path_length = 0;
    size_t at = 0;
    if(!find_location(line, &path_length, &at))
        return;

    long line_number = 0;
    at += 1 + read_number(line + at + 1, &line_number);

    long column = 0;
    if(line[at] == FIELD_SEPARATOR) {
        size_t digits = read_number(line + at + 1, &column);
        /* A column only if digits follow and another colon closes them; gcc
           with -fno-diagnostics-show-column writes "a.c:7: warning:". */
        if(digits > 0 && line[at + 1 + digits] == FIELD_SEPARATOR)
            at += 1 + digits;
        else
            column = 0;
    }
    if(line[at] != FIELD_SEPARATOR)
        return;
    at++;
    while(line[at] == ' ')
        at++;

    const char *severity_start = line + at;
    const char *severity_end = strchr(severity_start, FIELD_SEPARATOR);
    diagnostic_severity severity = diagnostic_severity_unknown;
    if(severity_end == NULL ||
       !severity_of(severity_start, (size_t)(severity_end - severity_start), &severity))
        return; /* no severity: keep the line whole, as unknown */

    const char *message = severity_end + 1;
    while(*message == ' ')
        message++;

    copy_bounded(out->file, sizeof out->file, line, path_length);
    out->line = line_number;
    out->column = column;
    out->severity = severity;
    copy_bounded(out->message, sizeof out->message, message, strlen(message));
    split_rule(out);
}

bool diagnostic_parse(const char *text, diagnostic_list *out) {
    if(text == NULL)
        return true;

    const char *cursor = text;
    while(*cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);

        char line[DIAGNOSTIC_TEXT_MAX];
        copy_bounded(line, sizeof line, cursor, length);
        if(line[0] != '\0' && !is_summary_line(line)) {
            diagnostic item;
            diagnostic_parse_line(line, &item);
            if(!diagnostic_list_push(out, &item))
                return false;
        }

        if(end == NULL)
            break;
        cursor = end + 1;
    }
    return true;
}

/* --- what a failing linker says --- */

/* The driver reporting that the link as a whole did not work. It repeats what
   the lines above already said, in the way `1 error generated.` does. */
static bool is_link_summary(const char *line) {
    return strstr(line, ": ld returned ") != NULL || strstr(line, "linker command failed") != NULL;
}

/* "<object>: in function `name':", which names a temporary object file the
   reader cannot open and a function that the line below locates properly. */
static bool is_link_context(const char *line) { return strstr(line, ": in function ") != NULL; }

/* Past a leading "ld: " or "/usr/bin/ld: ".
 *
 * GNU ld writes it on some of its lines and not on others — the first location
 * of a failed link carries no prefix and the second one does — so it is
 * stripped where it is and not looked for where it is not. Matched on the
 * program's own name rather than on the shape of the line, because a source
 * path followed by a space is a shape a real diagnostic has too. */
static const char *past_link_tool(const char *line) {
    const char *separator = strstr(line, ": ");
    if(separator == NULL)
        return line;
    const char *base = line;
    for(const char *at = line; at < separator; at++) {
        if(*at == '/')
            base = at + 1;
    }
    const size_t length = (size_t)(separator - base);
    if((length == 2 && strncmp(base, "ld", 2) == 0) || (length > 3 && strncmp(base, "ld.", 3) == 0))
        return separator + 2;
    return line;
}

/* "<path>:<line>: <message>" — the one shape a failing linker gives that can
   be pointed at, and only when the objects carry debug information. Without it
   the same failure reads "main.c:(.text+0x9): undefined reference", naming an
   offset into a section rather than a place in anyone's code. */
static bool parse_link_location(const char *line, diagnostic *out) {
    for(const char *at = strchr(line, ':'); at != NULL; at = strchr(at + 1, ':')) {
        const char *digits = at + 1;
        const char *end = digits;
        while(*end >= '0' && *end <= '9')
            end++;
        if(end == digits || end[0] != ':' || end[1] != ' ')
            continue;
        const size_t path_length = (size_t)(at - line);
        if(path_length == 0)
            return false;
        copy_bounded(out->file, sizeof out->file, line, path_length);
        out->line = strtol(digits, NULL, 10);
        copy_bounded(out->message, sizeof out->message, end + 2, strlen(end + 2));
        return true;
    }
    return false;
}

bool diagnostic_parse_link(const char *text, diagnostic_list *out) {
    if(text == NULL)
        return true;

    const char *cursor = text;
    while(*cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        const size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);

        char line[DIAGNOSTIC_TEXT_MAX];
        copy_bounded(line, sizeof line, cursor, length);
        if(line[0] != '\0' && !is_link_summary(line) && !is_link_context(line)) {
            const char *said = past_link_tool(line);
            /* Everything a failing linker says is an error. It names no
               severity because it has only the one to name. */
            diagnostic item = {.severity = diagnostic_severity_error};
            if(!parse_link_location(said, &item))
                copy_bounded(item.message, sizeof item.message, said, strlen(said));
            if(!diagnostic_list_push(out, &item))
                return false;
        }

        if(end == NULL)
            break;
        cursor = end + 1;
    }
    return true;
}

size_t diagnostic_count_severity(const diagnostic_list *list, diagnostic_severity severity) {
    size_t count = 0;
    for(size_t i = 0; i < list->count; i++) {
        if(list->items[i].severity == severity)
            count++;
    }
    return count;
}

bool diagnostic_format(const diagnostic *item, const char *root, char *out, size_t out_size) {
    if(item->severity == diagnostic_severity_unknown || item->file[0] == '\0') {
        int written = snprintf(out, out_size, "%s", item->message);
        return written >= 0 && (size_t)written < out_size;
    }

    char location[DIAGNOSTIC_PATH_MAX + 32];
    const char *path = fs_relative_to(item->file, root);
    int written =
        item->column > 0
            ? snprintf(location, sizeof location, "%s:%ld:%ld", path, item->line, item->column)
            : snprintf(location, sizeof location, "%s:%ld", path, item->line);
    if(written < 0 || (size_t)written >= sizeof location)
        return false;

    written = item->rule[0] != '\0'
                  ? snprintf(out, out_size, "%s: %s: %s [%s]", location,
                             diagnostic_severity_name(item->severity), item->message, item->rule)
                  : snprintf(out, out_size, "%s: %s: %s", location,
                             diagnostic_severity_name(item->severity), item->message);
    return written >= 0 && (size_t)written < out_size;
}

void diagnostic_write_text(FILE *stream, const diagnostic_list *list, const char *root) {
    for(size_t i = 0; i < list->count; i++) {
        char line[DIAGNOSTIC_TEXT_MAX + DIAGNOSTIC_PATH_MAX + 64];
        if(diagnostic_format(&list->items[i], root, line, sizeof line))
            fprintf(stream, "%s\n", line);
    }
}

/* Whether an entry belongs in a machine-readable report. The caret lines and
   source snippets a compiler interleaves are there to be read by a person next
   to the line they point at; carrying no file and no line, they are nothing a
   CI job could act on. They are still printed in the text form, where they mean
   something. */
static bool is_reportable(const diagnostic *item) {
    return item->severity != diagnostic_severity_unknown && item->file[0] != '\0';
}

void diagnostic_write_json(FILE *stream, const diagnostic_list *list, const char *root) {
    size_t remaining = 0;
    for(size_t i = 0; i < list->count; i++)
        remaining += is_reportable(&list->items[i]) ? 1 : 0;

    fputs("[\n", stream);
    for(size_t i = 0; i < list->count; i++) {
        const diagnostic *item = &list->items[i];
        if(!is_reportable(item))
            continue;
        fputs("  {\"file\": ", stream);
        json_write_string(stream, fs_relative_to(item->file, root));
        fprintf(stream, ", \"line\": %ld, \"column\": %ld, \"severity\": ", item->line,
                item->column);
        json_write_string(stream, diagnostic_severity_name(item->severity));
        fputs(", \"message\": ", stream);
        json_write_string(stream, item->message);
        fputs(", \"rule\": ", stream);
        json_write_string(stream, item->rule);
        fputs(--remaining > 0 ? "},\n" : "}\n", stream);
    }
    fputs("]\n", stream);
}

/* --- storage form (RFC-0006) --- */

/* Fields per record, in the order to_values writes them. */
#define FIELD_COUNT 7

/* The widest a long prints as, plus sign and terminator. */
#define NUMBER_TEXT_MAX 24

static bool push_number(str_list *out, long value) {
    char text[NUMBER_TEXT_MAX];
    snprintf(text, sizeof text, "%ld", value);
    return str_list_push(out, text);
}

/* Read a field written by push_number. Rejects trailing junk and negatives:
   both mean the record was not written by this code. */
static bool read_stored_number(const char *text, long *out) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if(end == text || *end != '\0' || value < 0)
        return false;
    *out = value;
    return true;
}

bool diagnostic_list_to_values(const diagnostic_list *list, str_list *out) {
    for(size_t i = 0; i < diagnostic_list_count(list); i++) {
        const diagnostic *item = diagnostic_list_get(list, i);
        if(!str_list_push(out, item->file) || !push_number(out, item->line) ||
           !push_number(out, item->column) || !push_number(out, (long)item->severity) ||
           !str_list_push(out, item->rule) || !str_list_push(out, item->rule_native) ||
           !str_list_push(out, item->message))
            return false;
    }
    return true;
}

/* True for a severity value that names one of the enumerators. Anything else
   was not written by this code, and guessing would replay the wrong severity —
   which decides whether the command fails. */
static bool severity_is_known(long value) {
    return value == diagnostic_severity_note || value == diagnostic_severity_warning ||
           value == diagnostic_severity_error || value == diagnostic_severity_unknown;
}

bool diagnostic_list_from_values(const str_list *values, diagnostic_list *out) {
    size_t count = str_list_count(values);
    if(count % FIELD_COUNT != 0)
        return false;

    for(size_t at = 0; at < count; at += FIELD_COUNT) {
        diagnostic item;
        memset(&item, 0, sizeof item);

        long line = 0;
        long column = 0;
        long severity = 0;
        if(!read_stored_number(str_list_get(values, at + 1), &line) ||
           !read_stored_number(str_list_get(values, at + 2), &column) ||
           !read_stored_number(str_list_get(values, at + 3), &severity) ||
           !severity_is_known(severity))
            return false;

        item.line = line;
        item.column = column;
        item.severity = (diagnostic_severity)severity;
        const char *file = str_list_get(values, at);
        const char *rule = str_list_get(values, at + 4);
        const char *native = str_list_get(values, at + 5);
        const char *message = str_list_get(values, at + 6);
        copy_bounded(item.file, sizeof item.file, file, strlen(file));
        copy_bounded(item.rule, sizeof item.rule, rule, strlen(rule));
        copy_bounded(item.rule_native, sizeof item.rule_native, native, strlen(native));
        copy_bounded(item.message, sizeof item.message, message, strlen(message));

        if(!diagnostic_list_push(out, &item))
            return false;
    }
    return true;
}
