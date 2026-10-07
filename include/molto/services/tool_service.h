#ifndef MOLTO_TOOL_SERVICE_H
#define MOLTO_TOOL_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

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

/*
 * A build tool a dependency's configuration runs: `cmake` or `ninja`
 * (RFC-0023, `[tool].kind = "build"`; RFC-0009, `system = "cmake"`).
 *
 * MOLTO_CMAKE and MOLTO_NINJA name one outright. Otherwise pickup is asked
 * which build tool of that name it has (`pickup install cmake` puts one
 * there), and failing that the PATH is: a machine with its own CMake does not
 * need pickup's. False, with a message saying how to get one, when none of the
 * three answers. Not cached: a configuration runs once per compiler.
 */
[[nodiscard]] bool tool_resolve_build(const char *name, resolved_tool *out, char *err,
                                      size_t err_size);

/* The name of a kind, for messages: "formatter", "linter". Never NULL. */
[[nodiscard]] const char *tool_kind_name(tool_kind kind);

/* The variable an analysis tool reads to learn where the macOS SDK is, and room
   for the path it holds. */
#define TOOL_SDK_ENV "SDKROOT"
#define TOOL_SDK_PATH_MAX 4096

/*
 * Where the platform's SDK is, for a tool that will not find it by itself.
 *
 * Only macOS needs this. Its system headers live in an SDK rather than in
 * /usr/include, and Apple's clang finds that SDK because Apple taught its
 * driver to; the clang-tidy pickup unpacks is upstream LLVM, which was not, and
 * reports `'stdio.h' file not found` on every source it is handed.
 *
 * On macOS the answer is SDKROOT when the environment already names one — that
 * is how a user, or Xcode, says which SDK they mean — and otherwise what
 * `xcrun --show-sdk-path` says (MOLTO_XCRUN overrides which xcrun, as
 * MOLTO_PKG_CONFIG does for pkg-config). It is written to `out` and true is
 * returned. False, with `err` saying why, when xcrun could not answer.
 *
 * Everywhere else there is no SDK to find: `out` is left empty, true is
 * returned and nothing is run.
 */
[[nodiscard]] bool tool_platform_sdk(char *out, size_t out_size, char *err, size_t err_size);

#endif /* MOLTO_TOOL_SERVICE_H */
