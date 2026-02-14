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

static mpmc_queue_t *mpmc_queue_create(size_t capacity) {;
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