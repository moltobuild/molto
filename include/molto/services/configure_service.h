#ifndef MOLTO_CONFIGURE_SERVICE_H
#define MOLTO_CONFIGURE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/services/compile_lines.h>
#include <molto/services/recipe_service.h>
#include <molto/util/str_list.h>

/*
 * Running upstream's configure for a dependency (RFC-0009, `[build]
 * system = "autotools"`, `via = "delegate"`).
 *
 * A library whose release tarball ships `pg_config.h.in` rather than
 * `pg_config.h` cannot be compiled until something has answered its questions
 * about this machine. That something is its own `configure`, run once in the
 * unpacked source in molto's cache: `sh ./configure <args>` with `CC` set to the
 * compiler molto resolved and the recipe's `env`, then `make -C <dir> <file>`
 * for each of the recipe's `targets`. molto then compiles `[artifacts].sources`
 * itself; it never asks make to build the library.
 *
 * The answer is kept: a stamp in the source records a digest of everything
 * that shaped it (the arguments, the environment, the targets and the
 * compiler), and a later build with the same digest runs nothing. A different
 * compiler is a different machine as far as configure is concerned, so it is
 * asked again.
 *
 * `sh` and `make` come from the PATH. On Windows that is MSYS2's, which is
 * what upstream's own Windows instructions use.
 */

/* What a configuration may see of the dependencies molto already resolved:
   their include directories and their `-L` directories. autotools gets them as
   CPPFLAGS and LDFLAGS; CMake as CMAKE_INCLUDE_PATH, CMAKE_LIBRARY_PATH and a
   CMAKE_PREFIX_PATH of each include directory's parent, which is what its
   find modules search (OpenSSL's, for libwebsockets). Either list may be
   NULL. */
typedef struct {
    const str_list *includes;
    const str_list *link_flags;
} configure_view;

/* Configure `name`'s source at `root` for the compiler `cc`, or do nothing if
   the stamp says it already is. `target` is the `--target` triple, or NULL for
   this machine; a cross build passes it to configure as `--host`. `view` may
   be NULL. False with a message naming the dependency, the step and where its
   log is. */
[[nodiscard]] bool configure_dependency(const char *name, const char *root,
                                        const recipe_build *build, const char *cc,
                                        const char *target, const configure_view *view, char *err,
                                        size_t err_size);

/* The digest the stamp holds. Split out so the rule for when configure runs
   again is testable without running it. */
void configure_fingerprint(const recipe_build *build, const char *cc, const char *target,
                           const configure_view *view, char hex_out[65]);

/* What `name`'s configured build compiles, when its recipe says to ask it
   (`[build].sources`, RFC-0025): `make -n` or CMake's compile_commands.json,
   read once per configuration and kept beside the stamp. Nothing, and true,
   for a recipe that lists its own sources. Call after configure_dependency,
   with the same compiler. */
[[nodiscard]] bool configure_compile_lines(const char *name, const char *root,
                                           const recipe_build *build, const char *cc,
                                           compile_lines *out, char *err, size_t err_size);

/* Where a delegated CMake configuration writes, relative to the source: what a
   recipe's include paths name for the headers it generates. */
#define CONFIGURE_CMAKE_DIR ".molto-cmake"

#endif /* MOLTO_CONFIGURE_SERVICE_H */
