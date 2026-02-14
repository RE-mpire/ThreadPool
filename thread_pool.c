/*
 * Lock-free (MPMC) bounded queue thread-pool in C.
 * Uses POSIX threads, semaphores, and C11 atomics.
 *
 * Usage:
 *   pool_t *p = pool_create(4, 1024);
 *   pool_submit(p, my_job, my_arg);
 *   pool_wait(p);
 *   pool_destroy(p, 1);
 *
 * Notes:
 * - If you expect producers to be faster than consumers, configure a larger capacity.
 */
 
#include <stdlib.h>
#include <stdint.h>
#include "thread_pool.h"
#include "mpmc_sem.h"

// internal MPMC queue based on Vyukov's algorithm
typedef struct {
  atomic_size_t seq;
  job_t job;
} node_t;

typedef struct {
  node_t *buffer;    // slot buffer (capacity entries)
  size_t capacity;   // power-of-two capacity
  size_t mask;
  atomic_size_t enqueue_pos;
  atomic_size_t dequeue_pos;
  mpmc_sem_t available;    // counts available jobs
} mpmc_queue_t;

static size_t next_power_of_two(size_t x) {
  if (x <= 2) return 2;
  size_t v = 1;
  while (v < x) v <<= 1;
  return v;
}

static mpmc_queue_t *mpmc_queue_create(size_t capacity) {
  capacity = next_power_of_two(capacity);
  mpmc_queue_t *q = malloc(sizeof(*q));
  if (!q) return NULL;
  q->capacity = capacity;
  q->mask = capacity - 1;
  q->buffer = malloc(sizeof(node_t) * capacity);
  if (!q->buffer) { free(q); return NULL; }
  for (size_t i = 0; i < capacity; ++i) {
    q->buffer[i].seq = i;
    q->buffer[i].job.func = NULL;
    q->buffer[i].job.arg = NULL;
  }
  q->enqueue_pos = 0;
  q->dequeue_pos = 0;
  if (mpmc_sem_init(&q->available, 0) != 0) {
    free(q->buffer);
    free(q);
    return NULL;
  }
  return q;
}

static void mpmc_queue_destroy(mpmc_queue_t *q) {
  if (!q) return;
  mpmc_sem_destroy(&q->available);
  free(q->buffer);
  free(q);
}

// Non-blocking enqueue. Returns 0 on success, -1 if full.
static int mpmc_enqueue_nb(mpmc_queue_t *q, job_t job) {
  size_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
  for (;;) {
    node_t *node = &q->buffer[pos & q->mask];
    size_t seq = atomic_load_explicit(&node->seq, memory_order_acquire);
    intptr_t dif = (intptr_t)seq - (intptr_t)pos;
    if (dif == 0) {
      if (atomic_compare_exchange_weak_explicit(&q->enqueue_pos, &pos, pos + 1,
        memory_order_relaxed, memory_order_relaxed)) {
        // we've reserved the slot
        node->job = job; // copy job
        // publish by setting seq = pos+1
        atomic_store_explicit(&node->seq, pos + 1, memory_order_release);
        // signal availability
        mpmc_sem_post(&q->available);
        return 0;
      }
      // CAS failed - pos updated to new value by CAS; loop with that pos
    } else if (dif < 0) {
        return -1;  // queue is full
    } else {
      pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
    }
  }
}

// Blocking enqueue with exponential backoff. Returns 0 on success, -1 on error.
static int mpmc_enqueue_blocking(mpmc_queue_t *q, job_t job) {
  int spin = 1;
  while (1) {
    if (mpmc_enqueue_nb(q, job) == 0) return 0;
    // queue full - backoff
    if (spin < 32) {
      for (int i = 0; i < (1 << spin); ++i) SPIN_HINT();
      ++spin;
    } else {
      sched_yield();
    }
  }
  return -1;
}

// Blocking dequeue that waits on semaphore. Returns 0 on success and fills job.
// Returns -1 if interrupted by shutdown (the caller should check pool state separately).
static int mpmc_dequeue_wait(mpmc_queue_t *q, job_t *out_job) {
  // wait for available count
  if (mpmc_sem_wait(&q->available) != 0) return -1;
  size_t pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
  for (;;) {
    node_t *node = &q->buffer[pos & q->mask];
    size_t seq = atomic_load_explicit(&node->seq, memory_order_acquire);
    size_t dif = seq - (pos + 1);
    if (dif == 0) {
      if (atomic_compare_exchange_weak_explicit(&q->dequeue_pos, &pos, pos + 1,
        memory_order_relaxed, memory_order_relaxed)) {
        // we've reserved the slot
        *out_job = node->job; // copy
        // mark slot as free for producers: seq = pos + capacity
        atomic_store_explicit(&node->seq, pos + q->capacity, memory_order_release);
        return 0;
      }
    } else {
      pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
    }
  }
  return -1;
}

/* ---------------- Thread Pool ---------------- */

struct pool {
  mpmc_queue_t *q;
  pthread_t *threads;
  size_t n_threads;
  atomic_int running;     // 1 = running, 0 = stopping
  atomic_int accepting;   // 1 = accept new jobs
  atomic_size_t busy;     // number of workers currently executing jobs
  atomic_size_t queued;   // number of jobs enqueued but not yet completed
};

static void *worker(void *arg) {
  pool_t *pool = (pool_t *)arg;
  while (1) {
    job_t job;
    if (mpmc_dequeue_wait(pool->q, &job) != 0) {
      // sem_wait error; check if pool is stopping
      if (!atomic_load_explicit(&pool->running, memory_order_acquire))
        break;
      continue;
    }

    // Poison pill (NULL function) indicates shutdown request
    if (job.func == NULL) break;

    // Mark this worker as busy
    atomic_fetch_add_explicit(&pool->busy, 1, memory_order_acq_rel);

    // Execute the job
    job.func(job.arg);

    // Mark done
    atomic_fetch_sub_explicit(&pool->busy, 1, memory_order_acq_rel);
    atomic_fetch_sub_explicit(&pool->queued, 1, memory_order_release);
  }
  return NULL;
}

pool_t *pool_create(size_t num_threads, size_t capacity) {
  pool_t *pool = malloc(sizeof(*pool));
  if (!pool) return NULL;

  pool->q = mpmc_queue_create(capacity);
  if (!pool->q) { 
    free(pool); 
    return NULL;
  }

  pool->n_threads = num_threads;
  pool->threads = malloc(sizeof(pthread_t) * num_threads);
  if (!pool->threads) { 
    mpmc_queue_destroy(pool->q);
    free(pool);
    return NULL;
  }
  
  atomic_init(&pool->running, 1);
  atomic_init(&pool->accepting, 1);
  atomic_init(&pool->busy, 0);
  atomic_init(&pool->queued, 0);
  for (size_t i = 0; i < num_threads; ++i) {
    pthread_create(&pool->threads[i], NULL, worker, pool);
  }
  return pool;
}

void pool_destroy(pool_t *pool, int wait_for_jobs) {
  if (!pool) return;

  // Stop accepting new submissions immediately
  atomic_store_explicit(&pool->accepting, 0, memory_order_release);

  if (wait_for_jobs) {
    // Wait efficiently until all queued jobs finish
    while (atomic_load_explicit(&pool->queued, memory_order_acquire) > 0 ||
         atomic_load_explicit(&pool->busy, memory_order_acquire) > 0) {
    }
  }

  // Enqueue one poison-pill per worker to ensure each thread wakes and exits.
  // Use blocking enqueue so we ensure the poison pills are actually placed.
  job_t poison = { .func = NULL, .arg = NULL };
  for (size_t i = 0; i < pool->n_threads; ++i) {
    // This will block until there's space. Since we either waited for the queue to
    // drain (wait_for_jobs) or we have stopped accepting new submissions, this will
    // succeed in a finite time.
    (void)mpmc_enqueue_blocking(pool->q, poison);
  }

  // Now mark running = 0 (workers will exit when they dequeue poison)
  atomic_store_explicit(&pool->running, 0, memory_order_release);

  // Join threads
  for (size_t i = 0; i < pool->n_threads; ++i) pthread_join(pool->threads[i], NULL);

  free(pool->threads);
  mpmc_queue_destroy(pool->q);
  free(pool);
}


int pool_submit(pool_t *pool, job_fn fn, void *arg) {
  if (!atomic_load_explicit(&pool->accepting, memory_order_acquire)) return -1;

  job_t job = { .func = fn, .arg = arg };
  int ret = mpmc_enqueue_nb(pool->q, job);
  if (ret == 0)
    atomic_fetch_add_explicit(&pool->queued, 1, memory_order_relaxed);
  return ret;
}

int pool_submit_blocking(pool_t *pool, job_fn fn, void *arg) {
  if (!atomic_load_explicit(&pool->accepting, memory_order_acquire)) return -1;

  job_t job = { .func = fn, .arg = arg };
  int ret = mpmc_enqueue_blocking(pool->q, job);
  if (ret == 0)
    atomic_fetch_add_explicit(&pool->queued, 1, memory_order_relaxed);
  return ret;
}

void pool_wait(pool_t *pool) {
  while (atomic_load_explicit(&pool->queued, memory_order_acquire) > 0 ||
       atomic_load_explicit(&pool->busy, memory_order_acquire) > 0) {
  }
}