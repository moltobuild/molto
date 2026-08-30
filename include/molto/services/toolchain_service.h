#ifndef MOLTO_TOOLCHAIN_SERVICE_H
#define MOLTO_TOOLCHAIN_SERVICE_H

#include <stdbool.h>

#include <molto/project/project_ctx.h>
#include <molto/workspace/wsdb.h>

/* Size of the buffers holding a resolved compiler path. */
#define TOOLCHAIN_PATH_MAX 4096

/* The compilers Molto will invoke for this project. */
typedef struct {
    char cc[TOOLCHAIN_PATH_MAX];  /* C driver */
    char cxx[TOOLCHAIN_PATH_MAX]; /* C++ driver; "" when none was found */
    char vendor[32];
    char version[32];
} resolved_toolchain;

/*
 * Turn what a manifest asks for into the compiler that provides it here.
 *
 * A manifest states capabilities — a standard, features that must really
 * compile — and never a binary, because a binary is a fact about one machine.
 * Pickup answers that question; this is the only part of Molto that knows it
 * exists.
 *
 * The answer is recorded in the workspace database, so the query happens once
 * rather than on every build. It is asked again when the request changes, when
 * the compiler it named is replaced, or when `refresh` demands it.
 *
 * `needs_cpp` says the project has C++ sources, which makes a C++ driver part
 * of the request: a toolchain without one cannot build it, however well it
 * matches otherwise.
 *
 * `C_COMPILER` and `CPP_COMPILER` override everything: setting them is a
 * deliberate choice to bypass resolution, so they win and nothing is cached.
 *
 * Returns a molto_exit_code; on failure the reason is already on stderr.
 */
/* What the registry calls this machine, from `pickup host`.

   Asked rather than derived. Pickup owns the target vocabulary because pickup
   is what downloads published artifacts, and a second derivation here would
   disagree with it the first time an architecture is spelled differently — the
   compiler's own triple is `x86_64-unknown-linux-gnu` and the catalogue
   publishes under `linux-x86_64`, which is exactly that disagreement already
   sitting in the open.

   False when pickup cannot be run or answers that nothing is published for this
   host. Not cached: it costs one process, it cannot change while molto runs,
   and a wrong answer kept in a database is worse than a cheap right one. */
[[nodiscard]] bool toolchain_host_target(char *out, size_t out_size);

/* `platform` is the triple the code is being built for, or NULL for this
   machine's own. It reaches pickup as `--target` and it is part of the request
   string, so a cross build never answers itself with the compiler the host
   build resolved: the question changed, and the remembered answer was to the
   other one. */
[[nodiscard]] int toolchain_resolve(const project_target *target, const char *platform,
                                    bool needs_cpp, wsdb *db, bool refresh,
                                    resolved_toolchain *out);

#endif /* MOLTO_TOOLCHAIN_SERVICE_H */
