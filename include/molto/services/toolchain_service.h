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
[[nodiscard]] int toolchain_resolve(const project_target *target, bool needs_cpp, wsdb *db,
                                    bool refresh, resolved_toolchain *out);

#endif /* MOLTO_TOOLCHAIN_SERVICE_H */
