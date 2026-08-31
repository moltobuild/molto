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

/* The name of the signal a child died from, for the 128 + N codes above.
 *
 * It lives here because the convention does: a caller reporting one has the
 * number this service handed it. `strsignal` is POSIX, and on Windows the
 * question does not arise — nothing there dies from a signal, so the answer is
 * the honest "unknown signal" rather than a table of names for a mechanism the
 * platform does not have. */
[[nodiscard]] const char *process_signal_name(int signal_number);

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

/* --- exchanging a document with a child process (RFC-0014) --- */

/* How an exchange ended. The child's own exit code is a separate field, because
   "the plugin ran and said no" and "the plugin broke" are different facts and a
   single integer cannot carry both. */
typedef enum {
    process_exchange_ok,          /* it exited on its own; `code` says with what */
    process_exchange_not_started, /* fork or exec failed; nothing ran */
    process_exchange_timed_out,   /* it passed its deadline and was killed */
    process_exchange_too_large,   /* its answer passed the cap and was refused mid-read */
    /* The exchange itself broke — a failed poll, memory that ran out mid-read.
       Separate from `not_started` because the child did run, and a caller
       reporting "the plugin could not be started" about a plugin that started
       sends whoever reads it looking in the wrong place. */
    process_exchange_failed,
} process_exchange_result;

/* One exchange: a document in, a document out.
 *
 * Different from every capture above in the one way that matters here. Those
 * merge the child's two streams on purpose, because a compiler diagnoses on
 * stderr and clang-tidy on stdout and a caller reading either wants both. For a
 * plugin the two streams mean different things — stdout is the document and
 * stderr is what the plugin has to say about producing it — so merging them
 * would corrupt the answer with the explanation. */
typedef struct {
    /* in */
    const char *request; /* written to the child's stdin, which is then closed,
                            so a plugin that reads to EOF has the whole request */
    size_t request_size; /* 0 with a non-NULL request means strlen(request) */
    size_t answer_max;   /* refuse an answer larger than this; 0 means no cap */
    unsigned timeout_ms; /* 0 waits as long as it takes */
    const process_env_var *env;
    size_t env_count;

    /* out */
    char *answer; /* heap, NUL-terminated; the caller frees it. NULL when the
                     child wrote nothing */
    size_t answer_size;
    int code; /* the child's exit code, or 128 + N when a signal ended it */
} process_exchange;

/* Run `argv`, write `io->request` to its standard input, read its standard
   output into `io->answer`, and leave its standard error inherited so what it
   says reaches the user directly.
 *
 * Both directions are driven at once rather than one after the other. Writing
 * the whole request first would deadlock the moment a child answers before it
 * has finished reading: the parent blocks writing into a full pipe while the
 * child blocks writing into one nobody is draining. Neither side is at fault
 * and neither can recover, so the parent polls both.
 *
 * A child that exits before reading its request does not kill Molto: SIGPIPE is
 * ignored for the duration and the failed write is simply the end of the
 * request. A child that never finishes is killed at `timeout_ms` — a plugin
 * that hangs must not hang a build.
 *
 * `io->answer` is set on every result that produced bytes, including a timeout,
 * so a caller can report what it got. It is the caller's decision what a
 * partial answer means, and for an IR document the answer is nothing: a
 * document half-read is valid JSON prefix and invalid meaning. */
[[nodiscard]] process_exchange_result process_exchange_run(const char *const argv[],
                                                           process_exchange *io);

/* Build the NULL-terminated argv the exec family wants from a str_list. The
   array is the caller's to free; the strings inside stay owned by `list`.
   Returns NULL on allocation failure. */
[[nodiscard]] const char **process_argv_from_list(const str_list *list);

#endif /* MOLTO_PROCESS_SERVICE_H */
