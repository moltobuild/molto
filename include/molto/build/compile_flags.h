#ifndef MOLTO_COMPILE_FLAGS_H
#define MOLTO_COMPILE_FLAGS_H

#include <stdbool.h>

#include <molto/project/project_ctx.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/str_list.h>

/*
 * Turning a manifest into compiler arguments.
 *
 * It lives apart from the build service because the build is no longer the only
 * thing that needs it: `molto lint` runs the compiler in a syntax-only pass, and
 * hands clang-tidy the same arguments after a `--`. Two copies of the rules
 * below would drift, and both are contracts from RFC-0003 rather than
 * conveniences: a relative include is anchored at the project root, and `flags`
 * is passed verbatim.
 */

/* The driver to invoke for a translation unit. NULL when C++ is needed and the
   resolved toolchain has no C++ driver: reporting that beats invoking the C one
   and letting the linker fail with something unrelated. */
[[nodiscard]] const char *compile_flags_driver(const resolved_toolchain *chain, bool is_cpp);

/* Push "<prefix><value>" onto argv: "-DFOO=1", "-lm". The buffer is sized for a
   manifest option value, not for a path — see compile_flags_push_include. */
[[nodiscard]] bool compile_flags_push_prefixed(str_list *argv, const char *prefix,
                                               const char *value);

/* Push one include directory as "-I<dir>".

   A relative one is anchored at the project root, not at the working
   directory. The manifest describes the project, so `include = ["vendor"]`
   means the project's vendor/ — and the compiler runs wherever the user
   happened to invoke molto from, which is not necessarily the root. */
[[nodiscard]] bool compile_flags_push_include(str_list *argv, const char *root,
                                              const char *directory);

/* Append a scope's defines (-D), include dirs (-I) and raw flags to argv.
   Only include dirs are anchored: defines are not paths, and flags are a raw
   escape hatch that RFC-0003 promises to pass verbatim. */
[[nodiscard]] bool compile_flags_push_options(str_list *argv, const char *root,
                                              const project_options *options);

/* Push "-std=<name>" for the language of the translation unit, from `cpp_std`
   when it is C++ and `std` otherwise. Pushes nothing, and succeeds, when the
   manifest declares no standard for that language. */
[[nodiscard]] bool compile_flags_push_std(str_list *argv, const project_target *target,
                                          bool is_cpp);

#endif /* MOLTO_COMPILE_FLAGS_H */
