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
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return SIGNAL_EXIT_BASE + WTERMSIG(status);
    return -1;
}
