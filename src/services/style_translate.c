#include <molto/services/style_translate.h>

#include <molto/services/fs_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Where generated configurations live: machine-owned, under the directory
   Molto already owns, so the project tree never grows a .clang-format. */
#define STYLE_DIR ".bin/style"
#define FORMAT_CONFIG "clang-format.yaml"
#define LINT_CONFIG "clang-tidy.yaml"

/* The backends this version can translate for. */
#define BACKEND_CLANG_FORMAT "clang-format"
#define BACKEND_CLANG_TIDY "clang-tidy"

/* Separator between a backend name and its pinned version: "clang-format@22.1.8". */
#define BACKEND_PIN_SEPARATOR '@'

/* Size of the buffer holding a rendered configuration. */
#define CONFIG_TEXT_MAX 4096

/* What the `molto` preset asks the linter for: the compiler's own diagnostics
   and the checks that find bugs.

   The path-sensitive analyzer (clang-analyzer-*) is deliberately not here. It
   is slow, and its security checks demand the C11 Annex K functions that glibc
   does not ship, so a default that included it would bury real findings under
   hundreds of "use snprintf_s" on a platform where that does not exist. A
   project that wants it asks for it by name, with the `security` rule. */
#define PRESET_MOLTO_CHECKS "clang-diagnostic-*,bugprone-*"

static void set_err(char *err, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t size, const char *format, ...) {
    if(err == NULL || size == 0)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(err, size, format, args);
    va_end(args);
}

/* Append to a bounded buffer, reporting rather than truncating. */
static bool append(char *out, size_t out_size, size_t *used, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static bool append(char *out, size_t out_size, size_t *used, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out + *used, out_size - *used, format, args);
    va_end(args);
    if(written < 0 || (size_t)written >= out_size - *used)
        return false;
    *used += (size_t)written;
    return true;
}

/* Check the pin in a configuration against the tool pickup actually resolved.
   Molto does not install or rewrite paths — that is pickup's half of the split
   — so verifying that what is here is what was asked for is the whole of what
   it can do, and it is what makes formatting reproducible. */
static bool check_backend(const char *declared, const char *expected_name,
                          const resolved_tool *backend, const char *file, char *err,
                          size_t err_size) {
    if(declared[0] == '\0')
        return true; /* no pin: whatever pickup reports is the backend */

    const char *at = strchr(declared, BACKEND_PIN_SEPARATOR);
    size_t name_length = at != NULL ? (size_t)(at - declared) : strlen(declared);
    if(strlen(expected_name) != name_length || strncmp(declared, expected_name, name_length) != 0) {
        set_err(err, err_size,
                "%s: backend '%s' is not supported by molto (only %s); "
                "remove the key or switch backend",
                file, declared, expected_name);
        return false;
    }
    if(at == NULL)
        return true; /* a name without a version pins nothing further */

    /* The reported version is a sentence ("clang-format version 22.1.8"), so
       the pin is looked for inside it rather than compared whole. */
    const char *version = at + 1;
    if(backend->version[0] != '\0' && strstr(backend->version, version) == NULL) {
        set_err(err, err_size,
                "%s: backend is pinned to '%s' but this machine has '%s'; "
                "install the pinned version or change the pin",
                file, declared, backend->version);
        return false;
    }
    return true;
}

/* --- format.json -> .clang-format --- */

/* The canonical model, key by key, against clang-format's own vocabulary.
   Verified against clang-format 22.1.8. The modern enums are emitted rather
   than the legacy booleans, which are deprecated since LLVM 17; the backend is
   pinned, so there is no older release to stay compatible with. */
static const char *brace_style_native(brace_style style) {
    switch(style) {
    case brace_style_break:
        return "Stroustrup";
    case brace_style_linux:
        return "Linux";
    case brace_style_allman:
        return "Allman";
    case brace_style_attach:
    default:
        return "Attach";
    }
}

/* What a C project's headers are parsed as. clang-format has no C mode: it
   parses everything as C++ and `Standard` only chooses which dialect. The
   oldest one is the point of the exercise — it is the only setting under which
   `requires`, `concept`, `co_await` and the rest are ordinary identifiers,
   which is what they are in C. */
#define STANDARD_FOR_C "c++03"

/* The C++ standards clang-format names, longest first so "c++2a" is not matched
   by a prefix of "c++2". A dialect outside the table gets the newest, which is
   clang-format's own default and the right guess for something unrecognised. */
static const struct {
    const char *declared;
    const char *native;
} cpp_standards[] = {
    {"c++03", "c++03"}, {"gnu++03", "c++03"}, {"c++11", "c++11"}, {"gnu++11", "c++11"},
    {"c++14", "c++14"}, {"gnu++14", "c++14"}, {"c++17", "c++17"}, {"gnu++17", "c++17"},
    {"c++20", "c++20"}, {"gnu++20", "c++20"}, {"c++2a", "c++20"}, {"gnu++2a", "c++20"},
};
#define CPP_STANDARD_COUNT (sizeof cpp_standards / sizeof cpp_standards[0])

/* The value of `Standard`, from what the manifest declared. An empty cpp_std is
   a C project: nothing in it should be read as a C++ keyword. */
static const char *standard_native(const char *cpp_std) {
    if(cpp_std == NULL || cpp_std[0] == '\0')
        return STANDARD_FOR_C;
    for(size_t i = 0; i < CPP_STANDARD_COUNT; i++) {
        if(strcmp(cpp_standards[i].declared, cpp_std) == 0)
            return cpp_standards[i].native;
    }
    return "Latest";
}

bool style_translate_format_text(const style_config *config, const resolved_tool *backend,
                                 const char *cpp_std, char *out, size_t out_size, char *err,
                                 size_t err_size) {
    if(!check_backend(config->backend, BACKEND_CLANG_FORMAT, backend, "format.json", err, err_size))
        return false;

    size_t used = 0;
    out[0] = '\0';
    bool ok = append(out, out_size, &used, "# Generated by molto from format.json. Do not edit.\n")
              /* The preset is the starting point the explicit keys layer onto. */
              && append(out, out_size, &used, "BasedOnStyle: LLVM\n") &&
              /* Before anything else: it decides how the sources are parsed, and a
                 wrong answer here misformats what the other keys then align. */
              append(out, out_size, &used, "Standard: %s\n", standard_native(cpp_std)) &&
              append(out, out_size, &used, "IndentWidth: %d\n", config->style.indent_width) &&
              append(out, out_size, &used, "ColumnLimit: %d\n", config->style.line_width) &&
              /* ForIndentation, not Always: the canonical key promises to indent with
                 tabs, and Always would also put them inside alignment. */
              append(out, out_size, &used, "UseTab: %s\n",
                     config->style.use_tabs ? "ForIndentation" : "Never") &&
              append(out, out_size, &used, "BreakBeforeBraces: %s\n",
                     brace_style_native(config->style.braces)) &&
              append(out, out_size, &used, "PointerAlignment: %s\n",
                     config->style.pointers == pointer_alignment_left ? "Left" : "Right") &&
              append(out, out_size, &used, "SortIncludes: %s\n",
                     config->style.sort_includes ? "CaseSensitive" : "Never") &&
              append(out, out_size, &used, "SpaceBeforeParens: %s\n",
                     config->style.space_before_paren ? "ControlStatements" : "Never") &&
              append(out, out_size, &used, "ReflowComments: %s\n",
                     config->style.column_limit_comments ? "Always" : "Never");
    if(!ok)
        set_err(err, err_size, "format.json: the translated configuration is too long");
    return ok;
}

/* --- linter.json -> clang-tidy --- */

/* The canonical rule names, and the checks each one is. A name outside this
   table is refused by name: RFC-0005 requires saying which option a backend
   cannot express rather than dropping it. The concepts are chosen so a second
   backend can express them too. */
static const struct {
    const char *rule;
    const char *checks;
} rule_checks[] = {
    {"bugprone", "bugprone-*"},
    {"performance", "performance-*"},
    {"portability", "portability-*"},
    {"modernize", "modernize-*"},
    {"readability", "readability-*"},
    {"security", "clang-analyzer-security-*"},
    {"naming_snake_case", "readability-identifier-naming"},
    {"readability_magic_numbers", "readability-magic-numbers"},
    {"identifier_length", "readability-identifier-length"},
    {"unused", "clang-diagnostic-unused*"},
    {"shadow", "clang-diagnostic-shadow"},
    {"uninitialized", "clang-diagnostic-uninitialized"},
    {"implicit_conversion", "clang-diagnostic-conversion"},
    {"sign_compare", "clang-diagnostic-sign-compare"},
};
#define RULE_CHECK_COUNT (sizeof rule_checks / sizeof rule_checks[0])

static const char *checks_for(const char *rule) {
    for(size_t i = 0; i < RULE_CHECK_COUNT; i++) {
        if(strcmp(rule_checks[i].rule, rule) == 0)
            return rule_checks[i].checks;
    }
    return NULL;
}

/* Whether `list` already contains `item` as a whole comma-separated element.
   Compared element by element rather than with strstr, so "bugprone-*" is not
   found inside "-bugprone-*", which means the opposite. */
static bool check_list_has(const char *list, const char *item) {
    size_t item_length = strlen(item);
    for(const char *at = list; *at != '\0';) {
        const char *comma = strchr(at, ',');
        size_t length = comma != NULL ? (size_t)(comma - at) : strlen(at);
        if(length == item_length && strncmp(at, item, length) == 0)
            return true;
        if(comma == NULL)
            break;
        at = comma + 1;
    }
    return false;
}

/* Build the two comma-separated lists clang-tidy takes: everything that is on,
   and the subset that is fatal. An `off` rule is listed with a minus, which is
   how clang-tidy subtracts it from what the preset enabled.

   A rule the preset already enabled is not appended twice. The duplicate did
   no harm to clang-tidy, which is why it survived, but a generated file is
   read by people when something looks wrong and "bugprone-*,bugprone-*" makes
   the reader doubt the generator before they doubt their configuration. */
static bool compose_check_lists(const lint_config *config, char *checks, size_t checks_size,
                                char *fatal, size_t fatal_size, char *err, size_t err_size) {
    size_t checks_used = 0;
    size_t fatal_used = 0;
    checks[0] = '\0';
    fatal[0] = '\0';

    if(config->preset == style_preset_molto &&
       !append(checks, checks_size, &checks_used, "%s", PRESET_MOLTO_CHECKS)) {
        set_err(err, err_size, "linter.json: the translated check list is too long");
        return false;
    }

    for(size_t i = 0; i < config->rule_count; i++) {
        const lint_rule *rule = &config->rules[i];
        const char *native = checks_for(rule->name);
        if(native == NULL) {
            set_err(err, err_size,
                    "linter.json: rule '%s' is not supported by %s "
                    "(no equivalent check); remove it or switch backend",
                    rule->name, BACKEND_CLANG_TIDY);
            return false;
        }

        /* `off` always emits: a minus subtracts what the preset enabled, so it
           carries meaning even when the plain check is already listed. */
        const bool subtract = rule->severity == lint_severity_off;
        bool ok = true;
        if(subtract || !check_list_has(checks, native))
            ok = append(checks, checks_size, &checks_used, "%s%s%s", checks_used > 0 ? "," : "",
                        subtract ? "-" : "", native);
        if(ok && rule->severity == lint_severity_error && !check_list_has(fatal, native))
            ok = append(fatal, fatal_size, &fatal_used, "%s%s", fatal_used > 0 ? "," : "", native);
        if(!ok) {
            set_err(err, err_size, "linter.json: the translated check list is too long");
            return false;
        }
    }
    return true;
}

bool style_translate_lint_text(const lint_config *config, const resolved_tool *backend, char *out,
                               size_t out_size, char *err, size_t err_size) {
    if(!check_backend(config->backend, BACKEND_CLANG_TIDY, backend, "linter.json", err, err_size))
        return false;

    char checks[CONFIG_TEXT_MAX];
    char fatal[CONFIG_TEXT_MAX];
    if(!compose_check_lists(config, checks, sizeof checks, fatal, sizeof fatal, err, err_size))
        return false;

    size_t used = 0;
    out[0] = '\0';
    bool ok =
        append(out, out_size, &used, "# Generated by molto from linter.json. Do not edit.\n") &&
        append(out, out_size, &used, "Checks: '%s'\n", checks) &&
        append(out, out_size, &used, "WarningsAsErrors: '%s'\n", fatal);
    if(!ok)
        set_err(err, err_size, "linter.json: the translated configuration is too long");
    return ok;
}

/* --- writing --- */

/* Write `text` to <root>/.bin/style/<filename>, creating the directory. */
static bool write_generated(const char *root, const char *filename, const char *text,
                            char *out_path, size_t out_path_size, char *err, size_t err_size) {
    char directory[STYLE_CONFIG_PATH_MAX];
    if(!fs_format_path(directory, sizeof directory, "%s/" STYLE_DIR, root) ||
       !fs_make_dirs(directory)) {
        set_err(err, err_size, "could not create '%s'", directory);
        return false;
    }
    if(!fs_format_path(out_path, out_path_size, "%s/%s", directory, filename)) {
        set_err(err, err_size, "path too long to compose (%s)", filename);
        return false;
    }
    if(!fs_write_file(out_path, text)) {
        set_err(err, err_size, "could not write '%s'", out_path);
        return false;
    }
    return true;
}

bool style_translate_format(const char *root, const style_config *config,
                            const resolved_tool *backend, const char *cpp_std, char *out_path,
                            size_t out_path_size, char *err, size_t err_size) {
    char text[CONFIG_TEXT_MAX];
    return style_translate_format_text(config, backend, cpp_std, text, sizeof text, err,
                                       err_size) &&
           write_generated(root, FORMAT_CONFIG, text, out_path, out_path_size, err, err_size);
}

bool style_translate_lint(const char *root, const lint_config *config, const resolved_tool *backend,
                          char *out_path, size_t out_path_size, char *err, size_t err_size) {
    char text[CONFIG_TEXT_MAX];
    return style_translate_lint_text(config, backend, text, sizeof text, err, err_size) &&
           write_generated(root, LINT_CONFIG, text, out_path, out_path_size, err, err_size);
}
