#include <molto/project/manifest_edit.h>

#include <molto/project/project_ctx.h>
#include <molto/services/fs_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPS_TABLE "deps"
#define DEV_DEPS_TABLE "dev-deps"

/* Longest manifest this edits. Well past anything hand-written, and a bound is
   better than an unbounded read of a file that might not be a manifest. */
#define MANIFEST_MAX ((size_t)1024 * 1024)

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

/* --- reading the file as lines --- */

/* Where a line begins, and where the next one does. Everything here works on
   offsets into the original text so that nothing is copied until the whole
   edit is known to be valid. */
static size_t line_end(const char *text, size_t from) {
    const char *newline = strchr(text + from, '\n');
    return newline == NULL ? strlen(text) : (size_t)(newline - text) + 1;
}

static const char *skip_blanks(const char *p) {
    while(*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* The name inside a `[header]`, or false when the line is not one. Comments and
   whitespace around it are tolerated because a hand-written manifest has
   both. */
static bool read_header(const char *text, size_t start, size_t end, char *out, size_t out_size) {
    const char *p = skip_blanks(text + start);
    if(*p != '[')
        return false;
    p++;
    if(*p == '[')
        return false; /* [[array]] is not a table this edits */
    const char *close = memchr(p, ']', end - (size_t)(p - text));
    if(close == NULL)
        return false;

    size_t length = (size_t)(close - p);
    while(length > 0 && (p[length - 1] == ' ' || p[length - 1] == '\t'))
        length--;
    if(length == 0 || length >= out_size)
        return false;
    memcpy(out, p, length);
    out[length] = '\0';
    return true;
}

/* The key a `key = value` line declares, or false. */
static bool read_key(const char *text, size_t start, size_t end, char *out, size_t out_size) {
    const char *p = skip_blanks(text + start);
    if(*p == '#' || *p == '\n' || *p == '\0' || *p == '[')
        return false;
    const char *equals = memchr(p, '=', end - (size_t)(p - text));
    if(equals == NULL)
        return false;

    size_t length = (size_t)(equals - p);
    while(length > 0 && (p[length - 1] == ' ' || p[length - 1] == '\t'))
        length--;
    if(length == 0 || length >= out_size)
        return false;
    memcpy(out, p, length);
    out[length] = '\0';
    return true;
}

static bool is_deps_table(const char *header) {
    return strcmp(header, DEPS_TABLE) == 0 || strcmp(header, DEV_DEPS_TABLE) == 0;
}

/* True when `header` is "<table>.<name>". */
static bool is_subtable_of(const char *header, const char *table, const char *name) {
    const size_t length = strlen(table);
    return strncmp(header, table, length) == 0 && header[length] == '.' &&
           strcmp(header + length + 1, name) == 0;
}

/* `deps.sqlite` and `dev-deps.sqlite`: the long form, a table of its own. */
static bool is_deps_subtable(const char *header, const char *name) {
    return is_subtable_of(header, DEPS_TABLE, name) || is_subtable_of(header, DEV_DEPS_TABLE, name);
}

const char *manifest_find_dep(const char *text, const char *name) {
    char header[TOML_SECTION_MAX] = "";
    const char *current = NULL;

    for(size_t at = 0; text[at] != '\0';) {
        const size_t end = line_end(text, at);
        if(read_header(text, at, end, header, sizeof header)) {
            if(is_subtable_of(header, DEV_DEPS_TABLE, name))
                return DEV_DEPS_TABLE;
            if(is_subtable_of(header, DEPS_TABLE, name))
                return DEPS_TABLE;
            current = NULL;
            if(is_deps_table(header))
                current = strcmp(header, DEPS_TABLE) == 0 ? DEPS_TABLE : DEV_DEPS_TABLE;
        } else if(current != NULL) {
            char key[TOML_SECTION_MAX];
            if(read_key(text, at, end, key, sizeof key) && strcmp(key, name) == 0)
                return current;
        }
        at = end;
    }
    return NULL;
}

/* --- writing the result --- */

/* The edited manifest is parsed before it replaces the original. An edit that
   produced something Molto cannot read would otherwise be discovered by the
   next command, with the user's file already gone. */
static bool write_checked(const char *path, const char *text, char *err, size_t err_size) {
    project_ctx ctx;
    char parse_err[256] = "";
    if(!project_parse(text, &ctx, parse_err, sizeof parse_err))
        return set_error(err, err_size, "the edit would leave an unreadable manifest: %s",
                         parse_err);
    if(!fs_write_file(path, text))
        return set_error(err, err_size, "could not write %s", path);
    return true;
}

static char *read_manifest(const char *path, char *err, size_t err_size) {
    char *text = fs_read_file(path);
    if(text == NULL) {
        (void)set_error(err, err_size, "could not read %s", path);
        return NULL;
    }
    if(strlen(text) > MANIFEST_MAX) {
        free(text);
        (void)set_error(err, err_size, "%s is too large to edit", path);
        return NULL;
    }
    return text;
}

/* One heap string from three pieces, so an edit is "everything before, the new
   line, everything after". */
static char *splice(const char *text, size_t start, size_t end, const char *insert) {
    const size_t length = strlen(text);
    const size_t inserted = insert == NULL ? 0 : strlen(insert);
    const size_t total = length - (end - start) + inserted + 1;
    char *out = malloc(total);
    if(out == NULL)
        return NULL;
    /* Composed in one call rather than three memcpys, so the result is
       terminated by construction. `start` fits an int because the manifest is
       bounded well below INT_MAX. */
    (void)snprintf(out, total, "%.*s%s%s", (int)start, text, insert == NULL ? "" : insert,
                   text + end);
    return out;
}

/* --- adding --- */

/* The line an entry occupies, if the table already has one. */
static bool find_entry(const char *text, const char *table, const char *name, size_t *start,
                       size_t *end) {
    char header[TOML_SECTION_MAX] = "";
    bool inside = false;

    for(size_t at = 0; text[at] != '\0';) {
        const size_t stop = line_end(text, at);
        if(read_header(text, at, stop, header, sizeof header)) {
            inside = strcmp(header, table) == 0;
        } else if(inside) {
            char key[TOML_SECTION_MAX];
            if(read_key(text, at, stop, key, sizeof key) && strcmp(key, name) == 0) {
                *start = at;
                *end = stop;
                return true;
            }
        }
        at = stop;
    }
    return false;
}

/* The comment a line ends with, if any, from the first `#` outside a string.
   Copied without its newline, for a replacement line to put back. */
static bool trailing_comment(const char *text, size_t start, size_t end, char *out,
                             size_t out_size) {
    bool in_basic = false;
    bool in_literal = false;
    for(size_t at = start; at < end; at++) {
        const char c = text[at];
        if(c == '"' && !in_literal)
            in_basic = !in_basic;
        else if(c == '\'' && !in_basic)
            in_literal = !in_literal;
        else if(c == '#' && !in_basic && !in_literal) {
            size_t length = end - at;
            while(length > 0 && (text[at + length - 1] == '\n' || text[at + length - 1] == '\r' ||
                                 text[at + length - 1] == ' '))
                length--;
            if(length == 0 || length >= out_size)
                return false;
            memcpy(out, text + at, length);
            out[length] = '\0';
            return true;
        }
    }
    return false;
}

/* Where a new entry goes: just after the table's last entry, so additions
   accumulate in the order they were made rather than at the top. */
static bool find_table_end(const char *text, const char *table, size_t *at_out) {
    char header[TOML_SECTION_MAX] = "";
    bool inside = false;
    bool found = false;
    size_t last = 0;

    for(size_t at = 0; text[at] != '\0';) {
        const size_t stop = line_end(text, at);
        if(read_header(text, at, stop, header, sizeof header)) {
            if(inside)
                break; /* the next table begins */
            inside = strcmp(header, table) == 0;
            if(inside) {
                found = true;
                last = stop;
            }
        } else if(inside) {
            char key[TOML_SECTION_MAX];
            if(read_key(text, at, stop, key, sizeof key))
                last = stop;
        }
        at = stop;
    }

    *at_out = last;
    return found;
}

bool manifest_add_dep(const char *path, const char *table, const char *name, const char *value,
                      char *err, size_t err_size) {
    char *text = read_manifest(path, err, err_size);
    if(text == NULL)
        return false;

    /* One package is one version. Letting a name sit in both tables would make
       the graph refuse to resolve later, with a message about a conflict the
       user could have been told about here. */
    const char *held_by = manifest_find_dep(text, name);
    if(held_by != NULL && strcmp(held_by, table) != 0) {
        free(text);
        return set_error(err, err_size, "'%s' is already in [%s]; remove it from there first", name,
                         held_by);
    }

    char line[1024];
    if(snprintf(line, sizeof line, "%s = %s\n", name, value) < 0) {
        free(text);
        return set_error(err, err_size, "the entry for '%s' is too long", name);
    }

    size_t start = 0;
    size_t end = 0;
    char *edited = NULL;
    if(find_entry(text, table, name, &start, &end)) {
        /* Replaced where it stands, keeping any trailing comment: bumping a
           version does not make "# the JSON reader" untrue, and deleting a
           user's note is not something an edit to the value should do. */
        char kept[256];
        if(trailing_comment(text, start, end, kept, sizeof kept))
            (void)snprintf(line, sizeof line, "%s = %s %s\n", name, value, kept);
        edited = splice(text, start, end, line);
    } else if(find_table_end(text, table, &start)) {
        edited = splice(text, start, start, line);
    } else {
        /* No such table yet: append one. A blank line before it, so a manifest
           that did not end in one does not grow a jammed-together header. */
        char block[1200];
        const size_t length = strlen(text);
        const char *lead = length > 0 && text[length - 1] == '\n' ? "" : "\n";
        if(snprintf(block, sizeof block, "%s\n[%s]\n%s", lead, table, line) < 0) {
            free(text);
            return set_error(err, err_size, "the entry for '%s' is too long", name);
        }
        edited = splice(text, length, length, block);
    }

    free(text);
    if(edited == NULL)
        return set_error(err, err_size, "out of memory editing %s", path);

    const bool ok = write_checked(path, edited, err, err_size);
    free(edited);
    return ok;
}

/* --- removing --- */

/* The long form occupies a header and everything under it. */
static bool find_subtable(const char *text, const char *name, size_t *start, size_t *end) {
    char header[TOML_SECTION_MAX] = "";
    bool inside = false;

    for(size_t at = 0; text[at] != '\0';) {
        const size_t stop = line_end(text, at);
        const bool is_header = read_header(text, at, stop, header, sizeof header);
        if(inside && is_header) {
            *end = at;
            return true;
        }
        if(is_header && is_deps_subtable(header, name)) {
            inside = true;
            *start = at;
        }
        at = stop;
        if(inside)
            *end = at;
    }
    return inside;
}

bool manifest_remove_dep(const char *path, const char *name, char *err, size_t err_size) {
    char *text = read_manifest(path, err, err_size);
    if(text == NULL)
        return false;

    const char *table = manifest_find_dep(text, name);
    if(table == NULL) {
        free(text);
        return set_error(err, err_size, "'%s' is not a dependency of this project", name);
    }

    size_t start = 0;
    size_t end = 0;
    if(!find_entry(text, table, name, &start, &end) && !find_subtable(text, name, &start, &end)) {
        free(text);
        return set_error(err, err_size, "could not find where '%s' is declared", name);
    }

    char *edited = splice(text, start, end, NULL);
    free(text);
    if(edited == NULL)
        return set_error(err, err_size, "out of memory editing %s", path);

    const bool ok = write_checked(path, edited, err, err_size);
    free(edited);
    return ok;
}
