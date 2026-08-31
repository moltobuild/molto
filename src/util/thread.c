#include <molto/util/thread.h>

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <threads.h>
#include <time.h>
#include <unistd.h>
#endif

/* Nanoseconds in one millisecond and in one second, for the two conversions
   the POSIX side has to make. */
#define NANOS_PER_MILLI 1000000L
#define NANOS_PER_SECOND 1000000000L

/* ------------------------------------------------------------------ */
/* Mutexes                                                             */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

bool mutex_init(mutex *out) {
    CRITICAL_SECTION *section = malloc(sizeof *section);
    if(section == NULL)
        return false;
    InitializeCriticalSection(section);
    out->impl = section;
    return true;
}

void mutex_destroy(mutex *m) {
    if(m == NULL || m->impl == NULL)
        return;
    DeleteCriticalSection(m->impl);
    free(m->impl);
    m->impl = NULL;
}

void mutex_lock(mutex *m) { EnterCriticalSection(m->impl); }
void mutex_unlock(mutex *m) { LeaveCriticalSection(m->impl); }

#else

bool mutex_init(mutex *out) {
    mtx_t *lock = malloc(sizeof *lock);
    if(lock == NULL)
        return false;
    if(mtx_init(lock, mtx_plain) != thrd_success) {
        free(lock);
        return false;
    }
    out->impl = lock;
    return true;
}

void mutex_destroy(mutex *m) {
    if(m == NULL || m->impl == NULL)
        return;
    mtx_destroy(m->impl);
    free(m->impl);
    m->impl = NULL;
}

void mutex_lock(mutex *m) { (void)mtx_lock(m->impl); }
void mutex_unlock(mutex *m) { (void)mtx_unlock(m->impl); }

#endif

/* ------------------------------------------------------------------ */
/* Condition variables                                                 */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

bool condition_init(condition *out) {
    CONDITION_VARIABLE *variable = malloc(sizeof *variable);
    if(variable == NULL)
        return false;
    InitializeConditionVariable(variable);
    out->impl = variable;
    return true;
}

/* Nothing to release: a condition variable is a value, not a kernel object,
   and Windows has no call that destroys one. */
void condition_destroy(condition *c) {
    if(c == NULL || c->impl == NULL)
        return;
    free(c->impl);
    c->impl = NULL;
}

void condition_wait(condition *c, mutex *m) {
    (void)SleepConditionVariableCS(c->impl, m->impl, INFINITE);
}

void condition_wait_ms(condition *c, mutex *m, unsigned milliseconds) {
    (void)SleepConditionVariableCS(c->impl, m->impl, (DWORD)milliseconds);
}

void condition_signal(condition *c) { WakeConditionVariable(c->impl); }
void condition_broadcast(condition *c) { WakeAllConditionVariable(c->impl); }

#else

bool condition_init(condition *out) {
    cnd_t *variable = malloc(sizeof *variable);
    if(variable == NULL)
        return false;
    if(cnd_init(variable) != thrd_success) {
        free(variable);
        return false;
    }
    out->impl = variable;
    return true;
}

void condition_destroy(condition *c) {
    if(c == NULL || c->impl == NULL)
        return;
    cnd_destroy(c->impl);
    free(c->impl);
    c->impl = NULL;
}

void condition_wait(condition *c, mutex *m) { (void)cnd_wait(c->impl, m->impl); }

void condition_wait_ms(condition *c, mutex *m, unsigned milliseconds) {
    /* C11 wants the moment to stop waiting, not the length of the wait, so the
       relative figure becomes a deadline here — the carry included, which is
       what every caller used to spell out for itself. */
    struct timespec deadline;
    if(timespec_get(&deadline, TIME_UTC) != TIME_UTC)
        return;
    deadline.tv_sec += (time_t)(milliseconds / 1000U);
    deadline.tv_nsec += (long)(milliseconds % 1000U) * NANOS_PER_MILLI;
    if(deadline.tv_nsec >= NANOS_PER_SECOND) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= NANOS_PER_SECOND;
    }
    (void)cnd_timedwait(c->impl, m->impl, &deadline);
}

void condition_signal(condition *c) { (void)cnd_signal(c->impl); }
void condition_broadcast(condition *c) { (void)cnd_broadcast(c->impl); }

#endif

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

/* Win32 hands a thread one pointer and wants a DWORD back; C11 hands it one
   pointer and wants an int. The trampoline is the whole difference, and it
   keeps every caller writing the C11 shape. */
typedef struct {
    int (*body)(void *);
    void *arg;
} win_thread_body;

static DWORD WINAPI win_thread_main(LPVOID arg) {
    win_thread_body *carried = arg;
    int (*body)(void *) = carried->body;
    void *own = carried->arg;
    free(carried);
    return (DWORD)body(own);
}

bool thread_start(thread *out, int (*body)(void *), void *arg) {
    out->running = false;
    win_thread_body *carried = malloc(sizeof *carried);
    if(carried == NULL)
        return false;
    carried->body = body;
    carried->arg = arg;

    HANDLE handle = CreateThread(NULL, 0, win_thread_main, carried, 0, NULL);
    if(handle == NULL) {
        free(carried);
        return false;
    }
    out->impl = handle;
    out->running = true;
    return true;
}

void thread_join(thread *t) {
    if(t == NULL || !t->running)
        return;
    (void)WaitForSingleObject(t->impl, INFINITE);
    (void)CloseHandle(t->impl);
    t->impl = NULL;
    t->running = false;
}

void thread_sleep_ms(unsigned milliseconds) { Sleep((DWORD)milliseconds); }

#else

bool thread_start(thread *out, int (*body)(void *), void *arg) {
    out->running = false;
    thrd_t *id = malloc(sizeof *id);
    if(id == NULL)
        return false;
    if(thrd_create(id, body, arg) != thrd_success) {
        free(id);
        return false;
    }
    out->impl = id;
    out->running = true;
    return true;
}

void thread_join(thread *t) {
    if(t == NULL || !t->running)
        return;
    (void)thrd_join(*(thrd_t *)t->impl, NULL);
    free(t->impl);
    t->impl = NULL;
    t->running = false;
}

void thread_sleep_ms(unsigned milliseconds) {
    const struct timespec interval = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * NANOS_PER_MILLI,
    };
    (void)thrd_sleep(&interval, NULL);
}

#endif

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

size_t thread_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors > 0 ? (size_t)info.dwNumberOfProcessors : 1;
#else
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? (size_t)online : 1;
#endif
}
