#pragma once
#include <stdlib.h>
#include <string.h>
#include <sys/rseq.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline struct rseq* rseq_mutex_get_rseq_area() {
  return (struct rseq*)((ptrdiff_t)__builtin_thread_pointer() + __rseq_offset);
}

struct rseq_mutex;

int rseq_mutex_static_init();

int rseq_mutex_init(struct rseq_mutex** mutex);
struct rseq_mutex* rseq_mutex_create();
void rseq_mutex_free(struct rseq_mutex* mutex);

struct rseq_mutex_one {
  union {
    volatile int64_t locked;
    char buffer[128];
  };
};

struct rseq_mutex {
  struct rseq_mutex_one cpus[128]; /* unknown */
};

/* copied from librseq rseq_load_cbne_store__ptr after running through a
 * preprocessor so I did not have to add a dependency. see
 * https://github.com/compudj/librseq/blob/0fda052/include/rseq/arch/x86/bits.h#L22
 */
static int _rseq_mutex_lock_inner_new(
    intptr_t* v, intptr_t expect, intptr_t newv, int cpu) {
  __asm__ __volatile__ goto(
      ".pushsection __rseq_cs, \"aw\"\n\t"
      ".balign 32\n\t"
      "3"
      ":\n\t"
      ".long "
      "0x0"
      "\n\t"
      ".long "
      "0x0"
      "\n\t"
      ".quad "
      "1f"
      "\n\t"
      ".quad "
      "(2f) - (1f)"
      "\n\t"
      ".quad "
      "4f"
      "\n\t"
      ".long "
      "0x0"
      "\n\t"
      ".long "
      "0x0"
      "\n\t"
      ".quad "
      "1f"
      "\n\t"
      ".quad "
      "(2f) - (1f)"
      "\n\t"
      ".quad "
      "4f"
      "\n\t"
      ".popsection\n\t"

      ".pushsection __rseq_cs_ptr_array, \"aw\"\n\t"
      ".quad 3b\n\t"
      ".popsection\n\t"
      ".pushsection __rseq_exit_point_array, \"aw\"\n\t"
      ".quad 1f\n\t"
      ".quad %l[ne]\n\t"
      ".popsection\n\t"

      "leaq 3b(%%rip), %%rax\n\t"
      "movq %%rax, %%fs:8(%[rseq_offset])\n\t"
      "1:\n\t"
      "cmpl %[cpu_id], %%fs:4(%[rseq_offset])\n\t"
      "jnz 4f\n\t"

      "cmpq %[v], %[expect]\n\t"
      "jne %l[ne]\n\t"

      "movq %[newv], %[v]\n\t"
      "2:\n\t"

      ".pushsection __rseq_failure, \"ax\"\n\t"
      ".byte 0x0f, 0xb9, 0x3d\n\t"
      ".long 0x53053053\n\t"
      "4:\n\t"
      "jmp %l[abort]\n\t"
      ".popsection\n\t"
      :
      : [cpu_id] "r"(cpu),
        [rseq_offset] "r"(__rseq_offset),
        [v] "m"(*v),
        [expect] "r"(expect),
        [newv] "r"(newv)
      : "memory", "cc", "rax"

      : abort, ne

  );
  __asm__ __volatile__("" : : : "memory");
  return 0;
abort:
  __asm__ __volatile__("" : : : "memory");

  return -1;
ne:
  __asm__ __volatile__("" : : : "memory");
  return 1;
}

static inline int _rseq_mutex_lock_inner(struct rseq_mutex* mutex, int* cpu) {
  volatile int* cpu_id_start_ptr =
      (volatile int*)&rseq_mutex_get_rseq_area()->cpu_id_start;
  int cpu_id = *cpu_id_start_ptr;
  int64_t* value = (int64_t*)&mutex->cpus[cpu_id].locked;
  int64_t expected = 0;
  int64_t ret = _rseq_mutex_lock_inner_new(value, 0, 1, cpu_id);
  if (__builtin_expect(ret, 0) == 0) {
    *cpu = cpu_id;
    return 0; /* ok */
  } else if (ret == 1) {
    *cpu = -1;
    return 0; /* locked */
  } else {
    return -1; /* aborted - try again */
  }
}

/**
   Lock an rseq mutex
   returns -1 if this cpu was locked by another thread, or else the cpu id that
   it is locked to
*/
inline int rseq_mutex_try_lock(struct rseq_mutex* mutex) {
  int cpu;
  while (_rseq_mutex_lock_inner(mutex, &cpu) != 0)
    ;
  return cpu;
}

/** Release an rseq mutex
    DO NOT call if the result of rseq_mutex_try_lock was -1
*/
inline __attribute__((always_inline)) void rseq_mutex_release(
    struct rseq_mutex* mutex, int cpu) {
  mutex->cpus[cpu].locked = 0;
}

#ifdef __cplusplus
} // extern "C"
#endif
