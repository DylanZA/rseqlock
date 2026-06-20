#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rseq.h>

#ifdef __cplusplus
#define _RSEQ_NOEXCEPT noexcept
#else
#define _RSEQ_NOEXCEPT
#endif

typedef volatile int64_t rseq_mutex_atomic_i64;

#ifdef __cplusplus
extern "C" {
#endif

#define _rseq_unlikely(x) __builtin_expect(!!(x), 0)
#define _rseq_likely(x) __builtin_expect(!!(x), 1)
#define RSEQ_MUTEX_CPU_ALIGN 128

static inline struct rseq* rseq_mutex_get_rseq_area(void) {
  return (struct rseq*)((ptrdiff_t)__builtin_thread_pointer() + __rseq_offset);
}

static inline void _compiler_barrier(void) {
  __asm__ __volatile__("" : : : "memory");
}

struct rseq_mutex;

int rseq_mutex_static_init(void) _RSEQ_NOEXCEPT;

int rseq_mutex_init(struct rseq_mutex** mutex) _RSEQ_NOEXCEPT;
struct rseq_mutex* rseq_mutex_create(void) _RSEQ_NOEXCEPT;
void rseq_mutex_free(struct rseq_mutex* mutex) _RSEQ_NOEXCEPT;

struct rseq_mutex_one {
  rseq_mutex_atomic_i64 locked;
  rseq_mutex_atomic_i64 disabled;
  pthread_mutex_t other_cpu_mutex;
  char buffer[RSEQ_MUTEX_CPU_ALIGN - sizeof(rseq_mutex_atomic_i64) * 2 -
              sizeof(pthread_mutex_t)];
} __attribute__((aligned(RSEQ_MUTEX_CPU_ALIGN)));

#ifdef __cplusplus
static_assert(sizeof(struct rseq_mutex_one) == RSEQ_MUTEX_CPU_ALIGN,
              "unexpected rseq mutex CPU slot size");
static_assert(alignof(struct rseq_mutex_one) == RSEQ_MUTEX_CPU_ALIGN,
              "unexpected rseq mutex CPU slot alignment");
#else
_Static_assert(sizeof(struct rseq_mutex_one) == RSEQ_MUTEX_CPU_ALIGN,
               "unexpected rseq mutex CPU slot size");
_Static_assert(_Alignof(struct rseq_mutex_one) == RSEQ_MUTEX_CPU_ALIGN,
               "unexpected rseq mutex CPU slot alignment");
#endif

struct rseq_mutex {
  struct rseq_mutex_one cpus[128]; /* unknown */
};

/* copied from librseq rseq_load_cbne_store__ptr after running through a
 * preprocessor so I did not have to add a dependency. see
 * https://github.com/compudj/librseq/blob/0fda052/include/rseq/arch/x86/bits.h#L22
 * (with my addition of  the disabled pointer)
 */
static int _rseq_mutex_lock_inner_new(
    rseq_mutex_atomic_i64* disabled,
    rseq_mutex_atomic_i64* v,
    int64_t expect,
    int64_t newv,
    int cpu)
    _RSEQ_NOEXCEPT {
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

      // if disabled is set then dont do anything
      "cmpq $0, %[disabled]\n\t"
      "jnz %l[ne]\n\t"
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
        [disabled] "m"(*disabled),
        [v] "m"(*v),
        [expect] "r"(expect),
        [newv] "r"(newv)
      : "memory", "cc", "rax"

      : abort, ne

  );
  _compiler_barrier();
  return 0;
abort:
  _compiler_barrier();

  return -1;
ne:
  _compiler_barrier();
  return 1;
}

static inline int _read_cpu_id_start(void) _RSEQ_NOEXCEPT {
  volatile int* cpu_id_start_ptr =
      (volatile int*)&rseq_mutex_get_rseq_area()->cpu_id_start;
  return *cpu_id_start_ptr;
}

static inline int _rseq_mutex_lock_inner(struct rseq_mutex* mutex, int* cpu)
    _RSEQ_NOEXCEPT {
  int cpu_id = _read_cpu_id_start();
  rseq_mutex_atomic_i64* value = &mutex->cpus[cpu_id].locked;
  rseq_mutex_atomic_i64* disabled = &mutex->cpus[cpu_id].disabled;
  int64_t ret = _rseq_mutex_lock_inner_new(disabled, value, 0, 1, cpu_id);
  if (_rseq_likely(ret == 0)) {
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
   returns -1 if this cpu was locked by another thread, or else the cpu id
   that it is locked to
*/
static inline int rseq_mutex_try_lock(struct rseq_mutex* mutex) _RSEQ_NOEXCEPT {
  int cpu;
  while (_rseq_mutex_lock_inner(mutex, &cpu) != 0)
    ;
  return cpu;
}

void _rseq_mutex_set_unlocked_with_barrier(struct rseq_mutex* mutex, int cpu)
    _RSEQ_NOEXCEPT;

/** Release an rseq mutex
    DO NOT call if the result of rseq_mutex_try_lock was -1
*/
static inline void rseq_mutex_release(struct rseq_mutex* mutex, int cpu)
    _RSEQ_NOEXCEPT {
  if (_rseq_unlikely(cpu != _read_cpu_id_start())) {
    /*
     * Have to do an atomic set here, so that previous stores are not reordered
     * later. Only a problem for the slow path where we were rescheduled to
     * another core, as in that case another cpu may grab the lock and not see
     * those writes, which violates the mutex invariants.
     */
    _rseq_mutex_set_unlocked_with_barrier(mutex, cpu);
  } else {
    /* Compiler barrier is needed. CPU barrier is enforced if reading from
     * a different cpu.
     */
    _compiler_barrier();
    mutex->cpus[cpu].locked = 0;
  }
}

/**
   Lock an rseq mutex for a given CPU
   This will spin until it is able to lock
   returns -1 on error, 0 on success
*/
int rseq_mutex_spin_lock_cpu(struct rseq_mutex* mutex, int cpu) _RSEQ_NOEXCEPT;

/**
   Unlock an rseq mutex for a given CPU, after having had a successful
   call to rseq_mutex_spin_lock_cpu
*/
void rseq_mutex_spin_unlock_cpu(struct rseq_mutex* mutex, int cpu)
    _RSEQ_NOEXCEPT;

#undef _rseq_unlikely
#undef _rseq_likely
#ifdef __cplusplus
} // extern "C"
#endif

#undef _RSEQ_NOEXCEPT
#undef _rseq_unlikely
#undef _rseq_likely
