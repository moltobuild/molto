#include <molto/services/process_service.h>

#include <sys/wait.h>
#include <unistd.h>

int process_run(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* Child: replace the image with the requested command. execvp keeps
           the current stdio, so the command's output is shown verbatim. */
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}
