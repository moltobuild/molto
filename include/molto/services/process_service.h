#ifndef MOLTO_PROCESS_SERVICE_H
#define MOLTO_PROCESS_SERVICE_H

/* Run a command described by a NULL-terminated `argv`, inheriting stdio so
   the child's output (e.g. compiler diagnostics) is shown verbatim.
   Returns:
     - the child's exit code (0-255) if it exited normally;
     - 128 + N if it was killed by signal N (shell convention);
     - -1 if the process could not be started (fork/wait failed). */
[[nodiscard]] int process_run(const char *const argv[]);

#endif /* MOLTO_PROCESS_SERVICE_H */
