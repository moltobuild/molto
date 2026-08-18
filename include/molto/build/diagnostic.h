#ifndef MOLTO_DIAGNOSTIC_H
#define MOLTO_DIAGNOSTIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <molto/util/str_list.h>

/*
 * What a tool said, normalized.
 *
 * RFC-0005 requires that diagnostics from every source — the compiler, the
 * formatter, the lint backend — read the same, so output does not change shape
 * when a backend is swapped. That turns out to cost almost nothing: gcc,
 * clang-tidy and clang-format --dry-run already print the same line.
 *
 *     src/net/socket.c:42:9: error: 'maxRetries' is not snake_case [naming]
 *
 * The parser is deliberately tolerant. A line it cannot read is kept as a
 * diagnostic of unknown severity rather than dropped, so the caret lines,
 * source snippets and "In file included from" headers a compiler interleaves
 * still reach the user in place. Nothing a tool says is ever thrown away
 * silently.
 */

#define DIAGNOSTIC_PATH_MAX 512
#define DIAGNOSTIC_TEXT_MAX 1024
#define DIAGNOSTIC_RULE_MAX 64

typedef enum {
    /* Attached context: the "note:" under an error, and everything a tool
       prints that carries no severity at all. Kept, never a reason to fail. */
    diagnostic_severity_note,
    diagnostic_severity_warning,
    diagnostic_severity_error,
    /* A line that did not parse. Its whole text is in `message` and `file` is
       empty, so it prints back exactly as the tool wrote it. */
    diagnostic_severity_unknown,
} diagnostic_severity;

/* How the tool counted the column it reported.
 *
 * gcc counts what a terminal counts: a tab advances to the next stop of eight,
 * and a character outside ASCII is one column however many bytes it took.
 * clang counts bytes, and so does clang-tidy whichever compiler the project
 * builds with. Both are defensible, they disagree on any line holding a tab or
 * anything outside ASCII, and a caret placed under the wrong model lands
 * somewhere the tool was not pointing.
 *
 * It belongs to the diagnostic rather than to the report that holds it, because
 * one report can hold both: `molto lint` runs a compiler pass and a clang-tidy
 * pass over the same file, and with gcc as the compiler the two count
 * differently about the same line.
 *
 * The default is gcc's, which is also the one to assume for a compiler chosen
 * by hand, which answers for no vendor at all. */
typedef enum {
    diagnostic_columns_display, /* gcc */
    diagnostic_columns_byte,    /* clang, and clang-tidy always */
} diagnostic_column_unit;

typedef struct {
    char file[DIAGNOSTIC_PATH_MAX]; /* "" when the line carried no location */
    long line;                      /* 0 when absent */
    long column;                    /* 0 when absent */
    /* Which model counted `column`. The parser cannot tell — the number reads
       the same either way — so whoever ran the tool says so afterwards. */
    diagnostic_column_unit columns;
    diagnostic_severity severity;
    char message[DIAGNOSTIC_TEXT_MAX];
    /* The rule twice: canonical for reading, native for acting on. Someone who
       reads [unused_variable] still needs -Wno-unused-variable to silence it,
       and [bugprone_assignment_in_if_condition] needs its hyphens for NOLINT. */
    char rule[DIAGNOSTIC_RULE_MAX];
    char rule_native[DIAGNOSTIC_RULE_MAX];
} diagnostic;

/* A growable list, public like str_list: it holds no state beyond its items. */
typedef struct {
    diagnostic *items;
    size_t count;
    size_t capacity;
} diagnostic_list;

void diagnostic_list_init(diagnostic_list *list);
[[nodiscard]] bool diagnostic_list_push(diagnostic_list *list, const diagnostic *item);
[[nodiscard]] size_t diagnostic_list_count(const diagnostic_list *list);
[[nodiscard]] const diagnostic *diagnostic_list_get(const diagnostic_list *list, size_t index);
/* Append every item of `other`, in order. */
[[nodiscard]] bool diagnostic_list_append(diagnostic_list *list, const diagnostic_list *other);
void diagnostic_list_free(diagnostic_list *list);

/* Which model a compiler counts its columns in, from the vendor pickup
   reported. Everything that is not clang counts what a terminal counts. */
[[nodiscard]] diagnostic_column_unit diagnostic_columns_of_vendor(const char *vendor);

/* Say that every diagnostic in `list` came from a tool counting `unit`. Called
   by whoever ran that tool, since the parser has no way of knowing. */
void diagnostic_list_set_columns(diagnostic_list *list, diagnostic_column_unit unit);

/* Parse one line. Always succeeds: see diagnostic_severity_unknown. */
void diagnostic_parse_line(const char *line, diagnostic *out);

/* Split `text` into lines and parse each into `out` (caller-initialised).
   False only on allocation failure. */
[[nodiscard]] bool diagnostic_parse(const char *text, diagnostic_list *out);

/* Parse what a *failing* linker wrote, which the grammar above cannot read: a
   linker names no severity, because it has only the one to name, and prefixes
   some of its lines with its own program name and not others.
 *
 * Only for output a failed link produced. Everything it says is taken as an
 * error, so a linker that merely warned must go through `diagnostic_parse`
 * instead. False only on allocation failure. */
[[nodiscard]] bool diagnostic_parse_link(const char *text, diagnostic_list *out);

/* How many entries carry `severity`. Only errors fail a command (RFC-0005). */
[[nodiscard]] size_t diagnostic_count_severity(const diagnostic_list *list,
                                               diagnostic_severity severity);

/* Render one diagnostic in Molto's normalized form, with the path shown
   relative to `root` when it is under it. Parts it does not have are omitted,
   and a line that did not parse prints unchanged. */
[[nodiscard]] bool diagnostic_format(const diagnostic *item, const char *root, char *out,
                                     size_t out_size);

/* Write the whole list, one normalized line each. */
void diagnostic_write_text(FILE *stream, const diagnostic_list *list, const char *root);

/* Write the whole list as one JSON document, for CI (RFC-0005). */
void diagnostic_write_json(FILE *stream, const diagnostic_list *list, const char *root);

/* The name of a severity, for reports: "error", "warning", "note". Never NULL. */
[[nodiscard]] const char *diagnostic_severity_name(diagnostic_severity severity);

/* --- storage form (RFC-0006) ---
   A list flattened into strings, to be recorded and replayed exactly. Each
   diagnostic becomes a fixed run of fields rather than one delimited line: the
   store keeps every string with its length, so a message containing tabs or
   quotes needs no escaping and cannot be misread on the way back.

   The severity travels as its numeric value, not as its name, because the name
   of `unknown` is "note" — a line that did not parse would come back as one
   that did, and print differently from how the tool wrote it. */

/* Append `list` to `out` (caller-initialised). False on allocation failure. */
[[nodiscard]] bool diagnostic_list_to_values(const diagnostic_list *list, str_list *out);

/* Rebuild a list from what to_values wrote. False if `values` is not a whole
   number of well-formed records, in which case the caller must re-analyse
   rather than trust a partial reading. */
[[nodiscard]] bool diagnostic_list_from_values(const str_list *values, diagnostic_list *out);

#endif /* MOLTO_DIAGNOSTIC_H */
