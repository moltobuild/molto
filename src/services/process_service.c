#include <molto/services/process_service.h>

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* Offset used to report a signal death as a return code, following the usual
   shell convention: a child killed by signal N reports 128 + N. */
#define SIGNAL_EXIT_BASE 128

/* Exit code a shell reports when a command cannot be executed. */
#define EXIT_COMMAND_NOT_RUNNABLE 127

/* Ends of a pipe, named so the code reads as data flow rather than indices. */
#define PIPE_READ 0
#define PIPE_WRITE 1

/* Translate a wait status into the code every function here returns. */
static int status_to_code(int status) {
    if(WIFEXITED(status))
        return WEXITSTATUS(status);
    if(WIFSIGNALED(status))
        return SIGNAL_EXIT_BASE + WTERMSIG(status);
    return -1;
}

/* One pipe, and which of the child's streams are wired to it. */
typedef struct {
    int write_end; /* -1 when nothing is captured */
    int read_end;  /* closed in the child; -1 when nothing is captured */
    bool stdout_captured;
    bool stderr_captured;
} child_pipe;

/* Fork and exec `argv`, wiring the child's streams as `wiring` says; a stream
   that is not captured stays inherited. Returns the pid, or -1.
   The whole service differs only in how it answers this one question. */
static pid_t spawn_child(const char *const argv[], const process_env_var *env, size_t env_count,
                         const child_pipe *wiring) {
    pid_t pid = fork();
    if(pid != 0)
        return pid; /* the parent, or -1 */

    if(wiring->read_end >= 0)
        close(wiring->read_end);
    if(wiring->stdout_captured)
        dup2(wiring->write_end, STDOUT_FILENO);
    if(wiring->stderr_captured)
        dup2(wiring->write_end, STDERR_FILENO);
    /* The original descriptor is now a duplicate of whatever it was wired to;
       leaving it open would hold the pipe from being seen as closed at EOF. */
    if(wiring->write_end >= 0)
        close(wiring->write_end);

    /* Export the project's variables before handing over the image. Doing it
       here rather than in the parent keeps Molto's own environment untouched,
       so one project's [env] cannot leak into anything else. */
    for(size_t i = 0; i < env_count; i++) {
        if(setenv(env[i].name, env[i].value, 1) != 0)
            _exit(EXIT_COMMAND_NOT_RUNNABLE);
    }
    /* If execvp fails (e.g. command not found), exit like a shell would. */
    execvp(argv[0], (char *const *)argv);
    _exit(EXIT_COMMAND_NOT_RUNNABLE);
}

/* Read `fd` to EOF, keeping what fits in `out`. Reading past the buffer and
   discarding is deliberate: closing early would kill the child with SIGPIPE
   and report a signal death instead of the exit code it was about to give. */
static void drain(int fd, char *out, size_t out_size, bool *truncated) {
    size_t total = 0;
    for(;;) {
        char discard[4096];
        bool room = total + 1 < out_size;
        char *into = room ? out + total : discard;
        size_t want = room ? out_size - total - 1 : sizeof discard;

        ssize_t got = read(fd, into, want);
        if(got <= 0)
            break;
        if(room)
            total += (size_t)got;
        else if(truncated != NULL)
            *truncated = true;
    }
    out[total] = '\0';
}

int process_execute(const char *const argv[], process_spec *spec) {
    if(spec == NULL || argv == NULL || argv[0] == NULL)
        return -1;

    bool capturing =
        spec->stdout_to == process_stream_capture || spec->stderr_to == process_stream_capture;
    if(capturing && (spec->capture == NULL || spec->capture_size == 0))
        return -1;
    if(capturing)
        spec->capture[0] = '\0';

    int output[2] = {-1, -1};
    if(capturing && pipe(output) != 0)
        return -1;

    const child_pipe wiring = {
        .write_end = capturing ? output[PIPE_WRITE] : -1,
        .read_end = capturing ? output[PIPE_READ] : -1,
        .stdout_captured = spec->stdout_to == process_stream_capture,
        .stderr_captured = spec->stderr_to == process_stream_capture,
    };
    pid_t pid = spawn_child(argv, spec->env, spec->env_count, &wiring);
    if(pid < 0) {
        if(capturing) {
            close(output[PIPE_READ]);
            close(output[PIPE_WRITE]);
        }
        return -1;
    }

    if(capturing) {
        close(output[PIPE_WRITE]);
        /* Drain before waiting: a child that fills the pipe would block forever
           if we waited on it first. */
        drain(output[PIPE_READ], spec->capture, spec->capture_size, &spec->truncated);
        close(output[PIPE_READ]);
    }

    int status = 0;
    if(waitpid(pid, &status, 0) < 0)
        return -1;
    return status_to_code(status);
}

int process_run(const char *const argv[]) { return process_run_env(argv, NULL, 0); }

int process_run_env(const char *const argv[], const process_env_var *env, size_t env_count) {
    process_spec spec = {
        .env = env,
        .env_count = env_count,
        .stdout_to = process_stream_inherit,
        .stderr_to = process_stream_inherit,
    };
    return process_execute(argv, &spec);
}

int process_capture(const char *const argv[], char *out, size_t out_size) {
    process_spec spec = {
        .stdout_to = process_stream_capture,
        /* stderr is deliberately inherited: what the child writes there is a
           message for the user, not part of the answer being read. */
        .stderr_to = process_stream_inherit,
        .capture = out,
        .capture_size = out_size,
    };
    return process_execute(argv, &spec);
}

int process_capture_all(const char *const argv[], const process_env_var *env, size_t env_count,
                        char *out, size_t out_size, bool *truncated) {
    process_spec spec = {
        .env = env,
        .env_count = env_count,
        .stdout_to = process_stream_capture,
        .stderr_to = process_stream_capture,
        .capture = out,
        .capture_size = out_size,
    };
    int code = process_execute(argv, &spec);
    if(truncated != NULL)
        *truncated = spec.truncated;
    return code;
}

const char **process_argv_from_list(const str_list *list) {
    size_t count = str_list_count(list);
    const char **argv = malloc((count + 1) * sizeof *argv);
    if(argv == NULL)
        return NULL;
    for(size_t i = 0; i < count; i++)
        argv[i] = str_list_get(list, i);
    argv[count] = NULL;
    return argv;
}
