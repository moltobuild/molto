#ifndef MOLTO_CONFIGURE_SERVICE_H
#define MOLTO_CONFIGURE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/services/recipe_service.h>

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

/* Configure `name`'s source at `root` for the compiler `cc`, or do nothing if
   the stamp says it already is. `target` is the `--target` triple, or NULL for
   this machine; a cross build passes it to configure as `--host`. False with
   a message naming the dependency, the step and where its log is. */
[[nodiscard]] bool configure_dependency(const char *name, const char *root,
                                        const recipe_build *build, const char *cc,
                                        const char *target, char *err, size_t err_size);

/* The digest the stamp holds, for `build` and `cc`. Split out so the rule for
   when configure runs again is testable without running it. */
void configure_fingerprint(const recipe_build *build, const char *cc, const char *target,
                           char hex_out[65]);

#endif /* MOLTO_CONFIGURE_SERVICE_H */
