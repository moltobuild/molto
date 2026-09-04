#ifndef MOLTO_THREAD_H
#define MOLTO_THREAD_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Threads, mutexes and condition variables, in the shape Molto uses them.
 *
 * Molto reached for C11 `<threads.h>` directly until the Windows port, where
 * mingw ships no such header — not in the runtime the cross toolchain has, and
 * not in the one MSYS2 installs. The interface below is the subset the three
 * callers actually use, and nothing more: a pool of workers, a drawer, and the
 * two ways of waiting.
 *
 * Apple has the same gap for the same reason — C11 threads are optional and
 * neither vendor implemented them — so the POSIX side is pthreads, which Linux
 * and macOS both have and which is what glibc builds its C11 threads on top of
 * anyway. On Windows these are the Win32 primitives directly, rather than
 * winpthreads, so the binary needs no threading runtime beside it. Three
 * platforms, two implementations.
 *
 * Each handle is one pointer. The alternative — the platform's own struct
 * behind an `#ifdef` — would put <windows.h> in a header every caller
 * includes, and that header defines `ERROR`, `min` and `max` as macros. One
 * allocation per mutex, of which a whole build makes three, is the cheaper
 * side of that trade.
 */

typedef struct {
    void *impl;
} mutex;

typedef struct {
    void *impl;
} condition;

typedef struct {
    void *impl;
    bool running;
} thread;

/* A mutex, held by one thread at a time. Init returns false only if there is
   no memory for it; every other operation cannot fail. */
[[nodiscard]] bool mutex_init(mutex *out);
void mutex_destroy(mutex *m);
void mutex_lock(mutex *m);
void mutex_unlock(mutex *m);

/* A condition variable: a way to sleep until another thread says something
   changed. Every wait must be made holding `m`, and reacquires it on return. */
[[nodiscard]] bool condition_init(condition *out);
void condition_destroy(condition *c);
void condition_wait(condition *c, mutex *m);

/* The same, giving up after `milliseconds` whether or not anyone signalled.

   Relative rather than a deadline, which is what both callers wanted and what
   both platforms take: C11 asks for an absolute `struct timespec` and every
   caller was composing one out of a relative figure, carrying the nanoseconds
   over by hand. */
void condition_wait_ms(condition *c, mutex *m, unsigned milliseconds);

void condition_signal(condition *c);
void condition_broadcast(condition *c);

/* Start `body` on a new thread with `arg`. False if the thread could not be
   created, which every caller treats as "do the work on this thread instead"
   rather than as an error. */
[[nodiscard]] bool thread_start(thread *out, int (*body)(void *), void *arg);

/* Wait for a started thread to finish. Safe on one that never started. */
void thread_join(thread *t);

/* Pause this thread. Used by a drawer between frames. */
void thread_sleep_ms(unsigned milliseconds);

/* How many threads this machine can really run at once, and never zero: a
   count nobody can obtain is reported as one, because a pool of no workers
   does no work. It belongs here rather than beside the filesystem because the
   only caller is the pool deciding how wide to be. */
[[nodiscard]] size_t thread_cpu_count(void);

#endif /* MOLTO_THREAD_H */
