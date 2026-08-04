#ifndef MOLTO_STYLE_CONFIG_H
#define MOLTO_STYLE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The canonical style model of RFC-0005, and the two files that express it:
 * `format.json` for layout and `linter.json` for checks.
 *
 * The option names here belong to Molto, not to any backend. That is the point:
 * `.clang-format` and `uncrustify.cfg` describe overlapping ideas in
 * incompatible vocabularies, so switching engines means rewriting the
 * configuration from scratch. Molto owns one vocabulary and translates it
 * (see style_translate).
 *
 * Both files are optional; absent means the defaults. Both fail closed: an
 * unknown key, an unknown value or a list that overflows is an error with a
 * line the user can act on, never something quietly ignored. A typo that does
 * nothing is exactly what that prevents.
 *
 * The two live together because they are the same concern and share half their
 * keys — `backend`, `preset` and `exclude` mean the same thing in both.
 */

#define STYLE_BACKEND_MAX 64
#define STYLE_MAX_EXCLUDES 16
#define STYLE_EXCLUDE_MAX 128
#define LINT_MAX_RULES 32
#define LINT_RULE_NAME_MAX 64

/* A curated starting point the explicit keys are layered on top of. */
typedef enum {
    style_preset_molto, /* the default: spec.md section 17 */
    style_preset_none,
} style_preset;

typedef enum {
    brace_style_attach,
    brace_style_break,
    brace_style_linux,
    brace_style_allman,
} brace_style;

typedef enum {
    pointer_alignment_left,  /* int* p */
    pointer_alignment_right, /* int *p */
} pointer_alignment;

/* The initial formatting vocabulary (RFC-0005). */
typedef struct {
    int indent_width;
    bool use_tabs;
    int line_width;
    brace_style braces;
    pointer_alignment pointers;
    bool sort_includes;
    bool space_before_paren;
    bool column_limit_comments;
} style_options;

/* What the paths of a project are filtered by. Shared by both files. */
typedef struct {
    char exclude[STYLE_MAX_EXCLUDES][STYLE_EXCLUDE_MAX];
    size_t exclude_count;
} style_excludes;

typedef struct {
    char backend[STYLE_BACKEND_MAX]; /* "" when the file does not pin one */
    style_preset preset;
    style_excludes paths;
    style_options style;
} style_config;

/* Each rule or family is off, reported, or fatal. Only `error` fails a command
   (RFC-0005): a warning is reported and the command still succeeds. */
typedef enum {
    lint_severity_off,
    lint_severity_warn,
    lint_severity_error,
} lint_severity;

typedef struct {
    char name[LINT_RULE_NAME_MAX];
    lint_severity severity;
} lint_rule;

typedef struct {
    char backend[STYLE_BACKEND_MAX];
    style_preset preset;
    style_excludes paths;
    lint_rule rules[LINT_MAX_RULES];
    size_t rule_count;
} lint_config;

/* Seed the defaults RFC-0005 specifies. Always call before parsing. */
void style_config_defaults(style_config *out);
void lint_config_defaults(lint_config *out);

/* Parse the text of one file. False with a message in `err` on anything
   malformed, unknown or oversized. */
[[nodiscard]] bool style_config_parse(const char *json, style_config *out, char *err,
                                      size_t err_size);
[[nodiscard]] bool lint_config_parse(const char *json, lint_config *out, char *err,
                                     size_t err_size);

/* Read <root>/format.json or <root>/linter.json. A missing file is not an
   error: RFC-0005 says an absent configuration means the defaults. */
[[nodiscard]] bool style_config_load(const char *root, style_config *out, char *err,
                                     size_t err_size);
[[nodiscard]] bool lint_config_load(const char *root, lint_config *out, char *err, size_t err_size);

/* True if `relative_path` matches one of the exclude patterns.

   Patterns are POSIX fnmatch without FNM_PATHNAME, so a star crosses a slash
   and a pattern naming a directory followed by one star matches everything
   below it, however deep. A pattern ending in a slash and two stars is
   additionally tried with that suffix removed, so it also matches the
   directory itself. Stated here so the behaviour is a promise, not an
   accident. */
[[nodiscard]] bool style_excludes_match(const style_excludes *excludes, const char *relative_path);

#endif /* MOLTO_STYLE_CONFIG_H */
