#include <moltest.h>

#include <molto/util/task_pool.h>

#include <stdatomic.h>
#include <threads.h>

static void increment_task(void *arg) {
    atomic_int *counter = arg;
    atomic_fetch_add(counter, 1);
}

static void slow_task(void *arg) {
    struct timespec nap = { .tv_sec = 0, .tv_nsec = 200000 }; /* 0.2 ms */
    thrd_sleep(&nap, NULL);
    atomic_fetch_add((atomic_int *)arg, 1);
}

/* Submit `n` tasks to a pool of `workers` and assert each ran exactly once. */
static void run_batch(size_t workers, int n, task_fn fn) {
    task_pool *pool = task_pool_create(workers);
    EXPECT_TRUE(pool != NULL);
    if (pool == NULL)
        return;
    EXPECT_TRUE(task_pool_workers(pool) == workers);

    atomic_int counter = 0;
    bool submitted = true;
    for (int i = 0; i < n; i++)
        submitted &= task_pool_submit(pool, fn, &counter);
    EXPECT_TRUE(submitted);

    task_pool_wait(pool);
    EXPECT_TRUE(atomic_load(&counter) == n);
    task_pool_destroy(pool);
}

MOLTEST(task_pool) {
    /* Every task runs exactly once, across worker counts and N >> workers.
       workers == 1 covers the "single core drains the whole queue" case. */
    run_batch(1, 1000, increment_task);
    run_batch(2, 1000, increment_task);
    run_batch(8, 1000, increment_task);

    /* Uneven durations exercise work-stealing; all tasks still complete. */
    run_batch(4, 200, slow_task);

    /* Auto worker count and pool reuse across batches. */
    task_pool *pool = task_pool_create(0);
    EXPECT_TRUE(pool != NULL);
    if (pool == NULL)
        return;
    EXPECT_TRUE(task_pool_workers(pool) >= 1);

    atomic_int counter = 0;
    bool submitted = true;
    for (int i = 0; i < 500; i++)
        submitted &= task_pool_submit(pool, increment_task, &counter);
    EXPECT_TRUE(submitted);
    task_pool_wait(pool);
    EXPECT_TRUE(atomic_load(&counter) == 500);

    atomic_store(&counter, 0);
    submitted = true;
    for (int i = 0; i < 300; i++)
        submitted &= task_pool_submit(pool, increment_task, &counter);
    EXPECT_TRUE(submitted);
    task_pool_wait(pool);
    EXPECT_TRUE(atomic_load(&counter) == 300);

    task_pool_destroy(pool);
}
