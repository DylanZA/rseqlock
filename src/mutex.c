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
