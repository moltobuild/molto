#include <molto/services/tool_service.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/util/str_list.h>
#include <molto/util/toml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The resolver Molto asks, and how to find it. Shared with toolchain_service:
   one pickup answers both questions. */
#define PICKUP_PROGRAM "pickup"
#define PICKUP_PATH_ENV "MOLTO_PICKUP" /* explicit path, for an uninstalled build */

/* Manual overrides. Setting one bypasses resolution entirely. */
#define FORMATTER_OVERRIDE_ENV "MOLTO_CLANG_FORMAT"
#define LINTER_OVERRIDE_ENV "MOLTO_CLANG_TIDY"

/* The pickup sub-command and its arguments. */
#define ARG_TOOLS "tools"
#define ARG_FORMAT "--format"
#define FORMAT_TOML "toml"

/* The array of tables pickup answers with, and its keys. */
#define ANSWER_ARRAY "tool"
#define ANSWER_KIND "kind"
#define ANSWER_NAME "name"
#define ANSWER_PATH "path"
#define ANSWER_VERSION "version"

/* The workspace database keys holding each resolved tool. */
#define FORMATTER_KEY "tool:formatter"
#define LINTER_KEY "tool:linter"

/* Size of the buffer receiving pickup's answer. */
#define ANSWER_SIZE 8192

/* Fields recorded per tool: path, version, name. The path goes first because
   the database registers it as an input, so replacing the binary invalidates
   the entry on its own. */
#define RECORDED_FIELDS 3

const char *tool_kind_name(tool_kind kind) {
    return kind == tool_kind_formatter ? "formatter" : "linter";
}

static const char *override_env_of(tool_kind kind) {
    return kind == tool_kind_formatter ? FORMATTER_OVERRIDE_ENV : LINTER_OVERRIDE_ENV;
}

static const char *wsdb_key_of(tool_kind kind) {
    return kind == tool_kind_formatter ? FORMATTER_KEY : LINTER_KEY;
}

/* Where pickup lives: an explicit path when given, otherwise the PATH. */
static const char *pickup_program(void) {
    const char *explicit_path = getenv(PICKUP_PATH_ENV);
    if(explicit_path != NULL && explicit_path[0] != '\0')
        return explicit_path;
    return PICKUP_PROGRAM;
}

/* Take the tool straight from the environment, bypassing resolution. Returns
   false when the variable is not set, which is the normal case. */
static bool take_from_environment(tool_kind kind, resolved_tool *out) {
    const char *path = getenv(override_env_of(kind));
    if(path == NULL || path[0] == '\0')
        return false;

    memset(out, 0, sizeof *out);
    if(!fs_format_path(out->path, sizeof out->path, "%s", path))
        return false;
    (void)fs_format_path(out->name, sizeof out->name, "%s", tool_kind_name(kind));
    return true;
}

/* Serve the answer recorded in the workspace database. */
static bool take_from_wsdb(tool_kind kind, wsdb *db, resolved_tool *out) {
    const char *key = wsdb_key_of(kind);
    if(db == NULL || !wsdb_toolchain_fresh(db, key, tool_kind_name(kind)))
        return false;

    str_list values;
    str_list_init(&values);
    bool ok = wsdb_toolchain_values(db, key, &values) && str_list_count(&values) >= RECORDED_FIELDS;
    if(ok) {
        memset(out, 0, sizeof *out);
        ok = fs_format_path(out->path, sizeof out->path, "%s", str_list_get(&values, 0)) &&
             fs_format_path(out->version, sizeof out->version, "%s", str_list_get(&values, 1)) &&
             fs_format_path(out->name, sizeof out->name, "%s", str_list_get(&values, 2));
    }
    str_list_free(&values);
    return ok;
}

/* Record an answer so the next command does not have to ask again. */
static void remember(tool_kind kind, wsdb *db, const resolved_tool *tool) {
    if(db == NULL)
        return;
    str_list values;
    str_list_init(&values);
    bool ok = str_list_push(&values, tool->path) && str_list_push(&values, tool->version) &&
              str_list_push(&values, tool->name);
    if(!ok || !wsdb_record_toolchain(db, wsdb_key_of(kind), tool_kind_name(kind), &values))
        fprintf(stderr, "molto: warning: could not record the resolved %s\n", tool_kind_name(kind));
    str_list_free(&values);
}

/* Find the tool of `kind` in pickup's answer. Molto already parses TOML, which
   is why pickup speaks it: consuming the resolver adds no parser here. */
static bool parse_answer(const char *toml, tool_kind kind, resolved_tool *out) {
    char err[256] = "";
    toml_document *doc = toml_parse(toml, err, sizeof err);
    if(doc == NULL)
        return false;

    bool found = false;
    size_t count = toml_table_array_count(doc, ANSWER_ARRAY);
    for(size_t i = 0; !found && i < count; i++) {
        char section[TOML_SECTION_MAX];
        char reported[TOOL_NAME_MAX] = "";
        if(!toml_table_array_section(ANSWER_ARRAY, i, section, sizeof section) ||
           !toml_get_string(doc, section, ANSWER_KIND, reported, sizeof reported) ||
           strcmp(reported, tool_kind_name(kind)) != 0)
            continue;

        memset(out, 0, sizeof *out);
        found = toml_get_string(doc, section, ANSWER_PATH, out->path, sizeof out->path);
        /* The rest is descriptive; a missing field leaves it empty rather than
           failing a resolution that already named a binary. */
        (void)toml_get_string(doc, section, ANSWER_NAME, out->name, sizeof out->name);
        (void)toml_get_string(doc, section, ANSWER_VERSION, out->version, sizeof out->version);
    }
    toml_free(doc);
    return found && out->path[0] != '\0';
}

/* Ask pickup, and turn its failures into messages that say what to do. */
static int ask_pickup(tool_kind kind, resolved_tool *out) {
    const char *program = pickup_program();
    const char *const argv[] = {program, ARG_TOOLS, ARG_FORMAT, FORMAT_TOML, NULL};

    char answer[ANSWER_SIZE];
    int status = process_capture(argv, answer, sizeof answer);
    if(status != 0) {
        fprintf(stderr,
                "molto: could not run '%s' to find a %s.\n"
                "  Install pickup, or point " PICKUP_PATH_ENV " at it,\n"
                "  or set %s to the binary.\n",
                program, tool_kind_name(kind), override_env_of(kind));
        return exit_build_failure;
    }
    if(!parse_answer(answer, kind, out)) {
        /* Pickup ran and answered; this machine simply has no tool of this
           kind. That is a fact to act on, not a malfunction. */
        return exit_dependency_failure;
    }
    return exit_ok;
}

int tool_resolve(tool_kind kind, wsdb *db, bool refresh, resolved_tool *out) {
    /* An explicit choice by the user outranks resolution, and is not cached:
       the command line already carries the path, so changing the variable takes
       effect on its own. */
    if(take_from_environment(kind, out))
        return exit_ok;

    if(!refresh && take_from_wsdb(kind, db, out))
        return exit_ok;

    int code = ask_pickup(kind, out);
    if(code == exit_ok)
        remember(kind, db, out);
    return code;
}
