#include <rseqmutex.h>

void unlock(struct rseq_mutex* mutex) {
  rseq_mutex_release(mutex, 1);
}
