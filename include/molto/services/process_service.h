#ifndef MOLTO_PROCESS_SERVICE_H
#define MOLTO_PROCESS_SERVICE_H

#include <stddef.h>

/* One environment variable to export to a child process. Kept as plain strings
   so this service stays independent of the manifest model. */
typedef struct {
    const char *name;
    const char *value;
} process_env_var;

/* Run a command described by a NULL-terminated `argv`, inheriting stdio so
   the child's output (e.g. compiler diagnostics) is shown verbatim.
   Returns:
     - the child's exit code (0-255) if it exited normally;
     - 128 + N if it was killed by signal N (shell convention);
     - -1 if the process could not be started (fork/wait failed). */
[[nodiscard]] int process_run(const char *const argv[]);

/* As process_run, with `env` exported to the child only: the variables are set
   after forking, so Molto's own environment is never modified. A NULL or empty
   `env` behaves exactly like process_run. */
[[nodiscard]] int process_run_env(const char *const argv[],
                                  const process_env_var *env, size_t env_count);

/* Run `argv` and capture what it writes to stdout into `out`, NUL-terminated
   and truncated to `out_size`. Its stderr is left alone, so a tool Molto
   interrogates can still explain itself to the user while its answer is read
   here. Returns the same codes as process_run. */
[[nodiscard]] int process_capture(const char *const argv[],
                                  char *out, size_t out_size);

#endif /* MOLTO_PROCESS_SERVICE_H */
