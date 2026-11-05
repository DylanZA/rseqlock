#include "percpu_shared_ptr.h"

#include <sched.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using clock_type = std::chrono::steady_clock;

struct payload {
  explicit payload(std::uint64_t value) : value(value) {}
  std::uint64_t value;
};

// Keep a real copy/release pair in the measured loop, including std::shared_ptr
// in optimized builds. Neither the pointee nor this escape needs a shared RMW.
template <class Pointer>
inline void escape(const Pointer& pointer) {
  asm volatile("" : : "r"(pointer.get()) : "memory");
}

std::vector<int> available_cpus() {
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof(set), &set))
    throw std::runtime_error("sched_getaffinity failed");
  std::vector<int> cpus;
  for (int cpu = 0; cpu < std::min(CPU_SETSIZE, RSEQ_MUTEX_MAX_CPUS); ++cpu)
    if (CPU_ISSET(cpu, &set))
      cpus.push_back(cpu);
  if (cpus.empty())
    throw std::runtime_error(
        "benchmark needs an allowed CPU below RSEQ_MUTEX_MAX_CPUS");
  return cpus;
}

struct std_factory {
  static constexpr bool has_main_owner = false;
  auto make(std::uint64_t value) { return std::make_shared<payload>(value); }
  void finish() {}
};

template <class TCounter>
struct percpu_factory {
  static constexpr bool has_main_owner = true;
  struct pointer {
    explicit pointer(percpu_shared_owner<payload, TCounter>&& value)
        : owner(std::move(value)), reference(owner.weak()) {}
    // The root carries the main owner; copies carry counted weak handles only.
    pointer(const pointer& other) : reference(other.reference) {}
    pointer(pointer&&) noexcept = default;
    payload* get() const { return reference.get(); }
    payload* operator->() const { return get(); }
    percpu_shared_owner<payload, TCounter> owner;
    percpu_weak_ptr<payload, TCounter> reference;
  };
  explicit percpu_factory(int interval)
      : factory(
            std::make_unique<percpu_shared_ptr_factory<TCounter>>(
                interval ? std::optional<std::chrono::milliseconds>(
                               std::chrono::minutes(interval))
                         : std::nullopt)) {}
  auto make(std::uint64_t value) {
    return pointer(factory->template make<payload>(value));
  }
  void fold() { factory->collect(); }
  void finish() { factory.reset(); }
  std::unique_ptr<percpu_shared_ptr_factory<TCounter>> factory;
};

template <class Factory>
void run(
    Factory& factory,
    const char* name,
    const std::string& workload,
    int threads,
    int iterations,
    int repeat,
    int interval,
    const std::vector<int>& cpus) {
  using pointer = decltype(factory.make(1));
  std::vector<pointer> roots;
  if (workload == "copy_shared" || workload == "copy_killed")
    roots.push_back(factory.make(1));
  else if (workload == "copy_distinct")
    for (int t = 0; t < threads; ++t)
      roots.push_back(factory.make(1));

  if constexpr (Factory::has_main_owner) {
    if (workload == "copy_killed") {
      roots.front().owner.kill();
      factory.fold(); // transition cost is outside this atomic-only copy loop
    }
  }

  std::atomic<int> ready{0}, done{0};
  std::atomic<bool> start{false};
  std::vector<std::uint64_t> sums(threads);
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(cpus[t % cpus.size()], &set);
      if (sched_setaffinity(0, sizeof(set), &set))
        std::abort();
      ready.fetch_add(1);
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      std::uint64_t sum = 0;
      if (workload == "churn") {
        for (int i = 0; i < iterations; ++i) {
          auto pointer = factory.make(1);
          escape(pointer);
          sum += pointer->value;
        }
      } else {
        const auto& source = roots[workload == "copy_distinct" ? t : 0];
        for (int i = 0; i < iterations; ++i) {
          auto copy = source;
          escape(copy);
          sum += copy->value;
        }
      }
      sums[t] = sum;
      done.fetch_add(1, std::memory_order_release);
    });
  }
  while (ready.load() != threads)
    std::this_thread::yield();
  auto begin = clock_type::now();
  start.store(true, std::memory_order_release);
  while (done.load(std::memory_order_acquire) != threads)
    std::this_thread::yield();
  auto end = clock_type::now();
  for (auto& worker : workers)
    worker.join();
  for (auto sum : sums)
    if (sum != static_cast<std::uint64_t>(iterations))
      throw std::runtime_error("incorrect benchmark checksum");

  roots.clear();
  auto drain_begin = clock_type::now();
  factory.finish();
  auto drain_end = clock_type::now();
  double elapsed =
      std::chrono::duration<double, std::nano>(end - begin).count();
  double drain =
      std::chrono::duration<double, std::nano>(drain_end - drain_begin).count();
  // Allocation churn includes the final deferred deletion cost. Copy workloads
  // report cleanup separately because pointee creation/deletion is not timed.
  double measured = elapsed + (workload == "churn" ? drain : 0);
  double operations = double(threads) * iterations;
  std::cout << name << ',' << workload << ',' << threads << ',' << iterations
            << ',' << repeat << ',' << interval << ',' << measured / operations
            << ',' << operations * 1e9 / measured << ',' << drain << '\n';
}

int number(const char* text, bool allow_zero = false) {
  std::size_t used = 0;
  int value = std::stoi(text, &used);
  if (text[used] || value < (allow_zero ? 0 : 1))
    throw std::invalid_argument(
        "arguments must be positive integers (collect_minutes may be zero)");
  return value;
}

int main(int argc, char** argv) try {
  if (argc > 5 || (argc > 1 && std::string(argv[1]) == "--help")) {
    std::cerr
        << "Usage: " << argv[0]
        << " [max_threads=16] [iterations=5000000] [repeats=3] [collect_minutes=1]\n"
        << "Threads: powers of two plus max_threads; churn is capped at 2000\n"
        << "iterations/thread. collect_minutes=0 selects manual collection.\n";
    return argc > 5 ? 1 : 0;
  }
  auto cpus = available_cpus();
  int maximum = argc > 1 ? number(argv[1]) : std::min<int>(16, cpus.size());
  int iterations = argc > 2 ? number(argv[2]) : 5000000;
  int repeats = argc > 3 ? number(argv[3]) : 3;
  int interval = argc > 4 ? number(argv[4], true) : 1;
  std::cerr
      << "Workers pinned round-robin to " << cpus.size()
      << " allowed CPUs; counts above that oversubscribe.\n"
      << "ns_per_op is wall time / total operations (one copy+release or\n"
      << "make+release). Churn includes final drain; drain_ns is also reported.\n";
  std::cout
      << "implementation,workload,threads,iterations,repeat,collect_minutes,ns_per_op,ops_per_second,drain_ns\n"
      << std::fixed << std::setprecision(2);
  for (int threads = 1;;
       threads = threads > maximum / 2 ? maximum : threads * 2) {
    for (const std::string workload :
         {"copy_shared", "copy_distinct", "copy_killed", "churn"}) {
      int cycles =
          workload == "churn" ? std::min(iterations, 2000) : iterations;
      for (int repeat = 0; repeat < repeats; ++repeat) {
        // Rotate execution order to reduce systematic warmup/thermal bias.
        for (int offset = 0; offset < 4; ++offset) {
          int implementation = (offset + repeat) % 4;
          switch (implementation) {
            case 0: {
              std_factory factory;
              run(factory,
                  "std_shared_ptr",
                  workload,
                  threads,
                  cycles,
                  repeat,
                  0,
                  cpus);
              break;
            }
            case 1: {
              percpu_factory<std::int16_t> factory(interval);
              run(factory,
                  "percpu_int16",
                  workload,
                  threads,
                  cycles,
                  repeat,
                  interval,
                  cpus);
              break;
            }
            case 2: {
              percpu_factory<int> factory(interval);
              run(factory,
                  "percpu_int",
                  workload,
                  threads,
                  cycles,
                  repeat,
                  interval,
                  cpus);
              break;
            }
            case 3: {
              percpu_factory<std::int64_t> factory(interval);
              run(factory,
                  "percpu_int64",
                  workload,
                  threads,
                  cycles,
                  repeat,
                  interval,
                  cpus);
              break;
            }
          }
        }
      }
    }
    if (threads == maximum)
      break;
  }
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
