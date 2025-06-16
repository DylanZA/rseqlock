#include <linux/membarrier.h> /* Definition of MEMBARRIER_* constants */
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <unistd.h>
#include "rseqmutex.h"

int rseq_mutex_init(struct rseq_mutex** mutex) {
  *mutex = (struct rseq_mutex*)malloc(sizeof(struct rseq_mutex));
  memset(*mutex, 0, sizeof(struct rseq_mutex));
  return *mutex != NULL ? 0 : -1;
}

struct rseq_mutex* rseq_mutex_create() {
  struct rseq_mutex* ret;
  if (rseq_mutex_init(&ret)) {
    return NULL;
  } else {
    return ret;
  }
}

void rseq_mutex_free(struct rseq_mutex* mutex) {
  free(mutex);
}

void _rseq_mutex_set_unlocked_with_fence(struct rseq_mutex* mutex, int cpu) {
  atomic_store(&mutex->cpus[cpu].locked, 0);
}

int rseq_mutex_spin_lock_cpu(struct rseq_mutex* mutex, int cpu) {
  struct rseq_mutex_one* this_one = &mutex->cpus[cpu];
  int ret = pthread_mutex_lock(&this_one->other_cpu_mutex);
  if (ret) {
    return -1;
  }

  // ask future rseqs to fail
  atomic_store(&this_one->disabled, 1);

  // make sure that any existing rseq calls are finished after this call
  int64_t res = syscall(
      __NR_membarrier,
      MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
      MEMBARRIER_CMD_FLAG_CPU,
      cpu);
  if (res) {
    atomic_store(&this_one->disabled, 0);
    pthread_mutex_unlock(&this_one->other_cpu_mutex);
    return res;
  }

  // at this point cpu is either locked by another thread or is unlocked and no
  // one else can get at it since `disabled` is true. No threads are in the
  // midst of an rseq attempt to lock this cpu.
  //
  // So now, try get the lock in a spin loop
  while (atomic_exchange_explicit(&this_one->locked, 1, memory_order_acq_rel) ==
         1) {
    if (atomic_load_explicit(&this_one->locked, memory_order_relaxed) == 1) {
      sched_yield();
    }
  }
  return 0;
}

void rseq_mutex_spin_unlock_cpu(struct rseq_mutex* mutex, int cpu) {
  struct rseq_mutex_one* this_one = &mutex->cpus[cpu];
  atomic_store(&this_one->disabled, 0);
  atomic_store(&this_one->locked, 0);
  pthread_mutex_unlock(&this_one->other_cpu_mutex);
}
