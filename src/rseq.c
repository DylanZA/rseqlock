#include <sys/rseq.h>

int rseq_mutex_static_init() {
  if (__rseq_size == 0) {
    /* no rseq */
    return -1;
  }
}
