#include <linux/membarrier.h> /* Definition of MEMBARRIER_* constants */
#include <stdio.h>
#include <sys/rseq.h>
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <unistd.h>

int rseq_mutex_static_init() {
  if (__rseq_size == 0) {
    /* no rseq */
    return -1;
  }

  int ret = syscall(
      __NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ, 0, 0);
  if (ret) {
    return -1;
  }

  return 0;
}
