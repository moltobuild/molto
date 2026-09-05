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
#define LINT_MAX_RULE_OPTIONS 8
#define LINT_OPTION_NAME_MAX 32
#define LINT_OPTION_VALUE_MAX 64

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

/* One setting of one rule: `{ "threshold": 30 }` on `function_complexity`.
   The name is Molto's, the same as everything else here — a backend's own
   spelling would tie a project to that backend as surely as writing its
   configuration by hand. */
typedef struct {
    char name[LINT_OPTION_NAME_MAX];
    char value[LINT_OPTION_VALUE_MAX];
} lint_option;

/*
 * A rule, and what it was told.
 *
 * Two shapes in `linter.json`, because most rules need nothing said about them:
 *
 *     "bugprone": "warn"
 *     "function_complexity": ["warn", { "threshold": 30 }]
 *
 * A rule that takes options and is given none is not silent: it uses the
 * defaults the rule's own name implies. `naming_snake_case` means snake case,
 * and a rule whose name already says what it wants should not need saying
 * twice.
 */
typedef struct {
    char name[LINT_RULE_NAME_MAX];
    lint_severity severity;
    lint_option options[LINT_MAX_RULE_OPTIONS];
    size_t option_count;
} lint_rule;

typedef struct {
    char backend[STYLE_BACKEND_MAX];
    style_preset preset;
    style_excludes paths;
    /*
     * Whether the project's own headers are analysed. True unless a file says
     * otherwise, and it has to be, because the alternative is what molto did
     * until this key existed: clang-tidy reports only what it finds in the
     * file it was handed unless it is told which headers count, so every
     * `static inline`, every macro and every bug in a header went unread. In
     * this repository that was 79 files and 7,207 lines.
     *
     * "The project's own" and not "all of them": a dependency's headers arrive
     * through the same `-I` as the project's, and a lint report about somebody
     * else's code is a report nobody can act on.
     */
    bool headers;
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
