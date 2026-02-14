
/*
 * Semaphore wrapper for MPMC queue. 
 */

#ifndef MPMC_SEM_H
#define MPMC_SEM_H

#include <errno.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>

// On macOS, unnamed semaphores are not supported.
#ifdef __APPLE__
#include <fcntl.h>
#include <unistd.h>

typedef struct {
  sem_t *sem;
  char name[64];
} mpmc_sem_t;

static atomic_uint mpmc_sem_name_counter = 0;

static inline int mpmc_sem_init(mpmc_sem_t *s, unsigned int value) {
  unsigned int id = atomic_fetch_add_explicit(&mpmc_sem_name_counter, 1, memory_order_relaxed);
  for (int attempt = 0; attempt < 16; ++attempt) {
    snprintf(s->name, sizeof(s->name), "/mpmcq_%d_%u_%d", (int)getpid(), id, attempt);
    sem_t *sem = sem_open(s->name, O_CREAT | O_EXCL, 0600, value);
    if (sem != SEM_FAILED) {
      s->sem = sem;
      return 0;
    }
    if (errno != EEXIST) break;
  }
  s->sem = NULL;
  s->name[0] = '\0';
  return -1;
}

static inline void mpmc_sem_destroy(mpmc_sem_t *s) {
  if (!s || !s->sem) return;
  sem_close(s->sem);
  sem_unlink(s->name);
  s->sem = NULL;
  s->name[0] = '\0';
}

static inline int mpmc_sem_post(mpmc_sem_t *s) { return sem_post(s->sem); }
static inline int mpmc_sem_wait(mpmc_sem_t *s) { return sem_wait(s->sem); }

#else

typedef sem_t mpmc_sem_t;

static inline int mpmc_sem_init(mpmc_sem_t *s, unsigned int value) {
  return sem_init(s, 0, value);
}

static inline void mpmc_sem_destroy(mpmc_sem_t *s) { sem_destroy(s); }
static inline int mpmc_sem_post(mpmc_sem_t *s) { return sem_post(s); }
static inline int mpmc_sem_wait(mpmc_sem_t *s) { return sem_wait(s); }

#endif

#endif // MPMC_SEM_H
