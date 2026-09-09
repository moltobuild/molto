#ifndef MOLTO_TOOLCHAIN_SERVICE_H
#define MOLTO_TOOLCHAIN_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/project_ctx.h>
#include <molto/workspace/wsdb.h>

/* Size of the buffers holding a resolved compiler path. */
#define TOOLCHAIN_PATH_MAX 4096

/* The link recipe's shape, mirroring the limits pickup composes it under
   (RECIPE_MAX_FLAGS, RECIPE_FLAG_MAX, RECIPE_MAX_DIRS in its recipe.h). Held
   the same size on purpose: a consumer narrower than the producer would drop
   the tail of a valid answer and build something that does not run, which is
   the failure this whole struct exists to prevent. */
#define TOOLCHAIN_MAX_FLAGS 8
#define TOOLCHAIN_FLAG_MAX 1024
#define TOOLCHAIN_MAX_DIRS 4

/* The identity, target triple, standard flag and C++ standard library pickup
   reports beside the compiler itself. */
#define TOOLCHAIN_ID_MAX 128
#define TOOLCHAIN_TARGET_MAX 128
#define TOOLCHAIN_STD_FLAG_MAX 32
#define TOOLCHAIN_STDLIB_MAX 32

/* The compilers Molto will invoke for this project, and the terms under which
   the resolver said they work.
 *
 * The paths alone are not an answer. A toolchain that keeps its runtime beside
 * the compiler compiles perfectly well and produces a program that cannot
 * start, so pickup answers with the flags that make the result runnable and the
 * directories its shared libraries actually live in. Carrying only `cc` and
 * `cxx` is what threw that away. */
typedef struct {
    char cc[TOOLCHAIN_PATH_MAX];  /* C driver */
    char cxx[TOOLCHAIN_PATH_MAX]; /* C++ driver; "" when none was found */
    char vendor[32];
    char version[32];

    /* Identity, so this same toolchain can be named again without a path. */
    char id[TOOLCHAIN_ID_MAX];
    /* What its compilers emit code for, in the compiler's own spelling. Not
       the catalogue's: `pickup host` answers that other question, and the two
       strings disagree by design. */
    char target[TOOLCHAIN_TARGET_MAX];
    /* The standard flag pickup proved this compiler accepts, e.g. "-std=c23".
       Empty when the request named no standard. */
    char std_flag[TOOLCHAIN_STD_FLAG_MAX];
    /* Which C++ standard library the flags below commit the build to. An ABI,
       not a preference: objects built against libc++ and against libstdc++
       cannot be linked together. Empty for a C-only resolution. */
    char stdlib[TOOLCHAIN_STDLIB_MAX];

    /* What the resolver says must reach the compile line and the link line for
       this toolchain to produce a working program. */
    char compile_flags[TOOLCHAIN_MAX_FLAGS][TOOLCHAIN_FLAG_MAX];
    size_t compile_flag_count;
    char link_flags[TOOLCHAIN_MAX_FLAGS][TOOLCHAIN_FLAG_MAX];
    size_t link_flag_count;

    /* Where the shared libraries the produced program needs actually live.
       Linking is not running: a caller that has to launch what it built cannot
       derive these from the flags. */
    char runtime_dirs[TOOLCHAIN_MAX_DIRS][TOOLCHAIN_PATH_MAX];
    size_t runtime_dir_count;
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

/* --- running what was built --- */

/* Room for every runtime directory plus the value the variable already held.
   The tail is generous because PATH on a developer's Windows box routinely
   runs to thousands of bytes and truncating it would break the program in a
   new way rather than fix the old one. */
#define TOOLCHAIN_RUNTIME_PATH_MAX (TOOLCHAIN_MAX_DIRS * (TOOLCHAIN_PATH_MAX + 1) + 32768)

/* The environment variable this platform's loader reads to find shared
   libraries at run time: PATH on Windows, DYLD_LIBRARY_PATH on macOS,
   LD_LIBRARY_PATH elsewhere. */
[[nodiscard]] const char *toolchain_runtime_path_var(void);

/* Compose the value that variable must hold for a program built with `chain`
   to start: the toolchain's own runtime directories first, then whatever the
   variable already held, so the toolchain that built the program wins and the
   rest of the user's environment still works.

   Returns false when there is nothing to say — no runtime directories, or the
   composed value would not fit — in which case the caller exports nothing and
   the child inherits the environment unchanged. */
[[nodiscard]] bool toolchain_runtime_path(const resolved_toolchain *chain, char *out,
                                          size_t out_size);

#endif /* MOLTO_TOOLCHAIN_SERVICE_H */
