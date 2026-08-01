#include <molto/services/process_service.h>

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* Offset used to report a signal death as a return code, following the usual
   shell convention: a child killed by signal N reports 128 + N. */
#define SIGNAL_EXIT_BASE 128

/* Exit code a shell reports when a command cannot be executed. */
#define EXIT_COMMAND_NOT_RUNNABLE 127

int process_run(const char *const argv[]) {
    return process_run_env(argv, NULL, 0);
}

/* Translate a wait status into the code every function here returns. */
static int status_to_code(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return SIGNAL_EXIT_BASE + WTERMSIG(status);
    return -1;
}

int process_run_env(const char *const argv[],
                    const process_env_var *env, size_t env_count) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* Child: export the project's variables before handing over the image.
           Doing it here rather than in the parent keeps Molto's own environment
           untouched, so one project's [env] cannot leak into anything else. */
        for (size_t i = 0; i < env_count; i++) {
            if (setenv(env[i].name, env[i].value, 1) != 0)
                _exit(EXIT_COMMAND_NOT_RUNNABLE);
        }
        /* execvp keeps the current stdio, so the command's output is shown
           verbatim. If it fails (e.g. command not found), exit like a shell. */
        execvp(argv[0], (char *const *)argv);
        _exit(EXIT_COMMAND_NOT_RUNNABLE);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return status_to_code(status);
}

/* Ends of a pipe, named so the code reads as data flow rather than indices. */
#define PIPE_READ  0
#define PIPE_WRITE 1

int process_capture(const char *const argv[], char *out, size_t out_size) {
    if (out == NULL || out_size == 0)
        return -1;
    out[0] = '\0';

    int output[2];
    if (pipe(output) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(output[PIPE_READ]);
        close(output[PIPE_WRITE]);
        return -1;
    }
    if (pid == 0) {
        close(output[PIPE_READ]);
        dup2(output[PIPE_WRITE], STDOUT_FILENO);
        close(output[PIPE_WRITE]);
        /* stderr is deliberately inherited: what the child writes there is a
           message for the user, not part of the answer being read. */
        execvp(argv[0], (char *const *)argv);
        _exit(EXIT_COMMAND_NOT_RUNNABLE);
    }

    close(output[PIPE_WRITE]);
    /* Drain before waiting: a child that fills the pipe would block forever if
       we waited on it first. */
    size_t total = 0;
    while (total + 1 < out_size) {
        ssize_t got = read(output[PIPE_READ], out + total, out_size - total - 1);
        if (got <= 0)
            break;
        total += (size_t)got;
    }
    out[total] = '\0';
    close(output[PIPE_READ]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return status_to_code(status);
}
