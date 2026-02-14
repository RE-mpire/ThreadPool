#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

// Include implementation to access internal APIs and types.
#include "../thread_pool.c"

#define TEST_ASSERT(cond, msg) do { \
  if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    exit(1); \
  } \
} while (0)

typedef struct {
  atomic_int counter;
  atomic_int executions;
} test_context_t;

static void increment_job(void *arg) {
  atomic_int *counter = (atomic_int *)arg;
  atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static void context_job(void *arg) {
  test_context_t *ctx = (test_context_t *)arg;
  atomic_fetch_add_explicit(&ctx->executions, 1, memory_order_relaxed);
  usleep(1000);  // 1ms to simulate work
}

static void test_pool_create_destroy(void) {
  pool_t *p = pool_create(4, 16);
  TEST_ASSERT(p != NULL, "pool create");
  pool_destroy(p, 0);
}

static void test_single_job(void) {
  pool_t *p = pool_create(2, 16);
  TEST_ASSERT(p != NULL, "pool create");

  atomic_int counter;
  atomic_init(&counter, 0);

  TEST_ASSERT(pool_submit(p, increment_job, &counter) == 0, "submit job");
  pool_wait(p);
  TEST_ASSERT(atomic_load_explicit(&counter, memory_order_relaxed) == 1, "job executed");

  pool_destroy(p, 1);
}

static void test_multiple_jobs(void) {
  pool_t *p = pool_create(4, 32);
  TEST_ASSERT(p != NULL, "pool create");

  atomic_int counter;
  atomic_init(&counter, 0);

  const int num_jobs = 100;
  for (int i = 0; i < num_jobs; ++i) {
    TEST_ASSERT(pool_submit(p, increment_job, &counter) == 0, "submit job");
  }

  pool_wait(p);
  int final = atomic_load_explicit(&counter, memory_order_relaxed);
  TEST_ASSERT(final == num_jobs, "all jobs executed");

  pool_destroy(p, 1);
}

static void test_queue_full_nonblocking(void) {
  pool_t *p = pool_create(1, 4);  // small capacity
  TEST_ASSERT(p != NULL, "pool create");

  atomic_int counter;
  atomic_init(&counter, 0);

  // Fill the queue
  for (int i = 0; i < 4; ++i) {
    int ret = pool_submit(p, increment_job, &counter);
    TEST_ASSERT(ret == 0, "submit to fill queue");
  }

  // Next one should fail (queue full)
  int ret = pool_submit(p, increment_job, &counter);
  TEST_ASSERT(ret == -1, "queue full returns -1");

  pool_wait(p);
  int final = atomic_load_explicit(&counter, memory_order_relaxed);
  TEST_ASSERT(final == 4, "only queued jobs executed");

  pool_destroy(p, 1);
}

static void test_submit_blocking(void) {
  pool_t *p = pool_create(1, 2);  // small capacity
  TEST_ASSERT(p != NULL, "pool create");

  atomic_int counter;
  atomic_init(&counter, 0);

  // Fill the queue
  for (int i = 0; i < 2; ++i) {
    int ret = pool_submit(p, increment_job, &counter);
    TEST_ASSERT(ret == 0, "submit to fill queue");
  }

  // blocking submit should succeed (will wait internally)
  int ret = pool_submit_blocking(p, increment_job, &counter);
  TEST_ASSERT(ret == 0, "blocking submit succeeds");

  pool_wait(p);
  int final = atomic_load_explicit(&counter, memory_order_relaxed);
  TEST_ASSERT(final == 3, "all submitted jobs executed");

  pool_destroy(p, 1);
}

static void test_concurrent_submits(void) {
  pool_t *p = pool_create(4, 256);
  TEST_ASSERT(p != NULL, "pool create");

  test_context_t ctx;
  atomic_init(&ctx.counter, 0);
  atomic_init(&ctx.executions, 0);

  const int num_jobs = 200;
  for (int i = 0; i < num_jobs; ++i) {
    TEST_ASSERT(pool_submit(p, context_job, &ctx) == 0, "submit job");
  }

  pool_wait(p);
  int final = atomic_load_explicit(&ctx.executions, memory_order_relaxed);
  TEST_ASSERT(final == num_jobs, "all concurrent jobs executed");

  pool_destroy(p, 1);
}

typedef struct {
  pool_t *pool;
  atomic_int *counter;
  int count;
} producer_args_t;

static void *producer_thread(void *arg) {
  producer_args_t *pa = (producer_args_t *)arg;
  for (int i = 0; i < pa->count; ++i) {
    while (pool_submit(pa->pool, increment_job, pa->counter) != 0) {
      usleep(1000);
    }
  }
  return NULL;
}

static void test_concurrent_producers(void) {
  pool_t *p = pool_create(4, 64);
  TEST_ASSERT(p != NULL, "pool create");

  atomic_int counter;
  atomic_init(&counter, 0);

  const int num_producers = 4;
  const int jobs_per_producer = 100;
  pthread_t threads[num_producers];
  producer_args_t args[num_producers];

  for (int i = 0; i < num_producers; ++i) {
    args[i].pool = p;
    args[i].counter = &counter;
    args[i].count = jobs_per_producer;
    pthread_create(&threads[i], NULL, producer_thread, &args[i]);
  }

  for (int i = 0; i < num_producers; ++i) {
    pthread_join(threads[i], NULL);
  }

  pool_wait(p);
  int final = atomic_load_explicit(&counter, memory_order_relaxed);
  int expected = num_producers * jobs_per_producer;
  TEST_ASSERT(final == expected, "all jobs from concurrent producers executed");

  pool_destroy(p, 1);
}

static void test_destroy_without_wait(void) {
  pool_t *p = pool_create(2, 16);
  TEST_ASSERT(p != NULL, "pool create");

  atomic_int counter;
  atomic_init(&counter, 0);

  // Submit jobs but destroy without waiting
  for (int i = 0; i < 10; ++i) {
    pool_submit(p, increment_job, &counter);
  }

  // This should force-stop workers
  pool_destroy(p, 0);

  // Some (not necessarily all) jobs may have executed, but we shouldn't crash
  // Just verify the counter is reasonable
  int final = atomic_load_explicit(&counter, memory_order_relaxed);
  TEST_ASSERT(final >= 0 && final <= 10, "counter in expected range");
}

int main(void) {
  test_pool_create_destroy();
  test_single_job();
  test_multiple_jobs();
  test_queue_full_nonblocking();
  test_submit_blocking();
  test_concurrent_submits();
  test_concurrent_producers();
  test_destroy_without_wait();
  printf("OK: thread pool tests passed\n");
  return 0;
}
