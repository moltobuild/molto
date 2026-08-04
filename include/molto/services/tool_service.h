#ifndef MOLTO_TOOL_SERVICE_H
#define MOLTO_TOOL_SERVICE_H

#include <stdbool.h>

#include <molto/workspace/wsdb.h>

/* Sizes of the fields describing one resolved tool. */
#define TOOL_NAME_MAX 64
#define TOOL_PATH_MAX 4096
#define TOOL_VERSION_MAX 128

typedef enum {
    tool_kind_formatter, /* clang-format */
    tool_kind_linter,    /* clang-tidy */
} tool_kind;

/* A style tool this machine has, and where. */
typedef struct {
    char name[TOOL_NAME_MAX];       /* "clang-format" */
    char path[TOOL_PATH_MAX];       /* absolute, exactly as pickup gave it */
    char version[TOOL_VERSION_MAX]; /* "clang-format version 22.1.8" */
} resolved_tool;

/*
 * Which formatter or linter to run, and from where.
 *
 * Molto does not look for these, does not install them and does not rewrite
 * their paths. Pickup already answers that question — `pickup tools` reports
 * the kind, the name, the path and the version, and pickup unpacks
 * clang-format and clang-tidy alongside the compiler — so Molto asks, takes the
 * path and runs it. It is the same split as with the compiler
 * (see toolchain_service): pickup provides the toolchain, Molto orchestrates it.
 *
 * The answer is recorded in the workspace database so the query happens once
 * rather than on every command, and re-asked when `refresh` demands it or when
 * the binary it named is replaced.
 *
 * MOLTO_CLANG_FORMAT and MOLTO_CLANG_TIDY override everything: setting one is a
 * deliberate choice to bypass resolution, so it wins and nothing is cached.
 *
 * Returns a molto_exit_code. exit_dependency_failure means this machine has no
 * tool of that kind, which callers may treat as a reason to do less rather than
 * as an error: `molto lint` still has the compiler.
 */
[[nodiscard]] int tool_resolve(tool_kind kind, wsdb *db, bool refresh, resolved_tool *out);

/* The name of a kind, for messages: "formatter", "linter". Never NULL. */
[[nodiscard]] const char *tool_kind_name(tool_kind kind);

#endif /* MOLTO_TOOL_SERVICE_H */
