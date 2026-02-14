# Non-Blocking Thread Pool
A fast efficient MPMC non-blocking thread pool library using C11 intrinsics.

Internally uses MPMC queue to store jobs

## Basic usage
``` c
#include "thread_pool.h"

pool_t* pool = pool_create(4); // create a thread pool with 4 threads

void my_task(void* arg) {
    // do some work
}

// submit a task to the pool
pool_submit(pool, my_task, NULL);

// destroy the pool and wait for all tasks to finish
pool_destroy(pool); 
```

## Testing

The project includes comprehensive tests for both the internal MPMC queue and the thread pool API.

### Run all tests:
```bash
make tests
```

### Run individual tests:
```bash
make test_mpmc       # Test the MPMC queue implementation
make test_thread_pool # Test the thread pool API
```

Test coverage includes:
- MPMC queue: capacity rounding, FIFO ordering, wraparound stability, full-queue detection, multi-producer/multi-consumer stress tests
- Thread pool: create/destroy, single and multiple job execution, queue-full semantics, blocking submit, concurrent job execution, concurrent producers, graceful shutdown