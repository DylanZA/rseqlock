#include <rseqmutex.h>

int lock(struct rseq_mutex* mutex) {
  int cpu;
  cpu = rseq_mutex_try_lock(mutex);
  return cpu;
}
