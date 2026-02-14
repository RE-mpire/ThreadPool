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