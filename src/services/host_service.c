#include <molto/services/host_service.h>

#include <molto/services/process_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The resolver, named once. Overridable so a test can answer without a
   pkg-config installed, and so a machine that keeps it somewhere unusual is not
   stuck — the same escape MOLTO_CLANG_FORMAT and MOLTO_CLANG_TIDY already
   offer. */
#define HOST_RESOLVER_ENV "MOLTO_PKG_CONFIG"
#define HOST_RESOLVER "pkg-config"

/* One capability's answer, in one call. `--cflags --libs` together rather than
   twice, because two invocations of a resolver can disagree if something
   changes between them, and because it is one process instead of two. */
#define HOST_ANSWER_MAX 8192

static bool fail(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

static const char *resolver(void) {
    const char *override = getenv(HOST_RESOLVER_ENV);
    return override != NULL && override[0] != '\0' ? override : HOST_RESOLVER;
}

/* --- running it --- */

/* Captured, never inherited: the answer is the point, and a resolver writing
   to Molto's own stdout would land in the middle of a build report. */
static bool ask(const char *const argv[], char *out, size_t out_size) {
    process_spec spec = {0};
    spec.stdout_to = process_stream_capture;
    spec.stderr_to = process_stream_capture;
    spec.capture = out;
    spec.capture_size = out_size;
    out[0] = '\0';
    return process_execute(argv, &spec) == 0 && !spec.truncated;
}

/* --- reading the answer --- */

static bool push_include(host_answer *out, const char *path) {
    if(out->include_count >= HOST_MAX_INCLUDES)
        return false;
    const int written = snprintf(out->includes[out->include_count], HOST_PATH_MAX, "%s", path);
    if(written < 0 || written >= HOST_PATH_MAX)
        return false;
    out->include_count++;
    return true;
}

static bool push_link(host_answer *out, const char *flag) {
    if(out->link_count >= HOST_MAX_LINKS)
        return false;
    const int written = snprintf(out->links[out->link_count], HOST_FLAG_MAX, "%s", flag);
    if(written < 0 || written >= HOST_FLAG_MAX)
        return false;
    out->link_count++;
    return true;
}

/* True for the one linker option a resolver may contribute: where the library
   it just located can be found again at run time.

   Kept for the same reason `-L` is, and it is worth being explicit about the
   reason because `-Wl,` is otherwise an escape hatch onto the whole linker. A
   resolver's job is to say where a library is; `-L` says that for the link and
   `-rpath` says it for the run. They are one fact spelled twice, and keeping
   only the first produces a binary that links and then cannot start — which is
   what a library installed outside the default path does without this. */
static bool is_rpath(const char *token) {
    return strncmp(token, "-Wl,-rpath,", 11) == 0 || strncmp(token, "-Wl,-rpath=", 11) == 0;
}

/* One token of a resolver's answer.
 *
 * Only `-I`, the two link forms and `-rpath` are kept. pkg-config emits
 * `-pthread`, `-D_REENTRANT`, `-m64` and whatever a `.pc` file's author put
 * there, and each of those would be a compile option entering the build from
 * outside the manifest that was reviewed. Dropped silently because the contract
 * says what is kept rather than what is refused: a resolver is not a source of
 * options.
 *
 * `-L` is kept alongside `-l` because a library outside the linker's default
 * path needs both, and dropping one of the pair would fail at link time with a
 * message about neither. */
static bool take(const char *token, host_answer *out) {
    if(strncmp(token, "-I", 2) == 0)
        return token[2] == '\0' || push_include(out, token + 2);
    if(strncmp(token, "-l", 2) == 0 || strncmp(token, "-L", 2) == 0 || is_rpath(token))
        return push_link(out, token);
    return true;
}

/* Split on whitespace, in order. A `.pc` file may quote a path with spaces;
   this does not handle that, and a path with a space in it is reported as a
   token that does not resolve rather than silently half-read. */
static bool read_answer(char *text, host_answer *out, const char *capability, char *err,
                        size_t err_size) {
    for(char *token = strtok(text, " \t\r\n"); token != NULL; token = strtok(NULL, " \t\r\n")) {
        if(!take(token, out))
            return fail(err, err_size,
                        "the resolver's answer for '%s' has more include or link flags than molto "
                        "carries",
                        capability);
    }
    return true;
}

/* --- the public half --- */

bool host_resolve(const char *capability, host_answer *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    if(capability == NULL || capability[0] == '\0')
        return fail(err, err_size, "a host capability with no name cannot be resolved");

    const char *tool = resolver();

    /* Asked before the flags are, so "no pkg-config here" and "no gtk+-3.0
       here" are different messages. The first is about the machine's setup and
       the second about its packages, and a user who is told the wrong one goes
       looking in the wrong place. */
    const char *const exists[] = {tool, "--exists", capability, NULL};
    char scratch[HOST_ANSWER_MAX];
    if(!ask(exists, scratch, sizeof scratch)) {
        const char *const probe[] = {tool, "--version", NULL};
        if(!ask(probe, scratch, sizeof scratch))
            return fail(err, err_size,
                        "'%s' is needed to find the host library '%s' and could not be run; set "
                        "%s if it is installed somewhere unusual",
                        tool, capability, HOST_RESOLVER_ENV);
        return fail(err, err_size,
                    "the host library '%s' is not installed, and molto does not install one: %s "
                    "knows of no such package",
                    capability, tool);
    }

    const char *const flags[] = {tool, "--cflags", "--libs", capability, NULL};
    char answer[HOST_ANSWER_MAX];
    if(!ask(flags, answer, sizeof answer))
        return fail(err, err_size, "%s could not say what '%s' needs", tool, capability);
    if(!read_answer(answer, out, capability, err, err_size))
        return false;

    /* A version is a nicety and its absence is not a failure: a `.pc` file may
       omit it, and what this records is what the resolver said. */
    const char *const version[] = {tool, "--modversion", capability, NULL};
    if(ask(version, scratch, sizeof scratch)) {
        char *end = strpbrk(scratch, " \t\r\n");
        if(end != NULL)
            *end = '\0';
        snprintf(out->version, sizeof out->version, "%.*s", (int)(sizeof out->version - 1),
                 scratch);
    }
    return true;
}

bool host_resolve_all(const project_target *target, host_answer *out, char *err, size_t err_size) {
    for(size_t i = 0; i < target->host_count; i++) {
        if(!host_resolve(target->host[i], &out[i], err, err_size))
            return false;
    }
    return true;
}
