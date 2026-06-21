#include <errno.h>
#include <linux/membarrier.h> /* Definition of MEMBARRIER_* constants */
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <unistd.h>
#include "rseqmutex.h"

int rseq_mutex_init(struct rseq_mutex** mutex) {
  int ret = posix_memalign(
      (void**)mutex, RSEQ_MUTEX_CPU_ALIGN, sizeof(struct rseq_mutex));
  if (ret) {
    *mutex = NULL;
    return -ret;
  }
  memset(*mutex, 0, sizeof(struct rseq_mutex));
  return 0;
}

struct rseq_mutex* rseq_mutex_create(void) {
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

void _rseq_mutex_set_unlocked_with_barrier(struct rseq_mutex* mutex, int cpu) {
  // use SEQ_CST as I believe this will force a compiler barrier
  __atomic_fetch_and(
      &mutex->cpus[cpu].lock_storage,
      ~(rseq_mutex_atomic_u64)_rseq_mutex_storage_lock_value,
      __ATOMIC_SEQ_CST);
}

int rseq_mutex_spin_lock_cpu(struct rseq_mutex* mutex, int cpu) {
  if (!_rseq_mutex_cpu_in_range(cpu)) {
    return -EINVAL;
  }
  struct rseq_mutex_one* this_one = &mutex->cpus[cpu];
  int ret = pthread_mutex_lock(&this_one->other_cpu_mutex);
  if (ret) {
    return -1;
  }

  // ask future locks to fail
  __atomic_fetch_or(
      &this_one->lock_storage,
      _rseq_mutex_storage_disabled_value,
      __ATOMIC_RELEASE);

  // make sure that any existing rseq calls are finished after this call
  long res = syscall(
      __NR_membarrier,
      MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
      MEMBARRIER_CMD_FLAG_CPU,
      cpu);
  if (res) {
    __atomic_fetch_and(
        &this_one->lock_storage,
        ~(rseq_mutex_atomic_u64)_rseq_mutex_storage_disabled_value,
        __ATOMIC_RELEASE);
    pthread_mutex_unlock(&this_one->other_cpu_mutex);
    return -errno;
  }

  // at this point cpu is either locked by another thread or is unlocked and no
  // one else can get at it since `disabled` is true. No threads are in the
  // midst of an rseq attempt to lock this cpu.
  //
  // So now, try get the lock in a spin loop
  do {
    rseq_mutex_atomic_u64 expected = _rseq_mutex_storage_disabled_value;
    if (__atomic_compare_exchange_n(
            &this_one->lock_storage,
            (void*)&expected,
            _rseq_mutex_storage_locked_and_disabled,
            false /* not weak */,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {
      break;
    }
    while (__atomic_load_n(&this_one->lock_storage, __ATOMIC_RELAXED) ==
           expected) {
      sched_yield();
    }
  } while (true);

  // at this point the core is locked, however the processor might have
  // not actually committed its state before unlocking the core.
  // Force a barrier on that core to get everything synced up
  // (I do not think this is needed on x86 since stores are not
  // reordered)
  //
  // Use the RSEQ membarrier call as that is the only one that allows
  // a cpu argument. It definitely does do a memory barrier (smp_mb) before it
  // does it's rseq business, so it will work.
  // One day if something like
  // https://lore.kernel.org/all/20250626155257.81256-1-dyudaken@gmail.com/
  // lands then can shift to that.
  res = syscall(
      __NR_membarrier,
      MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
      MEMBARRIER_CMD_FLAG_CPU,
      cpu);
  if (res) {
    rseq_mutex_spin_unlock_cpu(mutex, cpu);
    return -errno;
  }

  return 0;
}

void rseq_mutex_spin_unlock_cpu(struct rseq_mutex* mutex, int cpu) {
  struct rseq_mutex_one* this_one = &mutex->cpus[cpu];
  __atomic_store_n(&this_one->lock_storage, 0, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&this_one->other_cpu_mutex);
}
