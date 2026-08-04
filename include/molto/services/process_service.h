#ifndef MOLTO_PROCESS_SERVICE_H
#define MOLTO_PROCESS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/util/str_list.h>

/* One environment variable to export to a child process. Kept as plain strings
   so this service stays independent of the manifest model. */
typedef struct {
    const char *name;
    const char *value;
} process_env_var;

/* Where one of the child's output streams goes. */
typedef enum {
    process_stream_inherit, /* to Molto's own stdout/stderr, verbatim */
    process_stream_capture, /* into the spec's buffer */
} process_stream;

/* What to run and how to connect it. The convenience functions below are this
   struct filled in different ways: the fork/exec exists once, and adding a
   destination does not touch it. */
typedef struct {
    const process_env_var *env;
    size_t env_count;
    process_stream stdout_to;
    process_stream stderr_to;
    char *capture; /* where captured output goes; NULL if nothing is */
    size_t capture_size;
    bool truncated; /* out: the output did not fit in `capture` */
} process_spec;

/* Run `argv` as described by `spec`. Returns:
     - the child's exit code (0-255) if it exited normally;
     - 128 + N if it was killed by signal N (shell convention);
     - -1 if the process could not be started (fork/wait failed);
     - 127 if the command could not be executed, as a shell reports it.

   Both streams that are captured share one pipe, so what the child wrote is
   kept in the order it wrote it. Two pipes would need the parent to poll them —
   a child filling the one not being drained blocks forever — and the
   distinction buys nothing for the tools Molto runs: a compiler diagnoses on
   stderr, clang-tidy on stdout, and both read the same either way. */
[[nodiscard]] int process_execute(const char *const argv[], process_spec *spec);

/* Run a command described by a NULL-terminated `argv`, inheriting stdio so
   the child's output (e.g. compiler diagnostics) is shown verbatim. */
[[nodiscard]] int process_run(const char *const argv[]);

/* As process_run, with `env` exported to the child only: the variables are set
   after forking, so Molto's own environment is never modified. A NULL or empty
   `env` behaves exactly like process_run. */
[[nodiscard]] int process_run_env(const char *const argv[], const process_env_var *env,
                                  size_t env_count);

/* Run `argv` and capture what it writes to stdout into `out`, NUL-terminated
   and truncated to `out_size`. Its stderr is left alone, so a tool Molto
   interrogates can still explain itself to the user while its answer is read
   here. Returns the same codes as process_execute. */
[[nodiscard]] int process_capture(const char *const argv[], char *out, size_t out_size);

/* Run `argv` with `env` exported and capture stdout *and* stderr into `out`.
   A compiler says what it has to say on stderr, so reading only stdout — what
   process_capture does — comes back empty.

   When the output does not fit, the first `out_size - 1` bytes are kept and
   `*truncated` is set (when it is not NULL). The rest is read and discarded
   rather than left unread: closing the pipe early would kill the child with
   SIGPIPE and report a signal death instead of the exit code it was about to
   produce. */
[[nodiscard]] int process_capture_all(const char *const argv[], const process_env_var *env,
                                      size_t env_count, char *out, size_t out_size,
                                      bool *truncated);

/* Build the NULL-terminated argv the exec family wants from a str_list. The
   array is the caller's to free; the strings inside stay owned by `list`.
   Returns NULL on allocation failure. */
[[nodiscard]] const char **process_argv_from_list(const str_list *list);

#endif /* MOLTO_PROCESS_SERVICE_H */
