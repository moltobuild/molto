#ifndef MOLTO_TASK_POOL_H
#define MOLTO_TASK_POOL_H

#include <stdbool.h>
#include <stddef.h>

/* A work-stealing thread pool over OS threads (C11 <threads.h>).
   Each worker owns a deque; idle workers steal tasks from others. Submit many
   more tasks than workers freely: they queue and drain. Reusable across many
   fan-out workloads (compilation, and future ones). */
typedef struct task_pool task_pool;

/* A unit of work: a function and its opaque argument. */
typedef void (*task_fn)(void *arg);

/* Create a pool with `workers` threads. `workers == 0` selects the number of
   online CPUs (at least 1). Returns NULL on failure. */
[[nodiscard]] task_pool *task_pool_create(size_t workers);

/* Submit a task for execution. Returns false if it could not be enqueued. */
[[nodiscard]] bool task_pool_submit(task_pool *pool, task_fn fn, void *arg);

/* Block until every task submitted so far has finished executing. */
void task_pool_wait(task_pool *pool);

/* Stop the workers and free the pool. Safe to call after task_pool_wait. */
void task_pool_destroy(task_pool *pool);

/* Number of worker threads in the pool. */
[[nodiscard]] size_t task_pool_workers(const task_pool *pool);

#endif /* MOLTO_TASK_POOL_H */
