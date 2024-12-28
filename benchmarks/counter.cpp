#include <dlfcn.h>
#include <rseqmutex.h>
#include <sched.h>
#include <array>
#include <atomic>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

namespace {

struct GetCpuVdso {
  typedef int (*Vdso)(unsigned int* cpu, unsigned int* node, void* _);
  GetCpuVdso() {
    void* h = dlopen("linux-vdso.so.1", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    void* fun = dlsym(h, "__vdso_getcpu");
    if (!fun) {
      throw std::runtime_error("No vdso getcpu");
    }
    fun_ = (Vdso)fun;
  }
  GetCpuVdso(GetCpuVdso const&) = delete;
  GetCpuVdso(GetCpuVdso&&) = delete;
  Vdso fun_;
  uint64_t operator()() {
    unsigned int cpu;
    unsigned int node;
    if (fun_(&cpu, &node, NULL)) {
      return 0;
    }
    return uint64_t(cpu) << node;
  }
};

struct RseqRead {
  uint64_t operator()() { return rseq_mutex_get_rseq_area()->cpu_id; }
};

struct GetCpu {
  uint64_t operator()() {
    unsigned int cpu;
    unsigned int node;
    if (getcpu(&cpu, &node)) {
      return 0;
    }
    return uint64_t(cpu) << node;
  }
};

struct SchedGetCpu {
  uint64_t operator()() { return sched_getcpu(); }
};

struct CpuCache { // copied from facebook::folly
  uint64_t count = -1;
  uint64_t cpu = 0;
};
static thread_local CpuCache cpuCache;

template <class TUnderlying>
struct CachedGetCpu {
  TUnderlying getCpu;
  uint64_t operator()() {
    if (cpuCache.count++ <= 1000) {
      return cpuCache.cpu;
    } else {
      cpuCache.count = 0;
      cpuCache.cpu = getCpu();
      return cpuCache.cpu;
    }
  }
};

template <class TCpuGetter>
struct CleverAtomicRunner {
  void operator()() {
    auto cpu = cpuGetter();
    cores[cpu].value.fetch_add(1, std::memory_order_relaxed);
  }

  uint64_t get() const {
    return std::accumulate(
        cores.begin(),
        cores.end(),
        uint64_t(0),
        [](uint64_t val, CacheLine const& v) {
          return v.value.load(std::memory_order_relaxed) + val;
        });
  }

  TCpuGetter cpuGetter;
  struct CacheLine {
    std::atomic<uint64_t> value{0};
    std::array<unsigned char, 128> foo; /* cache line separator */
  };
  std::array<CacheLine, 64> cores;
};

struct RseqAtomicRunner {
  void operator()() {
    int cpu = rseq_mutex_try_lock(mutex.get());
    if (cpu >= 0) {
      cores[cpu].value++;
      rseq_mutex_release(mutex.get(), cpu);
    } else {
      fallback.fetch_add(1, std::memory_order_relaxed);
    }
  }

  uint64_t get() const {
    return std::accumulate(
               cores.begin(),
               cores.end(),
               uint64_t(0),
               [](uint64_t val, CacheLine const& v) { return v.value + val; }) +
        fallback.load(std::memory_order_relaxed);
  }

  struct Free {
    void operator()(struct rseq_mutex* m) { rseq_mutex_free(m); }
  };
  std::unique_ptr<struct rseq_mutex, Free> mutex =
      std::unique_ptr<struct rseq_mutex, Free>(rseq_mutex_create());
  struct CacheLine {
    uint64_t value{0};
    std::array<unsigned char, 128> foo;
  };
  std::array<CacheLine, 64> cores;
  std::atomic<uint64_t> fallback{0};
};

struct AtomicRunner {
  void operator()() { value.fetch_add(1, std::memory_order_relaxed); }
  uint64_t get() const { return value.load(std::memory_order_relaxed); }
  std::atomic<uint64_t> value{0};
};

template <class TRunner>
std::chrono::nanoseconds runInner(int nthreads, int ncycles, TRunner&& runner) {
  std::vector<std::chrono::nanoseconds> threadTimes;
  threadTimes.resize(nthreads);
  std::vector<std::thread> threads;
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < nthreads; i++) {
    threads.emplace_back([i, ncycles, &threadTimes, &runner]() {
      auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < ncycles; i++) {
        runner();
      }
      auto end = std::chrono::steady_clock::now();
      threadTimes[i] =
          std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  auto ret = runner.get();
  auto end = std::chrono::steady_clock::now();
  uint64_t total = uint64_t(nthreads) * uint64_t(ncycles);
  if (ret != total) {
    std::cerr << "Ret=" << ret << "total=" << total << std::endl;
    throw std::runtime_error("BUG");
  }
  return std::accumulate(
      threadTimes.begin(),
      threadTimes.end(),
      std::chrono::nanoseconds(),
      [](auto a, auto b) { return a + b; });
}

template <class TRunner>
std::chrono::nanoseconds runOne(int nthreads, int ncycles) {
  auto time = runInner(nthreads, ncycles, TRunner{});
  uint64_t total = uint64_t(nthreads) * uint64_t(ncycles);
  auto ret = time.count() / total;
  std::cerr << ret << " ns per cycle. " << std::endl;
  return std::chrono::nanoseconds(ret);
}

template <class TRunner>
std::vector<std::chrono::nanoseconds> run(int nthreads, int ncycles, int n) {
  std::vector<std::chrono::nanoseconds> ret;
  for (int i = 0; i < n; i++) {
    ret.push_back(runOne<TRunner>(nthreads, ncycles));
  }
  return ret;
}

struct TestResult {
  int threads;
  std::string name;
  std::vector<std::chrono::nanoseconds> res;
  template <class TRunner>
  static TestResult Run(std::string name, int threads, int cycles, int iter) {
    TestResult tr;
    tr.name = name;
    tr.threads = threads;
    tr.res = run<TRunner>(threads, cycles, iter);
    return tr;
  }

  static void csvHeader(std::ostream& so) {
    so << "name"
       << ","
       << "threads"
       << ","
       << "ns_per_cycle" << std::endl;
  }

  void csv(std::ostream& so) const {
    for (auto r : res) {
      so << name << "," << threads << "," << r.count() << std::endl;
    }
  }
};

int csvmode() {
  // run a bunch of tests and output a csv so we can make pretty charts
  std::vector<TestResult> res;
  TestResult::csvHeader(std::cout);
  for (int threads = 1; threads <= 256; threads <<= 1) {
    int const kIter = 7;
    int const kThreads = 128;
    std::cerr << "Rseq runner" << std::endl;

    TestResult::Run<RseqAtomicRunner>("rseq", threads, 20000000, kIter)
        .csv(std::cout);
    std::cerr << "Clever Atomic(RseqRead) runner" << std::endl;
    TestResult::Run<CleverAtomicRunner<RseqRead>>(
        "cleverAtomicRseqRead", threads, 20000000, kIter)
        .csv(std::cout);
    std::cerr << "Clever Atomic(GetCpu) runner" << std::endl;
    TestResult::Run<CleverAtomicRunner<GetCpu>>(
        "cleverAtomicGetCpu", threads, 20000000, kIter)
        .csv(std::cout);
    std::cerr << "Clever Atomic(GetCpuVdso) runner" << std::endl;
    TestResult::Run<CleverAtomicRunner<GetCpuVdso>>(
        "cleverAtomicGetCpuVdso", threads, 1000000, kIter)
        .csv(std::cout);
    std::cerr << "Clever Atomic(SchedGetcpu) runner" << std::endl;
    TestResult::Run<CleverAtomicRunner<SchedGetCpu>>(
        "cleverAtomicSchedGetCpu", threads, 20000000, kIter)
        .csv(std::cout);
    std::cerr << "Clever Atomic(CachedGetCpu(Vdso)) runner" << std::endl;
    TestResult::Run<CleverAtomicRunner<CachedGetCpu<GetCpuVdso>>>(
        "cleverAtomicCachedGetCpuVdso", threads, 20000000, kIter)
        .csv(std::cout);
    std::cerr << "Clever Atomic(CachedGetCpu) runner" << std::endl;
    TestResult::Run<CleverAtomicRunner<CachedGetCpu<GetCpu>>>(
        "cleverAtomicCachedGetCpu", threads, 20000000, kIter)
        .csv(std::cout);
    std::cerr << "Atomic runner" << std::endl;
    TestResult::Run<AtomicRunner>("atomic", threads, 500000, kIter)
        .csv(std::cout);
  }

  return 0;
}
} // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "csv") {
    return csvmode();
  }
  int const kIter = 3;
  int const kThreads = 128;
  std::cerr << "Rseq runner" << std::endl;
  run<RseqAtomicRunner>(kThreads, 20000000, kIter);
  std::cerr << "Clever Atomic(RseqRead) runner" << std::endl;
  run<CleverAtomicRunner<RseqRead>>(kThreads, 20000000, kIter);
  std::cerr << "Clever Atomic(GetCpu) runner" << std::endl;
  run<CleverAtomicRunner<GetCpu>>(kThreads, 20000000, kIter);
  std::cerr << "Clever Atomic(GetCpuVdso) runner" << std::endl;
  run<CleverAtomicRunner<GetCpuVdso>>(kThreads, 1000000, kIter);
  std::cerr << "Clever Atomic(SchedGetcpu) runner" << std::endl;
  run<CleverAtomicRunner<SchedGetCpu>>(kThreads, 20000000, kIter);
  std::cerr << "Clever Atomic(CachedGetCpu(Vdso)) runner" << std::endl;
  run<CleverAtomicRunner<CachedGetCpu<GetCpuVdso>>>(kThreads, 20000000, kIter);
  std::cerr << "Clever Atomic(CachedGetCpu) runner" << std::endl;
  run<CleverAtomicRunner<CachedGetCpu<GetCpu>>>(kThreads, 20000000, kIter);
  std::cerr << "Atomic runner" << std::endl;
  run<AtomicRunner>(kThreads, 500000, kIter);
  return 0;
}
