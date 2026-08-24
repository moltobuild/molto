/* For pipe2, which glibc declares only under this macro. It has to be defined
   before the first header, and it is scoped to this file rather than to the
   build so nothing else changes which declarations it can see. The name is the
   implementation's to give, which is why the reserved-identifier check is
   answered on the line rather than obeyed: renamed, this compiles without
   pipe2. */
#define _GNU_SOURCE /* NOLINT(bugprone-reserved-identifier) */

#include <molto/services/process_service.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
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
    /* An exchange also feeds the child: the read end becomes its standard
       input, and the write end is the parent's and closed here. -1 in every
       other caller, because 0 is a descriptor and would read as "wire fd 0",
       which is the one mistake this field could make. */
    int stdin_read_end;
    int stdin_write_end;
} child_pipe;

/* A pipe whose ends are closed by exec.
 *
 * Without that flag, a pipe opened on one worker thread is inherited by every
 * child another worker forks in the meantime, and a pipe only reaches EOF once
 * the last copy of its write end is closed. The capture waiting on that EOF
 * then sits there for as long as the slowest unrelated compiler runs. It is
 * not a deadlock — the compilers do finish — but it is a build that serialises
 * itself under `-j`, and the more cores there are the worse it reads.
 *
 * pipe2 sets the flag as part of the same call, so there is no window between
 * opening the pipe and protecting it. The fallback narrows that window to two
 * fcntl calls rather than closing it, which is what a platform without pipe2
 * can have. */
static bool open_pipe(int fds[2]) {
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if(pipe(fds) != 0)
        return false;
    (void)fcntl(fds[PIPE_READ], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[PIPE_WRITE], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

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
    if(wiring->stdin_write_end >= 0)
        close(wiring->stdin_write_end);
    if(wiring->stdin_read_end >= 0) {
        dup2(wiring->stdin_read_end, STDIN_FILENO);
        close(wiring->stdin_read_end);
    }
    /* dup2 clears close-on-exec on the descriptor it creates, so the streams
       wired here survive the exec even though the pipe they came from does
       not. That is the whole arrangement: the child keeps what it was given
       and drops what it merely inherited. */
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
    if(capturing && !open_pipe(output))
        return -1;

    const child_pipe wiring = {
        .write_end = capturing ? output[PIPE_WRITE] : -1,
        .read_end = capturing ? output[PIPE_READ] : -1,
        .stdout_captured = spec->stdout_to == process_stream_capture,
        .stderr_captured = spec->stderr_to == process_stream_capture,
        .stdin_read_end = -1,
        .stdin_write_end = -1,
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
    const char **argv = (const char **)malloc((count + 1) * sizeof *argv);
    if(argv == NULL)
        return NULL;
    for(size_t i = 0; i < count; i++)
        argv[i] = str_list_get(list, i);
    argv[count] = NULL;
    return argv;
}

/* --- exchanging a document with a child process --- */

/* How much is read in one go, and how much the answer buffer grows by. */
#define EXCHANGE_CHUNK 65536

/* Milliseconds on the monotonic clock, which is what a deadline has to be
   measured against: the wall clock can step backwards and a build would then
   wait for a plugin twice. */
static long long monotonic_ms(void) {
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Append `length` bytes to a growable buffer, keeping it NUL-terminated. */
static bool answer_append(char **answer, size_t *size, size_t *capacity, const char *text,
                          size_t length) {
    if(*size + length + 1 > *capacity) {
        size_t next = *capacity == 0 ? EXCHANGE_CHUNK : *capacity;
        while(next < *size + length + 1)
            next *= 2;
        char *bigger = realloc(*answer, next);
        if(bigger == NULL)
            return false;
        *answer = bigger;
        *capacity = next;
    }
    memcpy(*answer + *size, text, length);
    *size += length;
    (*answer)[*size] = '\0';
    return true;
}

/* Kill a child and reap it, so a timeout does not leave a process behind. */
static void kill_child(pid_t pid) {
    kill(pid, SIGKILL);
    int status = 0;
    (void)waitpid(pid, &status, 0);
}

typedef struct {
    int to_child;   /* -1 once the request is written and the pipe closed */
    int from_child; /* -1 once the answer reached EOF */
    const char *request;
    size_t remaining;
} exchange_state;

/* One turn of the loop: whichever end is ready, moved by one chunk.
   Returns false when the exchange should stop, with `*result` saying why. */
static bool exchange_step(exchange_state *state, process_exchange *io, size_t *capacity,
                          process_exchange_result *result) {
    struct pollfd watched[2];
    int count = 0;
    if(state->to_child >= 0)
        watched[count++] = (struct pollfd){.fd = state->to_child, .events = POLLOUT};
    if(state->from_child >= 0)
        watched[count++] = (struct pollfd){.fd = state->from_child, .events = POLLIN};
    if(count == 0)
        return false;

    if(poll(watched, (nfds_t)count, 50) < 0 && errno != EINTR) {
        *result = process_exchange_failed;
        return false;
    }

    for(int i = 0; i < count; i++) {
        if(watched[i].revents == 0)
            continue;

        if(watched[i].fd == state->to_child) {
            const ssize_t wrote = write(state->to_child, state->request, state->remaining);
            if(wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                continue; /* the pipe filled between the poll and the write */
            if(wrote <= 0) {
                /* A child that stopped reading has all the request it wants.
                   With SIGPIPE ignored this is a failed write and not a death. */
                close(state->to_child);
                state->to_child = -1;
                continue;
            }
            state->request += wrote;
            state->remaining -= (size_t)wrote;
            if(state->remaining == 0) {
                /* Closed rather than left open, because a plugin reading to EOF
                   is waiting for exactly this. */
                close(state->to_child);
                state->to_child = -1;
            }
            continue;
        }

        char chunk[EXCHANGE_CHUNK];
        const ssize_t got = read(state->from_child, chunk, sizeof chunk);
        if(got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue; /* drained between the poll and the read */
        if(got <= 0) {
            close(state->from_child);
            state->from_child = -1;
            continue;
        }
        if(io->answer_max > 0 && io->answer_size + (size_t)got > io->answer_max) {
            *result = process_exchange_too_large;
            return false;
        }
        if(!answer_append(&io->answer, &io->answer_size, capacity, chunk, (size_t)got)) {
            *result = process_exchange_failed;
            return false;
        }
    }
    return true;
}

process_exchange_result process_exchange_run(const char *const argv[], process_exchange *io) {
    if(io == NULL || argv == NULL || argv[0] == NULL)
        return process_exchange_not_started;

    io->answer = NULL;
    io->answer_size = 0;
    io->code = -1;

    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    if(!open_pipe(to_child))
        return process_exchange_not_started;
    if(!open_pipe(from_child)) {
        close(to_child[PIPE_READ]);
        close(to_child[PIPE_WRITE]);
        return process_exchange_not_started;
    }

    const child_pipe wiring = {
        .write_end = from_child[PIPE_WRITE],
        .read_end = from_child[PIPE_READ],
        .stdout_captured = true,
        /* Left inherited: stderr is what the plugin has to say, and it belongs
           on the terminal beside everything else a build prints. */
        .stderr_captured = false,
        .stdin_read_end = to_child[PIPE_READ],
        .stdin_write_end = to_child[PIPE_WRITE],
    };

    /* A child that exits without reading turns the parent's next write into
       SIGPIPE, which would end molto rather than the exchange. Ignored for the
       duration and restored after, so nothing else inherits the change. */
    void (*previous_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);

    const pid_t pid = spawn_child(argv, io->env, io->env_count, &wiring);
    close(to_child[PIPE_READ]);
    close(from_child[PIPE_WRITE]);
    if(pid < 0) {
        close(to_child[PIPE_WRITE]);
        close(from_child[PIPE_READ]);
        (void)signal(SIGPIPE, previous_sigpipe);
        return process_exchange_not_started;
    }

    /* The parent's ends are non-blocking, and this is what makes the poll loop
       above actually work rather than merely look like it does. On a blocking
       pipe, write() with more than PIPE_BUF bytes does not return until every
       byte is written — so the parent would sit inside one write() while the
       child filled the pipe coming back, which is precisely the deadlock the
       loop exists to avoid. Non-blocking turns that into a partial write the
       loop can interleave with a read. */
    (void)fcntl(to_child[PIPE_WRITE], F_SETFL, O_NONBLOCK);
    (void)fcntl(from_child[PIPE_READ], F_SETFL, O_NONBLOCK);

    exchange_state state = {
        .to_child = to_child[PIPE_WRITE],
        .from_child = from_child[PIPE_READ],
        .request = io->request,
        .remaining = io->request == NULL
                         ? 0
                         : (io->request_size > 0 ? io->request_size : strlen(io->request)),
    };
    /* Nothing to send is a closed stdin, not an open one: a frontend asked for
       nothing still reads to EOF. */
    if(state.remaining == 0) {
        close(state.to_child);
        state.to_child = -1;
    }

    const long long deadline = io->timeout_ms == 0 ? 0 : monotonic_ms() + io->timeout_ms;
    process_exchange_result result = process_exchange_ok;
    size_t capacity = 0;

    while(state.to_child >= 0 || state.from_child >= 0) {
        if(!exchange_step(&state, io, &capacity, &result))
            break;
        if(deadline != 0 && monotonic_ms() >= deadline) {
            result = process_exchange_timed_out;
            break;
        }
    }

    if(state.to_child >= 0)
        close(state.to_child);
    if(state.from_child >= 0)
        close(state.from_child);
    (void)signal(SIGPIPE, previous_sigpipe);

    if(result != process_exchange_ok) {
        kill_child(pid);
        return result;
    }

    int status = 0;
    if(waitpid(pid, &status, 0) < 0)
        return process_exchange_failed;
    io->code = status_to_code(status);
    /* An exec that failed reports as a shell does, and that is not a plugin
       that ran and answered — it is one that never started. */
    return io->code == EXIT_COMMAND_NOT_RUNNABLE ? process_exchange_not_started
                                                 : process_exchange_ok;
}
