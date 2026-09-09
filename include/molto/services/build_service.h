#ifndef MOLTO_BUILD_SERVICE_H
#define MOLTO_BUILD_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/build/profile.h>
#include <molto/build/report.h>
#include <molto/project/project_ctx.h>
#include <molto/services/process_service.h>
#include <molto/services/toolchain_service.h>
#include <molto/util/str_list.h>

/* Build the project rooted at `root` (may be ".") using `profile`.
   Discovers sources under `root/src`, compiles them incrementally, and links
   an executable at `root/build/<profile>/<package-name>`. When `out_binary` is
   not NULL and the build succeeds, the executable path is written into it
   (`out_binary_size` bytes). `refresh_toolchain` re-resolves the compiler
   instead of using the one recorded in the workspace database. `jobs` caps how
   many translation units compile at once; 0 takes the whole machine, which is
   what a build does when `-j` is not given.

   Also writes `root/compile_commands.json` describing every unit it compiled,
   including the ones it found up to date: it is what the tools that parse this
   code without being the build read (RFC-0007).
   Returns a molto_exit_code. */
[[nodiscard]] int build_project(const char *root, build_profile profile, const char *platform,
                                bool refresh_toolchain, size_t jobs, char *out_binary,
                                size_t out_binary_size);

/* The same build, saying what it is doing.
 *
 * The report is the caller's, because what a build says belongs to the command
 * a person typed and not to the service: `molto build` wants an inventory and a
 * bar, and a test suite building a hundred fixtures wants silence. `NULL` is
 * that silence, which is what the plain `build_project` above passes.
 *
 * When `chain_out` is not NULL it receives the toolchain the build resolved, for
 * the same reason `build_tests_with` hands back the manifest's [env]: whoever
 * runs the result has to run it under the terms it was built with, and where the
 * toolchain keeps its shared libraries is not derivable from the binary. */
[[nodiscard]] int build_project_with(const char *root, build_profile profile, const char *platform,
                                     bool refresh_toolchain, size_t jobs, char *out_binary,
                                     size_t out_binary_size, resolved_toolchain *chain_out,
                                     build_report *report);

/* Translate a manifest's [env] table into the plain pairs process_service
   expects, writing at most `capacity` of them. Returns how many were written.
   Lives here because the build service is what bridges the manifest model and
   the process service; neither of those needs to know about the other. */
size_t project_env_to_vars(const project_env *env, process_env_var *vars, size_t capacity);

/* Room for the manifest's [env] plus the one variable the toolchain adds. */
#define PROJECT_RUN_MAX_VARS (PROJECT_MAX_ENV + 1)

/* The variables a program this build produced has to run under: the manifest's
   [env], and — when the resolved toolchain keeps shared libraries of its own —
   the loader search path that lets the program find them. Compiling with a
   toolchain and then launching its output without that path is how a build
   succeeds and the program it produced does not start.

   `path_buffer` receives the composed path and must outlive `vars`, which point
   into it. A manifest that sets the variable itself keeps it: an explicit [env]
   entry is a decision, and overriding it here would be molto second-guessing
   the project. Returns how many vars were written. */
size_t project_run_vars(const project_env *env, const resolved_toolchain *chain,
                        process_env_var *vars, size_t capacity, char *path_buffer,
                        size_t path_buffer_size);

/* Enough room for what project_env_fingerprint can write. */
#define PROJECT_ENV_FINGERPRINT_MAX                                                                \
    (PROJECT_MAX_ENV * (PROJECT_ENV_NAME_MAX + PROJECT_ENV_VALUE_MAX + 1) + 1)

/* The [env] table as the one string that answers "would this run in the same
   environment": "NAME=value" per entry, in the order read_env sorted them,
   separated by a byte no manifest can produce.

   Returns the length written, and 0 with an empty `out` when there is nothing
   to say. That case is not an edge to tidy up later — it is what keeps a
   project without [env] fingerprinting byte for byte as it always did, and so
   keeps every workspace database and cached object already on disk valid. */
size_t project_env_fingerprint(const project_env *env, char *out, size_t size);

/* Build the project's test executables: compiles `root/src` and then compiles
   and links each source under `root/tests` into its own executable at
   `root/build/<profile>/tests/<name>`, linked against the project's objects
   (excluding the app's src/main.c). Appends every built test binary path to
   `test_binaries_out` (caller-initialised, caller-freed). A missing or empty
   tests/ directory is not an error. `jobs` caps the parallelism as in
   build_project. Returns a molto_exit_code.

   The `compile_commands.json` this writes covers tests/ as well as src/, so it
   is a superset of the one `molto build` leaves — running the tests is what
   makes an editor able to follow a test into the code it exercises.

   When `env_out` is not NULL it receives the manifest's [env], so that whoever
   runs these binaries runs them in the environment they were built in. It is
   handed back rather than read again from the manifest because the build may
   rewrite Project.toml on its way through, and a test that runs under a
   different environment than the one that compiled it is the bug this avoids. */
[[nodiscard]] int build_tests(const char *root, build_profile profile, const char *platform,
                              bool refresh_toolchain, size_t jobs, str_list *test_binaries_out,
                              project_env *env_out);

/* The same test build, saying what it is doing. See build_project_with. */
[[nodiscard]] int build_tests_with(const char *root, build_profile profile, const char *platform,
                                   bool refresh_toolchain, size_t jobs, str_list *test_binaries_out,
                                   project_env *env_out, resolved_toolchain *chain_out,
                                   build_report *report);

#endif /* MOLTO_BUILD_SERVICE_H */
