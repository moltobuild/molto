#ifndef MOLTO_STYLE_TRANSLATE_H
#define MOLTO_STYLE_TRANSLATE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/style_config.h>
#include <molto/services/tool_service.h>

/*
 * The canonical style model, rendered as the backend's own configuration.
 *
 * The generated file is machine-owned and lives under `.bin/`, never in the
 * project tree, so `.clang-format` files do not accumulate in repositories that
 * use Molto (RFC-0005).
 *
 * Coverage is not uniform and Molto must not pretend otherwise: an option with
 * no faithful translation for the selected backend fails with a diagnostic
 * naming both the option and the backend, rather than being dropped. Silently
 * ignoring one would leave the user with a style they did not ask for and no
 * indication why.
 */

/* Size of the buffer a caller passes for the generated config's path. */
#define STYLE_CONFIG_PATH_MAX 4096

/* Render `config` as clang-format YAML under <root>/.bin/, writing the path of
   the generated file into `out_path`. False with a message in `err`.

   `cpp_std` is the manifest's `[target].cpp_std`, empty for a C project. The
   formatter has to be told: a `.h` carries no language in its extension, so
   clang-format assumes the newest C++ and parses a C header with C++ keywords.
   A field named `requires` then reads as a C++20 requires-clause and the
   declaration is torn across three lines. Passing the standard the project
   declared is what keeps a C project from being formatted as C++. */
[[nodiscard]] bool style_translate_format(const char *root, const style_config *config,
                                          const resolved_tool *backend, const char *cpp_std,
                                          char *out_path, size_t out_path_size, char *err,
                                          size_t err_size);

/* Render `config` as clang-tidy YAML under <root>/.bin/. Rules become the
   Checks list, and the ones set to `error` also become WarningsAsErrors. */
[[nodiscard]] bool style_translate_lint(const char *root, const lint_config *config,
                                        const resolved_tool *backend, char *out_path,
                                        size_t out_path_size, char *err, size_t err_size);

/* Render `config` as the text of a clang-format configuration, without writing
   it anywhere. Exposed for the tests, which check the translation itself. */
[[nodiscard]] bool style_translate_format_text(const style_config *config,
                                               const resolved_tool *backend, const char *cpp_std,
                                               char *out, size_t out_size, char *err,
                                               size_t err_size);

/* Render `config` as the text of a clang-tidy configuration. */
[[nodiscard]] bool style_translate_lint_text(const lint_config *config,
                                             const resolved_tool *backend, char *out,
                                             size_t out_size, char *err, size_t err_size);

#endif /* MOLTO_STYLE_TRANSLATE_H */
