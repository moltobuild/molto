#include <molto/util/task_pool.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    task_fn fn;
    void *arg;
} task;

/* A double-ended queue of tasks guarded by its own mutex. The owning worker
   pushes/pops at the bottom (LIFO, good locality); thieves steal from the top
   (FIFO). Not lock-free, but correct and easy to reason about. */
typedef struct {
    task *items;
    size_t capacity;
    size_t top;
    size_t bottom;
    mtx_t mtx;
} deque;

typedef struct {
    deque queue;
    thrd_t thread;
    uint64_t rng;
    struct task_pool *pool;
    bool started;
} worker;

struct task_pool {
    worker *workers;
    size_t worker_count;
    atomic_bool shutdown;
    atomic_size_t pending;
    atomic_size_t rr;

    mtx_t work_mtx;
    cnd_t work_cnd;
    mtx_t done_mtx;
    cnd_t done_cnd;
};

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
    size_t next = d->capacity == 0 ? 16 : d->capacity * 2;
    task *items = realloc(d->items, next * sizeof(task));
    if (items == NULL)
        return false;
    d->items = items;
    d->capacity = next;
    return true;
}

static bool deque_push_bottom(deque *d, task value) {
    bool ok = true;
    mtx_lock(&d->mtx);
    if (d->top == d->bottom) {
        d->top = 0;
        d->bottom = 0;
    } else if (d->bottom == d->capacity && d->top > 0) {
        size_t live = d->bottom - d->top;
        memmove(d->items, d->items + d->top, live * sizeof(task));
        d->top = 0;
        d->bottom = live;
    }
    if (d->bottom == d->capacity && !deque_grow(d))
        ok = false;
    else
        d->items[d->bottom++] = value;
    mtx_unlock(&d->mtx);
    return ok;
}

static bool deque_pop_bottom(deque *d, task *out) {
    bool got = false;
    mtx_lock(&d->mtx);
    if (d->bottom > d->top) {
        *out = d->items[--d->bottom];
        got = true;
    }
    mtx_unlock(&d->mtx);
    return got;
}

static bool deque_steal_top(deque *d, task *out) {
    bool got = false;
    mtx_lock(&d->mtx);
    if (d->top < d->bottom) {
        *out = d->items[d->top++];
        got = true;
    }
    mtx_unlock(&d->mtx);
    return got;
}

/* Find the next task: pop from the worker's own deque, otherwise steal from a
   pseudo-random victim, scanning all peers once. */
static bool pool_next_task(task_pool *pool, worker *self, task *out) {
    if (deque_pop_bottom(&self->queue, out))
        return true;
    size_t count = pool->worker_count;
    if (count <= 1)
        return false;
    self->rng ^= self->rng << 13;
    self->rng ^= self->rng >> 7;
    self->rng ^= self->rng << 17;
    size_t start = (size_t)(self->rng % count);
    for (size_t k = 0; k < count; k++) {
        worker *victim = &pool->workers[(start + k) % count];
        if (victim == self)
            continue;
        if (deque_steal_top(&victim->queue, out))
            return true;
    }
    return false;
}

static void pool_mark_done(task_pool *pool) {
    if (atomic_fetch_sub(&pool->pending, 1) == 1) {
        mtx_lock(&pool->done_mtx);
        cnd_broadcast(&pool->done_cnd);
        mtx_unlock(&pool->done_mtx);
    }
}

static void pool_park(task_pool *pool) {
    mtx_lock(&pool->work_mtx);
    if (!atomic_load(&pool->shutdown)) {
        struct timespec deadline;
        timespec_get(&deadline, TIME_UTC);
        deadline.tv_nsec += 2 * 1000 * 1000; /* 2 ms */
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        cnd_timedwait(&pool->work_cnd, &pool->work_mtx, &deadline);
    }
    mtx_unlock(&pool->work_mtx);
}

static int worker_main(void *arg) {
    worker *self = arg;
    task_pool *pool = self->pool;
    while (!atomic_load(&pool->shutdown)) {
        task current;
        if (pool_next_task(pool, self, &current)) {
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

task_pool *task_pool_create(size_t workers) {
    if (workers == 0) {
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        workers = online > 0 ? (size_t)online : 1;
    }

    task_pool *pool = calloc(1, sizeof *pool);
    if (pool == NULL)
        return NULL;
    pool->worker_count = workers;
    atomic_init(&pool->shutdown, false);
    atomic_init(&pool->pending, 0);
    atomic_init(&pool->rr, 0);

    if (mtx_init(&pool->work_mtx, mtx_plain) != thrd_success
        || cnd_init(&pool->work_cnd) != thrd_success
        || mtx_init(&pool->done_mtx, mtx_plain) != thrd_success
        || cnd_init(&pool->done_cnd) != thrd_success) {
        free(pool);
        return NULL;
    }

    pool->workers = calloc(workers, sizeof(worker));
    if (pool->workers == NULL) {
        destroy_sync(pool);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < workers; i++) {
        worker *w = &pool->workers[i];
        w->pool = pool;
        w->rng = 0x9e3779b97f4a7c15ULL ^ (uint64_t)(i + 1);
        w->started = false;
        if (!deque_init(&w->queue)) {
            for (size_t j = 0; j < i; j++)
                deque_destroy(&pool->workers[j].queue);
            free(pool->workers);
            destroy_sync(pool);
            free(pool);
            return NULL;
        }
    }

    for (size_t i = 0; i < workers; i++) {
        if (thrd_create(&pool->workers[i].thread, worker_main, &pool->workers[i])
            == thrd_success) {
            pool->workers[i].started = true;
        } else {
            task_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

bool task_pool_submit(task_pool *pool, task_fn fn, void *arg) {
    task value = { .fn = fn, .arg = arg };
    size_t index = atomic_fetch_add(&pool->rr, 1) % pool->worker_count;
    atomic_fetch_add(&pool->pending, 1);
    if (!deque_push_bottom(&pool->workers[index].queue, value)) {
        atomic_fetch_sub(&pool->pending, 1);
        return false;
    }
    mtx_lock(&pool->work_mtx);
    cnd_signal(&pool->work_cnd);
    mtx_unlock(&pool->work_mtx);
    return true;
}

void task_pool_wait(task_pool *pool) {
    mtx_lock(&pool->done_mtx);
    while (atomic_load(&pool->pending) > 0)
        cnd_wait(&pool->done_cnd, &pool->done_mtx);
    mtx_unlock(&pool->done_mtx);
}

void task_pool_destroy(task_pool *pool) {
    if (pool == NULL)
        return;
    atomic_store(&pool->shutdown, true);
    mtx_lock(&pool->work_mtx);
    cnd_broadcast(&pool->work_cnd);
    mtx_unlock(&pool->work_mtx);
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i].started)
            thrd_join(pool->workers[i].thread, NULL);
    }
    for (size_t i = 0; i < pool->worker_count; i++)
        deque_destroy(&pool->workers[i].queue);
    free(pool->workers);
    destroy_sync(pool);
    free(pool);
}

size_t task_pool_workers(const task_pool *pool) {
    return pool->worker_count;
}
