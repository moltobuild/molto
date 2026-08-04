/*
 * task_pool — a work-stealing thread pool.
 *
 * The goal is to run many independent tasks (e.g. compiling one source file
 * each) across several CPU cores at once.
 *
 * How it works, in plain terms:
 *   - A "worker" is a real operating-system thread (thrd_t). We create one per
 *     CPU core by default. They run for the whole life of the pool.
 *   - Each worker owns its own queue of tasks, called a "deque" (double-ended
 *     queue). The owner adds and removes tasks at the BOTTOM (last-in first-out,
 *     which keeps recently added work hot in the CPU cache). Other, idle workers
 *     "steal" tasks from the TOP of someone else's deque (first-in first-out).
 *     This is what "work-stealing" means: a worker that runs out of its own work
 *     grabs work from a busy neighbour, so no core sits idle while others are
 *     overloaded.
 *   - When a worker finds no work anywhere, it "parks" (sleeps briefly) instead
 *     of spinning the CPU, and wakes up when new work arrives or on shutdown.
 *   - `pending` counts tasks that were submitted but not finished yet.
 *     task_pool_wait() blocks until it reaches zero.
 *
 * C primitives used here:
 *   - thrd_t / thrd_create / thrd_join : an OS thread and its start/join.
 *   - mtx_t (mutex)      : a lock; only one thread holds it at a time.
 *   - cnd_t (condition)  : a way to sleep until another thread signals you.
 *   - atomic_*           : reads/writes that are safe to do from many threads
 *                          without a lock (used for simple counters and flags).
 */

#include <molto/util/task_pool.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

/* Growth policy for a worker's deque: start small, then double on demand. */
#define DEQUE_INITIAL_CAPACITY 16
#define DEQUE_GROWTH_FACTOR 2

/* How long an idle worker sleeps before re-checking for stealable work. The
 * timeout (rather than an indefinite wait) also guarantees liveness even if a
 * wake-up signal is ever missed. */
#define PARK_TIMEOUT_MS 2
#define NANOS_PER_MILLI (1000L * 1000L)
#define NANOS_PER_SECOND (1000L * 1000L * 1000L)

/* xorshift64: a tiny, fast pseudo-random number generator. Each worker uses it
 * to pick a random victim to steal from, which spreads out stealing and avoids
 * everyone hammering the same neighbour. The shift amounts are the classic
 * xorshift constants; the seed basis is the odd "golden ratio" constant used to
 * give each worker a different, well-mixed starting seed. */
#define XORSHIFT_SHIFT_A 13
#define XORSHIFT_SHIFT_B 7
#define XORSHIFT_SHIFT_C 17
#define XORSHIFT_SEED_BASIS 0x9e3779b97f4a7c15ULL

/* A single unit of work: a function plus the argument to call it with. */
typedef struct {
    task_fn fn;
    void *arg;
} task;

/* A double-ended queue of tasks guarded by its own mutex. The owning worker
 * pushes/pops at the bottom (LIFO, good cache locality); thieves steal from the
 * top (FIFO). This is a simple mutex-based deque — not lock-free, but correct
 * and easy to reason about. */
typedef struct {
    task *items;
    size_t capacity;
    size_t top;    /* index thieves steal from */
    size_t bottom; /* index the owner pushes to / pops from */
    mtx_t mtx;
} deque;

/* One worker: its own deque, its OS thread, and a private RNG state. */
typedef struct {
    deque queue;
    thrd_t thread;
    uint64_t rng; /* xorshift64 state for victim selection */
    struct task_pool *pool;
    bool started; /* was the OS thread actually created? */
} worker;

struct task_pool {
    worker *workers;
    size_t worker_count;
    atomic_bool shutdown;           /* set to true to make workers exit */
    atomic_size_t pending;          /* submitted-but-not-finished task count */
    atomic_size_t round_robin_next; /* next worker index to receive a submit */

    /* Idle workers wait on work_cnd; submitters signal it. */
    mtx_t work_mtx;
    cnd_t work_cnd;
    /* task_pool_wait waits on done_cnd; it fires when pending reaches zero. */
    mtx_t done_mtx;
    cnd_t done_cnd;
};

/* --- deque operations (each locks the deque's own mutex) --- */

static bool deque_init(deque *d) {
    d->items = NULL;
    d->capacity = 0;
    d->top = 0;
    d->bottom = 0;
    return mtx_init(&d->mtx, mtx_plain) == thrd_success;
}

static void deque_destroy(deque *d) {
    mtx_destroy(&d->mtx);
    free(d->items);
}

static bool deque_grow(deque *d) {
    size_t next = d->capacity == 0 ? DEQUE_INITIAL_CAPACITY : d->capacity * DEQUE_GROWTH_FACTOR;
    task *items = realloc(d->items, next * sizeof(task));
    if(items == NULL)
        return false;
    d->items = items;
    d->capacity = next;
    return true;
}

/* Add a task at the bottom (the owner's end). Also compacts/resets the indices
 * when possible so memory stays bounded to the queue's live size. */
static bool deque_push_bottom(deque *d, task value) {
    bool ok = true;
    mtx_lock(&d->mtx);
    if(d->top == d->bottom) {
        /* Empty: reset both indices to the front. */
        d->top = 0;
        d->bottom = 0;
    } else if(d->bottom == d->capacity && d->top > 0) {
        /* Full at the back but free space at the front: slide items down. */
        size_t live = d->bottom - d->top;
        memmove(d->items, d->items + d->top, live * sizeof(task));
        d->top = 0;
        d->bottom = live;
    }
    if(d->bottom == d->capacity && !deque_grow(d))
        ok = false;
    else
        d->items[d->bottom++] = value;
    mtx_unlock(&d->mtx);
    return ok;
}

/* Remove a task from the bottom (owner side, LIFO). */
static bool deque_pop_bottom(deque *d, task *out) {
    bool got = false;
    mtx_lock(&d->mtx);
    if(d->bottom > d->top) {
        *out = d->items[--d->bottom];
        got = true;
    }
    mtx_unlock(&d->mtx);
    return got;
}

/* Steal a task from the top (thief side, FIFO). */
static bool deque_steal_top(deque *d, task *out) {
    bool got = false;
    mtx_lock(&d->mtx);
    if(d->top < d->bottom) {
        *out = d->items[d->top++];
        got = true;
    }
    mtx_unlock(&d->mtx);
    return got;
}

/* --- pool internals --- */

/* Find the next task for `self`: prefer its own deque, otherwise steal from a
 * pseudo-random victim, scanning every peer once. Returns false if no work was
 * found anywhere on this pass. */
static bool pool_next_task(task_pool *pool, worker *self, task *out) {
    if(deque_pop_bottom(&self->queue, out))
        return true;
    size_t count = pool->worker_count;
    if(count <= 1)
        return false;
    /* Advance the worker's xorshift64 RNG and pick a random starting victim. */
    self->rng ^= self->rng << XORSHIFT_SHIFT_A;
    self->rng ^= self->rng >> XORSHIFT_SHIFT_B;
    self->rng ^= self->rng << XORSHIFT_SHIFT_C;
    size_t start = (size_t)(self->rng % count);
    for(size_t offset = 0; offset < count; offset++) {
        worker *victim = &pool->workers[(start + offset) % count];
        if(victim == self)
            continue;
        if(deque_steal_top(&victim->queue, out))
            return true;
    }
    return false;
}

/* Called after a task finishes: decrement pending and, if it hit zero, wake any
 * thread blocked in task_pool_wait. atomic_fetch_sub returns the value BEFORE
 * the subtraction, so a return of 1 means we just reached 0. */
static void pool_mark_done(task_pool *pool) {
    if(atomic_fetch_sub(&pool->pending, 1) == 1) {
        mtx_lock(&pool->done_mtx);
        cnd_broadcast(&pool->done_cnd);
        mtx_unlock(&pool->done_mtx);
    }
}

/* Sleep briefly while there is no work, so an idle worker does not burn CPU. */
static void pool_park(task_pool *pool) {
    mtx_lock(&pool->work_mtx);
    if(!atomic_load(&pool->shutdown)) {
        struct timespec deadline;
        timespec_get(&deadline, TIME_UTC);
        deadline.tv_nsec += PARK_TIMEOUT_MS * NANOS_PER_MILLI;
        if(deadline.tv_nsec >= NANOS_PER_SECOND) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= NANOS_PER_SECOND;
        }
        cnd_timedwait(&pool->work_cnd, &pool->work_mtx, &deadline);
    }
    mtx_unlock(&pool->work_mtx);
}

/* The function every worker thread runs: pull work and execute it until the
 * pool is asked to shut down. */
static int worker_main(void *arg) {
    worker *self = arg;
    task_pool *pool = self->pool;
    while(!atomic_load(&pool->shutdown)) {
        task current;
        if(pool_next_task(pool, self, &current)) {
            current.fn(current.arg);
            pool_mark_done(pool);
        } else {
            pool_park(pool);
        }
    }
    return 0;
}

static void destroy_sync(task_pool *pool) {
    cnd_destroy(&pool->work_cnd);
    cnd_destroy(&pool->done_cnd);
    mtx_destroy(&pool->work_mtx);
    mtx_destroy(&pool->done_mtx);
}

/* --- public API --- */

task_pool *task_pool_create(size_t workers) {
    if(workers == 0) {
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        workers = online > 0 ? (size_t)online : 1;
    }

    task_pool *pool = calloc(1, sizeof *pool);
    if(pool == NULL)
        return NULL;
    pool->worker_count = workers;
    atomic_init(&pool->shutdown, false);
    atomic_init(&pool->pending, 0);
    atomic_init(&pool->round_robin_next, 0);

    if(mtx_init(&pool->work_mtx, mtx_plain) != thrd_success ||
       cnd_init(&pool->work_cnd) != thrd_success ||
       mtx_init(&pool->done_mtx, mtx_plain) != thrd_success ||
       cnd_init(&pool->done_cnd) != thrd_success) {
        free(pool);
        return NULL;
    }

    pool->workers = calloc(workers, sizeof(worker));
    if(pool->workers == NULL) {
        destroy_sync(pool);
        free(pool);
        return NULL;
    }

    /* Initialise every deque before starting any thread. */
    for(size_t i = 0; i < workers; i++) {
        worker *w = &pool->workers[i];
        w->pool = pool;
        w->rng = XORSHIFT_SEED_BASIS ^ (uint64_t)(i + 1); /* distinct per worker */
        w->started = false;
        if(!deque_init(&w->queue)) {
            for(size_t j = 0; j < i; j++)
                deque_destroy(&pool->workers[j].queue);
            free(pool->workers);
            destroy_sync(pool);
            free(pool);
            return NULL;
        }
    }

    /* Start the worker threads. */
    for(size_t i = 0; i < workers; i++) {
        if(thrd_create(&pool->workers[i].thread, worker_main, &pool->workers[i]) == thrd_success) {
            pool->workers[i].started = true;
        } else {
            /* Could not start them all: tear everything down cleanly. */
            task_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

bool task_pool_submit(task_pool *pool, task_fn fn, void *arg) {
    task value = {.fn = fn, .arg = arg};
    /* Spread submissions across workers in round-robin order; work-stealing
     * rebalances any imbalance afterwards. */
    size_t index = atomic_fetch_add(&pool->round_robin_next, 1) % pool->worker_count;
    atomic_fetch_add(&pool->pending, 1);
    if(!deque_push_bottom(&pool->workers[index].queue, value)) {
        atomic_fetch_sub(&pool->pending, 1);
        return false;
    }
    /* Wake one parked worker to pick this up. */
    mtx_lock(&pool->work_mtx);
    cnd_signal(&pool->work_cnd);
    mtx_unlock(&pool->work_mtx);
    return true;
}

void task_pool_wait(task_pool *pool) {
    mtx_lock(&pool->done_mtx);
    while(atomic_load(&pool->pending) > 0)
        cnd_wait(&pool->done_cnd, &pool->done_mtx);
    mtx_unlock(&pool->done_mtx);
}

void task_pool_destroy(task_pool *pool) {
    if(pool == NULL)
        return;
    /* Ask workers to stop and wake every parked one. */
    atomic_store(&pool->shutdown, true);
    mtx_lock(&pool->work_mtx);
    cnd_broadcast(&pool->work_cnd);
    mtx_unlock(&pool->work_mtx);
    for(size_t i = 0; i < pool->worker_count; i++) {
        if(pool->workers[i].started)
            thrd_join(pool->workers[i].thread, NULL);
    }
    for(size_t i = 0; i < pool->worker_count; i++)
        deque_destroy(&pool->workers[i].queue);
    free(pool->workers);
    destroy_sync(pool);
    free(pool);
}

size_t task_pool_workers(const task_pool *pool) { return pool->worker_count; }
