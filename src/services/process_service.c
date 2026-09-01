#ifdef _WIN32

#include <molto/services/process_service.h>
#include <molto/util/thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#else

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
#include <unistd.h>

#endif

#include <molto/util/clock.h>

/* Offset used to report a signal death as a return code, following the usual
   shell convention: a child killed by signal N reports 128 + N. */
#define SIGNAL_EXIT_BASE 128

/* Exit code a shell reports when a command cannot be executed. */
#define EXIT_COMMAND_NOT_RUNNABLE 127

/* Ends of a pipe, named so the code reads as data flow rather than indices. */
#define PIPE_READ 0
#define PIPE_WRITE 1

/* How much is read in one go, and how much the answer buffer grows by. */
#define EXCHANGE_CHUNK 65536

/*
 * ====================================================================
 * The platform
 * ====================================================================
 *
 * Everything that differs between systems is in this section and nowhere
 * else, which is what RFC-0017 asks of a service. Below it there is one
 * implementation of what Molto actually does with a process, and it never
 * asks which system it is on.
 *
 * The two systems disagree about more than spelling here. POSIX makes a
 * child by copying this process and then replacing it, so a caller can set
 * things up in between — that window is where the environment is exported
 * and the streams are wired. Windows has no such window: `CreateProcess`
 * takes the whole arrangement up front, as a command line, an environment
 * block and a set of handles. The shape below is the smaller of the two:
 * open a pipe, start a child with these ends, read, write, wait.
 */

/* One end of a pipe, and the one value that means there is none. */
#ifdef _WIN32
typedef HANDLE pipe_end;
typedef HANDLE child_handle;
#define PIPE_NONE NULL
#define CHILD_NONE NULL
#else
typedef int pipe_end;
typedef pid_t child_handle;
#define PIPE_NONE (-1)
#define CHILD_NONE (-1)
#endif

static bool end_open(pipe_end end) {
#ifdef _WIN32
    return end != NULL && end != INVALID_HANDLE_VALUE;
#else
    return end >= 0;
#endif
}

static void end_close(pipe_end *end) {
    if(!end_open(*end))
        return;
#ifdef _WIN32
    (void)CloseHandle(*end);
#else
    (void)close(*end);
#endif
    *end = PIPE_NONE;
}

/* Bytes read, 0 at end of stream, -1 on an error worth stopping for. */
static long end_read(pipe_end end, char *into, size_t want) {
#ifdef _WIN32
    DWORD got = 0;
    if(ReadFile(end, into, (DWORD)want, &got, NULL) == 0) {
        /* The child closed its end. That is the end of the stream here, the
           same fact POSIX reports as a read of zero. */
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    }
    return (long)got;
#else
    const ssize_t got = read(end, into, want);
    if(got < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1;
    return (long)got;
#endif
}

/* Bytes written, or -1 when the far end is gone. */
static long end_write(pipe_end end, const char *from, size_t count) {
#ifdef _WIN32
    DWORD wrote = 0;
    if(WriteFile(end, from, (DWORD)count, &wrote, NULL) == 0)
        return -1;
    return (long)wrote;
#else
    const ssize_t wrote = write(end, from, count);
    if(wrote < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1;
    return (long)wrote;
#endif
}

/* The platform's bin for output nobody reads, opened so a child inherits it.
 *
 * Both systems have one and neither calls it the same thing. Wired through the
 * same field a capture's pipe uses, so `spawn_child` needs to know nothing
 * about it: to that code it is a write end like any other. */
static pipe_end open_null_sink(void);

/* One pipe, and which of the child's streams are wired to it. */
typedef struct {
    pipe_end write_end; /* PIPE_NONE when nothing is captured */
    pipe_end read_end;  /* closed in the child; PIPE_NONE when nothing is captured */
    bool stdout_captured;
    bool stderr_captured;
    /* An exchange also feeds the child: the read end becomes its standard
       input, and the write end is the parent's and closed here. PIPE_NONE in
       every other caller, because 0 is a descriptor and would read as "wire
       fd 0", which is the one mistake this field could make. */
    pipe_end stdin_read_end;
    pipe_end stdin_write_end;
} child_pipe;

#ifdef _WIN32

/* --- Windows --- */

/*
 * A pipe whose ends the child does not inherit unless it is given them.
 *
 * Windows inherits by handle rather than by descriptor, and `bInheritHandle`
 * is what decides. Both ends are created inheritable and the parent's own end
 * is then made private again — the same reasoning as close-on-exec below, and
 * for the same failure: a read end left inheritable is duplicated into every
 * unrelated child, and a pipe reaches end of stream only when the last copy of
 * its write end is gone.
 */
static bool open_pipe(pipe_end fds[2]) {
    SECURITY_ATTRIBUTES inheritable = {
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .bInheritHandle = TRUE,
        .lpSecurityDescriptor = NULL,
    };
    return CreatePipe(&fds[PIPE_READ], &fds[PIPE_WRITE], &inheritable, 0) != 0;
}

/* Take one end out of the set a child inherits. */
static void end_keep_private(pipe_end end) {
    if(end_open(end))
        (void)SetHandleInformation(end, HANDLE_FLAG_INHERIT, 0);
}

static bool put(char *out, size_t size, size_t *at, char c) {
    if(*at + 1 >= size)
        return false;
    out[(*at)++] = c;
    return true;
}

/*
 * Windows takes a command line, not an argv, and the child pulls the argv back
 * out of it. Quoting has to follow the rule the C runtime parses by, exactly:
 * a backslash is literal unless it runs into a quote, and a run of them before
 * a quote is halved.
 *
 * Getting this wrong does not fail loudly. It hands a compiler a path with one
 * backslash too few, and the error comes back from the compiler about a file
 * that nearly exists.
 */
static bool quote_arg(char *out, size_t size, size_t *at, const char *arg) {
    const bool bare = arg[0] != '\0' && strpbrk(arg, " \t\n\v\"") == NULL;
    if(bare) {
        for(const char *c = arg; *c != '\0'; c++) {
            if(!put(out, size, at, *c))
                return false;
        }
        return true;
    }

    if(!put(out, size, at, '"'))
        return false;
    for(const char *c = arg;; c++) {
        size_t slashes = 0;
        while(*c == '\\') {
            slashes++;
            c++;
        }
        if(*c == '\0') {
            /* Doubled: they are about to sit before the closing quote, and a
               lone backslash there would escape it. */
            for(size_t i = 0; i < slashes * 2; i++) {
                if(!put(out, size, at, '\\'))
                    return false;
            }
            break;
        }
        const size_t emit = *c == '"' ? slashes * 2 + 1 : slashes;
        for(size_t i = 0; i < emit; i++) {
            if(!put(out, size, at, '\\'))
                return false;
        }
        if(!put(out, size, at, *c))
            return false;
    }
    return put(out, size, at, '"');
}

/* The documented ceiling for a command line. */
#define COMMAND_LINE_MAX 32768

static bool build_command_line(const char *const argv[], char *out, size_t size) {
    size_t at = 0;
    for(size_t i = 0; argv[i] != NULL; i++) {
        if(i > 0 && !put(out, size, &at, ' '))
            return false;
        if(!quote_arg(out, size, &at, argv[i]))
            return false;
    }
    out[at] = '\0';
    return true;
}

/*
 * The child's environment, which is this process's with the caller's variables
 * laid over it.
 *
 * POSIX exports them between the fork and the exec, so Molto's own environment
 * is never touched. There is no such moment here, and the alternative —
 * `SetEnvironmentVariable` before `CreateProcess` — would change this process
 * for real, which under `-j` means one worker's variables reaching another
 * worker's compiler. Composing a block keeps the promise the header makes.
 *
 * The block is NAME=VALUE strings, each NUL-terminated, the lot ended by an
 * empty one. Overridden names are dropped from the inherited half rather than
 * appended twice, because a block holding a name twice is undefined and the
 * one that wins is not the one written last.
 */
static bool env_name_is(const char *entry, const char *name) {
    const size_t length = strlen(name);
    return _strnicmp(entry, name, length) == 0 && entry[length] == '=';
}

static bool env_overridden(const char *entry, const process_env_var *env, size_t env_count) {
    for(size_t i = 0; i < env_count; i++) {
        if(env_name_is(entry, env[i].name))
            return true;
    }
    return false;
}

static char *build_environment(const process_env_var *env, size_t env_count) {
    if(env == NULL || env_count == 0)
        return NULL; /* inherit this process's, unchanged */

    char *inherited = GetEnvironmentStringsA();
    if(inherited == NULL)
        return NULL;

    size_t size = 1; /* the empty string that ends the block */
    for(const char *entry = inherited; *entry != '\0'; entry += strlen(entry) + 1) {
        if(!env_overridden(entry, env, env_count))
            size += strlen(entry) + 1;
    }
    for(size_t i = 0; i < env_count; i++)
        size += strlen(env[i].name) + strlen(env[i].value) + 2;

    char *block = malloc(size);
    if(block == NULL) {
        (void)FreeEnvironmentStringsA(inherited);
        return NULL;
    }

    size_t at = 0;
    for(const char *entry = inherited; *entry != '\0'; entry += strlen(entry) + 1) {
        if(env_overridden(entry, env, env_count))
            continue;
        const size_t length = strlen(entry) + 1;
        memcpy(block + at, entry, length);
        at += length;
    }
    for(size_t i = 0; i < env_count; i++) {
        const int written = snprintf(block + at, size - at, "%s=%s", env[i].name, env[i].value);
        if(written < 0 || (size_t)written >= size - at) {
            free(block);
            (void)FreeEnvironmentStringsA(inherited);
            return NULL;
        }
        at += (size_t)written + 1;
    }
    block[at] = '\0';

    (void)FreeEnvironmentStringsA(inherited);
    return block;
}

static child_handle spawn_child(const char *const argv[], const process_env_var *env,
                                size_t env_count, const child_pipe *wiring) {
    char command[COMMAND_LINE_MAX];
    if(!build_command_line(argv, command, sizeof command))
        return CHILD_NONE;

    char *environment = build_environment(env, env_count);
    if(env_count > 0 && environment == NULL)
        return CHILD_NONE;

    /* Nothing to send is an empty stdin, not the parent's — the same rule the
       POSIX half states, and for the same reason: a tool that reads standard
       input when nobody is writing blocks for ever and takes the build with
       it. `NUL` is what /dev/null is called here. */
    HANDLE nothing = INVALID_HANDLE_VALUE;
    if(!end_open(wiring->stdin_read_end)) {
        SECURITY_ATTRIBUTES inheritable = {
            .nLength = sizeof inheritable,
            .bInheritHandle = TRUE,
        };
        nothing = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                              OPEN_EXISTING, 0, NULL);
    }

    STARTUPINFOA startup = {.cb = sizeof(STARTUPINFOA)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = end_open(wiring->stdin_read_end)  ? wiring->stdin_read_end
                        : nothing != INVALID_HANDLE_VALUE ? nothing
                                                          : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput =
        wiring->stdout_captured ? wiring->write_end : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError =
        wiring->stderr_captured ? wiring->write_end : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION started = {0};
    const BOOL ok =
        CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, environment, NULL, &startup, &started);
    free(environment);
    if(nothing != INVALID_HANDLE_VALUE)
        CloseHandle(nothing); /* the child holds its own copy from here */
    if(!ok)
        return CHILD_NONE;

    /* The thread handle is of no use to anyone here, and leaving it open holds
       the child's kernel object alive after it exits. */
    (void)CloseHandle(started.hThread);
    return started.hProcess;
}

static pipe_end open_null_sink(void) {
    SECURITY_ATTRIBUTES inheritable = {
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .bInheritHandle = TRUE,
        .lpSecurityDescriptor = NULL,
    };
    HANDLE sink = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &inheritable, OPEN_EXISTING, 0, NULL);
    return sink == INVALID_HANDLE_VALUE ? PIPE_NONE : sink;
}

static bool child_started(child_handle child) { return child != CHILD_NONE; }

static int child_wait(child_handle child) {
    if(WaitForSingleObject(child, INFINITE) != WAIT_OBJECT_0) {
        (void)CloseHandle(child);
        return -1;
    }
    DWORD code = 0;
    const BOOL got = GetExitCodeProcess(child, &code);
    (void)CloseHandle(child);
    return got ? (int)code : -1;
}

static void child_kill(child_handle child) {
    (void)TerminateProcess(child, 1);
    (void)WaitForSingleObject(child, INFINITE);
    (void)CloseHandle(child);
}

#else

/* --- POSIX --- */

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
static bool open_pipe(pipe_end fds[2]) {
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

/* Nothing to do: open_pipe already asked for close-on-exec, which is how a
   descriptor is kept out of a child here. */
static void end_keep_private(pipe_end end) { (void)end; }

/* Fork and exec `argv`, wiring the child's streams as `wiring` says; a stream
   that is not captured stays inherited. Returns the pid, or -1. */
static child_handle spawn_child(const char *const argv[], const process_env_var *env,
                                size_t env_count, const child_pipe *wiring) {
    pid_t pid = fork();
    if(pid != 0)
        return pid; /* the parent, or -1 */

    if(end_open(wiring->read_end))
        close(wiring->read_end);
    if(end_open(wiring->stdin_write_end))
        close(wiring->stdin_write_end);
    if(end_open(wiring->stdin_read_end)) {
        dup2(wiring->stdin_read_end, STDIN_FILENO);
        close(wiring->stdin_read_end);
    } else {
        /* Nothing to send is an empty stdin, not the parent's.
           Inheriting it means a tool that reads standard input blocks on a
           stream nobody is writing, and takes the whole build down with it --
           `pkg-config` asked about a package is not supposed to read anything,
           but "not supposed to" is not a guarantee, and a build that hangs
           gives the person running it nothing to go on. /dev/null answers EOF
           immediately, which is what a program handed no input should see. */
        const int nothing = open("/dev/null", O_RDONLY);
        if(nothing >= 0) {
            dup2(nothing, STDIN_FILENO);
            close(nothing);
        }
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
    if(end_open(wiring->write_end))
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

static pipe_end open_null_sink(void) { return open("/dev/null", O_WRONLY); }

static bool child_started(child_handle child) { return child >= 0; }

/* Translate a wait status into the code every function here returns. */
static int status_to_code(int status) {
    if(WIFEXITED(status))
        return WEXITSTATUS(status);
    if(WIFSIGNALED(status))
        return SIGNAL_EXIT_BASE + WTERMSIG(status);
    return -1;
}

static int child_wait(child_handle child) {
    int status = 0;
    if(waitpid(child, &status, 0) < 0)
        return -1;
    return status_to_code(status);
}

/* Kill a child and reap it, so a timeout does not leave a process behind. */
static void child_kill(child_handle child) {
    kill(child, SIGKILL);
    int status = 0;
    (void)waitpid(child, &status, 0);
}

#endif

/*
 * ====================================================================
 * What Molto does with a process
 * ====================================================================
 */

/* Read `end` to EOF, keeping what fits in `out`. Reading past the buffer and
   discarding is deliberate: closing early would kill the child with SIGPIPE
   and report a signal death instead of the exit code it was about to give. */
static void drain(pipe_end end, char *out, size_t out_size, bool *truncated) {
    size_t total = 0;
    for(;;) {
        char discard[4096];
        bool room = total + 1 < out_size;
        char *into = room ? out + total : discard;
        size_t want = room ? out_size - total - 1 : sizeof discard;

        long got = end_read(end, into, want);
        if(got == -2)
            continue; /* nothing there yet; the end is not closed */
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

    pipe_end output[2] = {PIPE_NONE, PIPE_NONE};
    if(capturing && !open_pipe(output))
        return -1;
    if(capturing)
        end_keep_private(output[PIPE_READ]);

    const child_pipe wiring = {
        .write_end = capturing ? output[PIPE_WRITE] : PIPE_NONE,
        .read_end = capturing ? output[PIPE_READ] : PIPE_NONE,
        .stdout_captured = spec->stdout_to == process_stream_capture,
        .stderr_captured = spec->stderr_to == process_stream_capture,
        .stdin_read_end = PIPE_NONE,
        .stdin_write_end = PIPE_NONE,
    };
    child_handle child = spawn_child(argv, spec->env, spec->env_count, &wiring);
    if(!child_started(child)) {
        end_close(&output[PIPE_READ]);
        end_close(&output[PIPE_WRITE]);
        return -1;
    }

    if(capturing) {
        /* The parent's copy of the write end goes first, or the read below
           never sees the end of the stream: the pipe is open as long as any
           copy of its write end is. */
        end_close(&output[PIPE_WRITE]);
        /* Drain before waiting: a child that fills the pipe would block forever
           if we waited on it first. */
        drain(output[PIPE_READ], spec->capture, spec->capture_size, &spec->truncated);
        end_close(&output[PIPE_READ]);
    }

    return child_wait(child);
}

const char *process_signal_name(int signal_number) {
#ifdef _WIN32
    (void)signal_number;
    return "unknown signal";
#else
    const char *name = strsignal(signal_number);
    return name != NULL ? name : "unknown signal";
#endif
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

bool process_start(const char *const argv[], process_handle *out) {
    if(out == NULL || argv == NULL || argv[0] == NULL)
        return false;
    out->running = false;

    pipe_end sink = open_null_sink();
    if(!end_open(sink))
        return false;

    const child_pipe wiring = {
        .write_end = sink,
        .read_end = PIPE_NONE,
        .stdout_captured = true,
        .stderr_captured = true,
        .stdin_read_end = PIPE_NONE,
        .stdin_write_end = PIPE_NONE,
    };
    const child_handle child = spawn_child(argv, NULL, 0, &wiring);
    /* The parent's copy goes either way: the child has its own now, and
       holding this one open keeps a handle on the null device for no reason. */
    end_close(&sink);
    if(!child_started(child))
        return false;

#ifdef _WIN32
    out->process = child;
#else
    out->pid = child;
#endif
    out->running = true;
    return true;
}

/* The handle a started child carries, in the shape the platform half takes. */
static child_handle handle_of(const process_handle *handle) {
#ifdef _WIN32
    return handle->process;
#else
    return handle->pid;
#endif
}

int process_wait(process_handle *handle) {
    if(handle == NULL || !handle->running)
        return -1;
    handle->running = false;
    return child_wait(handle_of(handle));
}

void process_kill(process_handle *handle) {
    if(handle == NULL || !handle->running)
        return;
    handle->running = false;
    child_kill(handle_of(handle));
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

/* Milliseconds on the monotonic clock, which is what a deadline has to be
   measured against: the wall clock can step backwards and a build would then
   wait for a plugin twice. */
static long long monotonic_ms(void) { return (long long)(clock_monotonic_seconds() * 1000.0); }

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

/* Take what has just been read, or say why the exchange should stop. */
static bool answer_take(process_exchange *io, size_t *capacity, const char *chunk, size_t got,
                        process_exchange_result *result) {
    if(io->answer_max > 0 && io->answer_size + got > io->answer_max) {
        *result = process_exchange_too_large;
        return false;
    }
    if(!answer_append(&io->answer, &io->answer_size, capacity, chunk, got)) {
        *result = process_exchange_failed;
        return false;
    }
    return true;
}

/*
 * Driving both directions at once.
 *
 * Writing the whole request first would deadlock the moment a child answers
 * before it has finished reading: the parent blocks writing into a full pipe
 * while the child blocks writing into one nobody is draining. Neither side is
 * at fault and neither can recover.
 *
 * The two systems avoid that differently, and the difference is not a
 * preference. POSIX can ask two descriptors at once which of them is ready,
 * and can make a write return short rather than block — so one thread does
 * both. Windows can do neither to an anonymous pipe: there is no poll that
 * takes one, and no non-blocking mode. What it does have is a thread, so the
 * request is written on its own and the reader is left to drain. Killing the
 * child on a timeout closes its ends, which is what lets that thread finish.
 */

#ifdef _WIN32

typedef struct {
    pipe_end to_child;
    const char *request;
    size_t remaining;
} request_writer;

static int write_the_request(void *arg) {
    request_writer *writer = arg;
    while(writer->remaining > 0) {
        const long wrote = end_write(writer->to_child, writer->request, writer->remaining);
        if(wrote <= 0)
            break; /* the child stopped reading; it has all it wants */
        writer->request += wrote;
        writer->remaining -= (size_t)wrote;
    }
    /* Closed rather than left open, because a plugin reading to EOF is waiting
       for exactly this. */
    end_close(&writer->to_child);
    return 0;
}

/* How long to wait before asking the pipe again whether anything arrived. The
   same figure the POSIX poll uses, for the same reason: short enough that a
   timeout is honoured promptly, long enough not to spin a core. */
#define EXCHANGE_TICK_MS 50

static process_exchange_result exchange_pump(child_handle child, pipe_end to_child,
                                             pipe_end from_child, process_exchange *io) {
    request_writer writer = {
        .to_child = to_child,
        .request = io->request,
        .remaining = io->request == NULL
                         ? 0
                         : (io->request_size > 0 ? io->request_size : strlen(io->request)),
    };

    thread writing = {0};
    bool writing_started = false;
    if(writer.remaining == 0)
        end_close(&to_child); /* nothing to send is a closed stdin, not an open one */
    else
        writing_started = thread_start(&writing, write_the_request, &writer);

    if(writer.remaining > 0 && !writing_started) {
        end_close(&to_child);
        end_close(&from_child);
        return process_exchange_failed;
    }

    const long long deadline = io->timeout_ms == 0 ? 0 : monotonic_ms() + io->timeout_ms;
    process_exchange_result result = process_exchange_ok;
    size_t capacity = 0;

    while(end_open(from_child)) {
        DWORD waiting = 0;
        if(PeekNamedPipe(from_child, NULL, 0, NULL, &waiting, NULL) == 0) {
            end_close(&from_child); /* the child let go of its end */
            break;
        }
        if(waiting == 0) {
            if(deadline != 0 && monotonic_ms() >= deadline) {
                result = process_exchange_timed_out;
                break;
            }
            thread_sleep_ms(EXCHANGE_TICK_MS);
            continue;
        }

        char chunk[EXCHANGE_CHUNK];
        const size_t want = waiting < sizeof chunk ? (size_t)waiting : sizeof chunk;
        const long got = end_read(from_child, chunk, want);
        if(got <= 0) {
            end_close(&from_child);
            break;
        }
        if(!answer_take(io, &capacity, chunk, (size_t)got, &result))
            break;
        if(deadline != 0 && monotonic_ms() >= deadline) {
            result = process_exchange_timed_out;
            break;
        }
    }

    end_close(&from_child);
    if(result != process_exchange_ok) {
        /* Before joining, deliberately: a writer blocked in WriteFile is
           released by the child's handles going away, and nothing else would
           release it. */
        child_kill(child);
        if(writing_started)
            thread_join(&writing);
        return result;
    }
    if(writing_started)
        thread_join(&writing);
    end_close(&writer.to_child);

    io->code = child_wait(child);
    return io->code == EXIT_COMMAND_NOT_RUNNABLE ? process_exchange_not_started
                                                 : process_exchange_ok;
}

#else

typedef struct {
    pipe_end to_child;   /* PIPE_NONE once the request is written and the pipe closed */
    pipe_end from_child; /* PIPE_NONE once the answer reached EOF */
    const char *request;
    size_t remaining;
} exchange_state;

/* One turn of the loop: whichever end is ready, moved by one chunk.
   Returns false when the exchange should stop, with `*result` saying why. */
static bool exchange_step(exchange_state *state, process_exchange *io, size_t *capacity,
                          process_exchange_result *result) {
    struct pollfd watched[2];
    int count = 0;
    if(end_open(state->to_child))
        watched[count++] = (struct pollfd){.fd = state->to_child, .events = POLLOUT};
    if(end_open(state->from_child))
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
            const long wrote = end_write(state->to_child, state->request, state->remaining);
            if(wrote == -2)
                continue; /* the pipe filled between the poll and the write */
            if(wrote <= 0) {
                /* A child that stopped reading has all the request it wants.
                   With SIGPIPE ignored this is a failed write and not a death. */
                end_close(&state->to_child);
                continue;
            }
            state->request += wrote;
            state->remaining -= (size_t)wrote;
            if(state->remaining == 0) {
                /* Closed rather than left open, because a plugin reading to EOF
                   is waiting for exactly this. */
                end_close(&state->to_child);
            }
            continue;
        }

        char chunk[EXCHANGE_CHUNK];
        const long got = end_read(state->from_child, chunk, sizeof chunk);
        if(got == -2)
            continue; /* drained between the poll and the read */
        if(got <= 0) {
            end_close(&state->from_child);
            continue;
        }
        if(!answer_take(io, capacity, chunk, (size_t)got, result))
            return false;
    }
    return true;
}

static process_exchange_result exchange_pump(child_handle child, pipe_end to_child,
                                             pipe_end from_child, process_exchange *io) {
    /* The parent's ends are non-blocking, and this is what makes the poll loop
       above actually work rather than merely look like it does. On a blocking
       pipe, write() with more than PIPE_BUF bytes does not return until every
       byte is written — so the parent would sit inside one write() while the
       child filled the pipe coming back, which is precisely the deadlock the
       loop exists to avoid. Non-blocking turns that into a partial write the
       loop can interleave with a read. */
    (void)fcntl(to_child, F_SETFL, O_NONBLOCK);
    (void)fcntl(from_child, F_SETFL, O_NONBLOCK);

    exchange_state state = {
        .to_child = to_child,
        .from_child = from_child,
        .request = io->request,
        .remaining = io->request == NULL
                         ? 0
                         : (io->request_size > 0 ? io->request_size : strlen(io->request)),
    };
    /* Nothing to send is a closed stdin, not an open one: a frontend asked for
       nothing still reads to EOF. */
    if(state.remaining == 0)
        end_close(&state.to_child);

    const long long deadline = io->timeout_ms == 0 ? 0 : monotonic_ms() + io->timeout_ms;
    process_exchange_result result = process_exchange_ok;
    size_t capacity = 0;

    while(end_open(state.to_child) || end_open(state.from_child)) {
        if(!exchange_step(&state, io, &capacity, &result))
            break;
        if(deadline != 0 && monotonic_ms() >= deadline) {
            result = process_exchange_timed_out;
            break;
        }
    }

    end_close(&state.to_child);
    end_close(&state.from_child);

    if(result != process_exchange_ok) {
        child_kill(child);
        return result;
    }

    io->code = child_wait(child);
    if(io->code < 0)
        return process_exchange_failed;
    /* An exec that failed reports as a shell does, and that is not a plugin
       that ran and answered — it is one that never started. */
    return io->code == EXIT_COMMAND_NOT_RUNNABLE ? process_exchange_not_started
                                                 : process_exchange_ok;
}

#endif

process_exchange_result process_exchange_run(const char *const argv[], process_exchange *io) {
    if(io == NULL || argv == NULL || argv[0] == NULL)
        return process_exchange_not_started;

    io->answer = NULL;
    io->answer_size = 0;
    io->code = -1;

    pipe_end to_child[2] = {PIPE_NONE, PIPE_NONE};
    pipe_end from_child[2] = {PIPE_NONE, PIPE_NONE};
    if(!open_pipe(to_child))
        return process_exchange_not_started;
    if(!open_pipe(from_child)) {
        end_close(&to_child[PIPE_READ]);
        end_close(&to_child[PIPE_WRITE]);
        return process_exchange_not_started;
    }
    end_keep_private(to_child[PIPE_WRITE]);
    end_keep_private(from_child[PIPE_READ]);

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

#ifndef _WIN32
    /* A child that exits without reading turns the parent's next write into
       SIGPIPE, which would end molto rather than the exchange. Ignored for the
       duration and restored after, so nothing else inherits the change.
       Windows has no such signal: a write to a pipe nobody holds is an error
       the caller reads, which is what end_write already reports. */
    void (*previous_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);
#endif

    const child_handle child = spawn_child(argv, io->env, io->env_count, &wiring);
    end_close(&to_child[PIPE_READ]);
    end_close(&from_child[PIPE_WRITE]);
    if(!child_started(child)) {
        end_close(&to_child[PIPE_WRITE]);
        end_close(&from_child[PIPE_READ]);
#ifndef _WIN32
        (void)signal(SIGPIPE, previous_sigpipe);
#endif
        return process_exchange_not_started;
    }

    const process_exchange_result result =
        exchange_pump(child, to_child[PIPE_WRITE], from_child[PIPE_READ], io);

#ifndef _WIN32
    (void)signal(SIGPIPE, previous_sigpipe);
#endif
    return result;
}
