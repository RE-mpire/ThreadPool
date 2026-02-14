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

#ifndef LOCKLESS_JOB_POOL_H
#define LOCKLESS_JOB_POOL_H

#include <stddef.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <pthread.h>

// Platform-specific spin hint
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  #define SPIN_HINT() __asm__ volatile("pause" ::: "memory")
#elif defined(__aarch64__) || defined(__arm__)
  #define SPIN_HINT() __asm__ volatile("yield" ::: "memory")
#else
  #define SPIN_HINT() do { } while (0)
#endif

typedef void (*job_fn)(void *arg);

typedef struct {
  job_fn func;
  void *arg;
} job_t;

// Opaque pool type
typedef struct pool pool_t;

// Create a pool with `num_threads` worker threads and queue capacity `capacity`.
// Capacity must be > 1 and will be rounded up to the next power of two.
// Returns NULL on allocation failure.
pool_t *pool_create(size_t num_threads, size_t capacity);

// Destroy the pool. If `wait_for_jobs` is non-zero, the pool will finish all
// queued jobs before returning. If zero, worker threads are asked to stop
// immediately (some jobs may not run).
void pool_destroy(pool_t *pool, int wait_for_jobs);

// Submit a job non-blocking. Returns 0 on success, -1 if queue is full or pool not running.
int pool_submit(pool_t *pool, job_fn fn, void *arg);

// Submit a job but block until there is space. Returns 0 on success, -1 on error.
int pool_submit_blocking(pool_t *pool, job_fn fn, void *arg);

// Wait until all currently queued jobs are finished.
void pool_wait(pool_t *pool);

#endif // LOCKLESS_JOB_POOL_H