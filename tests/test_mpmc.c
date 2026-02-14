#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>

// Include implementation to access internal MPMC queue APIs for testing.
#include "../thread_pool.c"

#define TEST_ASSERT(cond, msg) do { \
  if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    exit(1); \
  } \
} while (0)

static void dummy_job(void *arg) {
  (void)arg;
}

static void test_capacity_rounding(void) {
  mpmc_queue_t *q = mpmc_queue_create(3);
  TEST_ASSERT(q != NULL, "queue create");
  TEST_ASSERT(q->capacity == 4, "capacity rounds to power of two");
  TEST_ASSERT(q->mask == 3, "mask matches capacity");
  mpmc_queue_destroy(q);
}

static void test_basic_fifo_and_full(void) {
  mpmc_queue_t *q = mpmc_queue_create(4);
  TEST_ASSERT(q != NULL, "queue create");

  for (size_t i = 0; i < 4; ++i) {
    job_t job = { .func = dummy_job, .arg = (void *)(uintptr_t)(i + 1) };
    TEST_ASSERT(mpmc_enqueue_nb(q, job) == 0, "enqueue ok");
  }

  job_t extra = { .func = dummy_job, .arg = NULL };
  TEST_ASSERT(mpmc_enqueue_nb(q, extra) == -1, "queue reports full");

  for (size_t i = 0; i < 4; ++i) {
    job_t out;
    TEST_ASSERT(mpmc_dequeue_wait(q, &out) == 0, "dequeue ok");
    TEST_ASSERT(out.func == dummy_job, "function preserved");
    TEST_ASSERT((size_t)(uintptr_t)out.arg == i + 1, "fifo order");
  }

  TEST_ASSERT(mpmc_enqueue_nb(q, extra) == 0, "enqueue works after dequeue");
  job_t out;
  TEST_ASSERT(mpmc_dequeue_wait(q, &out) == 0, "dequeue after reuse");

  mpmc_queue_destroy(q);
}

static void test_wraparound_stability(void) {
  mpmc_queue_t *q = mpmc_queue_create(2);
  TEST_ASSERT(q != NULL, "queue create");

  for (size_t i = 0; i < 10000; ++i) {
    job_t job = { .func = dummy_job, .arg = (void *)(uintptr_t)i };
    TEST_ASSERT(mpmc_enqueue_nb(q, job) == 0, "enqueue ok");
    job_t out;
    TEST_ASSERT(mpmc_dequeue_wait(q, &out) == 0, "dequeue ok");
    TEST_ASSERT((size_t)(uintptr_t)out.arg == i, "wraparound preserves order");
  }

  mpmc_queue_destroy(q);
}

typedef struct {
  mpmc_queue_t *q;
  size_t producer_id;
  size_t per_producer;
} producer_args_t;

typedef struct {
  mpmc_queue_t *q;
  atomic_int *seen;
  size_t total;
  atomic_size_t *consumed;
} consumer_args_t;

static void *producer_thread(void *arg) {
  producer_args_t *pa = (producer_args_t *)arg;
  size_t base = pa->producer_id * pa->per_producer;
  for (size_t i = 0; i < pa->per_producer; ++i) {
    size_t id = base + i;
    job_t job = { .func = dummy_job, .arg = (void *)(uintptr_t)id };
    while (mpmc_enqueue_nb(pa->q, job) != 0) {
      sched_yield();
    }
  }
  return NULL;
}

static void *consumer_thread(void *arg) {
  consumer_args_t *ca = (consumer_args_t *)arg;
  while (1) {
    job_t job;
    if (mpmc_dequeue_wait(ca->q, &job) != 0) {
      continue;
    }
    if (job.func == NULL) {
      break;
    }
    size_t id = (size_t)(uintptr_t)job.arg;
    TEST_ASSERT(id < ca->total, "job id in range");
    atomic_fetch_add_explicit(&ca->seen[id], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(ca->consumed, 1, memory_order_relaxed);
  }
  return NULL;
}

static void test_mpmc_concurrency(void) {
  const size_t producers = 4;
  const size_t consumers = 3;
  const size_t per_producer = 10000;
  const size_t total = producers * per_producer;

  mpmc_queue_t *q = mpmc_queue_create(64);
  TEST_ASSERT(q != NULL, "queue create");

  atomic_int *seen = calloc(total, sizeof(*seen));
  TEST_ASSERT(seen != NULL, "seen alloc");
  for (size_t i = 0; i < total; ++i) {
    atomic_init(&seen[i], 0);
  }

  atomic_size_t consumed;
  atomic_init(&consumed, 0);

  pthread_t prod_threads[producers];
  pthread_t cons_threads[consumers];
  producer_args_t prod_args[producers];
  consumer_args_t cons_args[consumers];

  for (size_t i = 0; i < consumers; ++i) {
    cons_args[i].q = q;
    cons_args[i].seen = seen;
    cons_args[i].total = total;
    cons_args[i].consumed = &consumed;
    pthread_create(&cons_threads[i], NULL, consumer_thread, &cons_args[i]);
  }

  for (size_t i = 0; i < producers; ++i) {
    prod_args[i].q = q;
    prod_args[i].producer_id = i;
    prod_args[i].per_producer = per_producer;
    pthread_create(&prod_threads[i], NULL, producer_thread, &prod_args[i]);
  }

  for (size_t i = 0; i < producers; ++i) {
    pthread_join(prod_threads[i], NULL);
  }

  for (size_t i = 0; i < consumers; ++i) {
    job_t stop = { .func = NULL, .arg = NULL };
    while (mpmc_enqueue_nb(q, stop) != 0) {
      sched_yield();
    }
  }

  for (size_t i = 0; i < consumers; ++i) {
    pthread_join(cons_threads[i], NULL);
  }

  TEST_ASSERT(atomic_load_explicit(&consumed, memory_order_relaxed) == total,
    "all jobs consumed");

  for (size_t i = 0; i < total; ++i) {
    int count = atomic_load_explicit(&seen[i], memory_order_relaxed);
    if (count != 1) {
      fprintf(stderr, "FAIL: job %zu seen %d times\n", i, count);
      exit(1);
    }
  }

  free(seen);
  mpmc_queue_destroy(q);
}

int main(void) {
  printf("Running mpmc queue tests ...\n");
  test_capacity_rounding();
  test_basic_fifo_and_full();
  test_wraparound_stability();
  test_mpmc_concurrency();
  printf("OK: mpmc queue tests passed\n");
  return 0;
}
