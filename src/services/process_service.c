#include <molto/services/process_service.h>

#include <sys/wait.h>
#include <unistd.h>

/* Offset used to report a signal death as a return code, following the usual
   shell convention: a child killed by signal N reports 128 + N. */
#define SIGNAL_EXIT_BASE 128

int process_run(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* Child: replace the image with the requested command. execvp keeps
           the current stdio, so the command's output is shown verbatim. If it
           fails (e.g. command not found), exit 127 like a shell would. */
        execvp(argv[0], (char *const *)argv);
        _exit(127);
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
