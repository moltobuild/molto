#ifndef MOLTO_TASK_POOL_H
#define MOLTO_TASK_POOL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A work-stealing thread pool built on OS threads (C11 <threads.h>).
 *
 * Use it to run many independent tasks across CPU cores at once. Each worker
 * thread owns a queue; an idle worker steals tasks from a busy one, so the load
 * balances itself. You may submit far more tasks than there are workers — they
 * queue up and drain. With a single worker (e.g. a 1-core machine) that one
 * worker simply runs every queued task in turn.
 *
 * Typical usage:
 *     task_pool *pool = task_pool_create(0);   // 0 => one worker per CPU core
 *     for (each item)
 *         task_pool_submit(pool, do_work, item);   // item outlives the wait
 *     task_pool_wait(pool);                    // block until all tasks finish
 *     task_pool_destroy(pool);
 *
 * Note: tasks run on other threads, so anything they share (counters, flags)
 * must be synchronised by the caller (e.g. with <stdatomic.h>).
 */
typedef struct task_pool task_pool;

/* A unit of work: a function and its opaque argument. */
typedef void (*task_fn)(void *arg);

/* Create a pool with `workers` threads. `workers == 0` selects the number of
   online CPUs (at least 1). Returns NULL on failure. Free it with
   task_pool_destroy. */
[[nodiscard]] task_pool *task_pool_create(size_t workers);

/* Queue a task for execution. `arg` must stay valid until task_pool_wait
   returns. Returns false if the task could not be enqueued (out of memory). */
[[nodiscard]] bool task_pool_submit(task_pool *pool, task_fn fn, void *arg);

/* Block the calling thread until every task submitted so far has finished. The
   pool can be reused for another batch afterwards. */
void task_pool_wait(task_pool *pool);

/* Stop the workers (joining their threads) and free the pool. Call it after
   task_pool_wait. Safe on NULL. */
void task_pool_destroy(task_pool *pool);

/* Number of worker threads in the pool. */
[[nodiscard]] size_t task_pool_workers(const task_pool *pool);

#endif /* MOLTO_TASK_POOL_H */
