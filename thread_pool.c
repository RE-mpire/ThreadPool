#include <stdlib.h>
#include "thread_pool.h"

// internal MPMC queue based on Vyukov's algorithm
typedef struct {
  atomic_size_t seq;
  job_t job;
} node_t;

typedef struct {
  node_t *buffer;    // slot buffer (capacity entries)
  size_t capacity;    // power-of-two capacity
  size_t mask;
  atomic_size_t enqueue_pos;
  atomic_size_t dequeue_pos;
  sem_t available;    // counts available jobs
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
  sem_init(&q->available, 0, 0);
  return q;
}

static void mpmc_queue_destroy(mpmc_queue_t *q) {
  if (!q) return;
  sem_destroy(&q->available);
  free(q->buffer);
  free(q);
}

// Non-blocking enqueue. Returns 0 on success, -1 if full.
static int mpmc_enqueue_nb(mpmc_queue_t *q, job_t job) {
  size_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
  for (;;) {
    node_t *node = &q->buffer[pos & q->mask];
    size_t seq = atomic_load_explicit(&node->seq, memory_order_acquire);
    size_t dif = seq - pos;
    if (dif == 0) {
      if (atomic_compare_exchange_weak_explicit(&q->enqueue_pos, &pos, pos + 1,
        memory_order_relaxed, memory_order_relaxed)) {
        // we've reserved the slot
        node->job = job; // copy job
        // publish by setting seq = pos+1
        atomic_store_explicit(&node->seq, pos + 1, memory_order_release);
        // signal availability
        sem_post(&q->available);
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
      for (int i = 0; i < (1 << spin); ++i) __asm__ volatile("pause" ::: "memory");
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
  if (sem_wait(&q->available) != 0) return -1;
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