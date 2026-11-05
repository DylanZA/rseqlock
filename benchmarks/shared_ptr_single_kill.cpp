#include "percpu_shared_ptr.h"

#include <sched.h>
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using bench_clock = std::chrono::steady_clock;

struct item {
  explicit item(std::atomic<bool>& destroyed) : destroyed(destroyed) {}
  ~item() {
    if (destroyed.exchange(true, std::memory_order_release))
      std::abort();
  }
  const std::uint64_t value = 1;
  std::atomic<bool>& destroyed;
};

std::vector<int> allowed_cpus() {
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof(set), &set))
    throw std::runtime_error("sched_getaffinity failed");
  std::vector<int> result;
  for (int cpu = 0; cpu < std::min(CPU_SETSIZE, RSEQ_MUTEX_MAX_CPUS); ++cpu)
    if (CPU_ISSET(cpu, &set))
      result.push_back(cpu);
  if (result.size() < 2)
    throw std::runtime_error("need two allowed CPUs below RSEQ_MUTEX_MAX_CPUS");
  return result;
}

void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof(set), &set))
    throw std::runtime_error("sched_setaffinity failed");
}

struct std_store {
  using pointer = std::shared_ptr<item>;
  std_store(int count, std::atomic<bool>* flags) {
    for (int i = 0; i < count; ++i)
      owners.push_back(std::make_shared<item>(flags[i]));
  }
  pointer reference(int index) const { return owners[index]; }
  void kill(int index) { owners[index].reset(); }
  void collect() {}
  std::vector<pointer> owners;
};

struct percpu_store {
  using pointer = percpu_shared_ptr<item>;
  percpu_store(int count, std::atomic<bool>* flags) : factory(std::nullopt) {
    for (int i = 0; i < count; ++i)
      owners.push_back(factory.make<item>(flags[i]));
  }
  pointer reference(int index) const { return owners[index].weak(); }
  void kill(int index) { owners[index].kill(); }
  void collect() { factory.collect(); }
  // Owners are destroyed before their factory, which then drains survivors.
  percpu_shared_ptr_factory<> factory;
  std::vector<percpu_shared_owner<item>> owners;
};

double percentile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  return values[static_cast<std::size_t>((values.size() - 1) * fraction)];
}

template <class Store>
void run(
    const char* name,
    int threads,
    int items,
    int kills,
    int interval_us,
    int repeat,
    const std::vector<int>& cpus) {
  auto destroyed = std::make_unique<std::atomic<bool>[]>(items);
  for (int i = 0; i < items; ++i)
    destroyed[i].store(false, std::memory_order_relaxed);
  auto store = std::make_unique<Store>(items, destroyed.get());
  std::atomic<int> ready{0}, finished{0}, retired_prefix{0};
  std::atomic<bool> start{false}, stop{false};
  std::vector<std::uint64_t> operations(threads), checksums(threads);
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      pin(cpus[t % (cpus.size() - 1)]); // reserve the last CPU for the killer
      std::vector<typename Store::pointer> references;
      references.reserve(items);
      for (int i = 0; i < items; ++i)
        references.push_back(store->reference(i));
      ++ready;
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      int dropped = 0;
      std::uint64_t count = 0, sum = 0, random = t + 1;
      while (!stop.load(std::memory_order_acquire)) {
        int first = retired_prefix.load(std::memory_order_acquire);
        while (dropped < first)
          references[dropped++].reset();
        for (int batch = 0; batch < 64; ++batch) {
          random ^= random << 13;
          random ^= random >> 7;
          random ^= random << 17;
          auto index = first + random % (items - first);
          auto copy = references[index];
          asm volatile("" : : "r"(copy.get()) : "memory");
          sum += copy->value;
          ++count;
        }
      }
      operations[t] = count;
      checksums[t] = sum;
      finished.fetch_add(1, std::memory_order_release);
    });
  }
  while (ready.load() != threads)
    std::this_thread::yield();
  std::vector<double> kill_times;
  kill_times.reserve(kills);
  std::uint64_t collection_calls = 0;
  auto begin = bench_clock::now();
  start.store(true, std::memory_order_release);
  for (int index = 0; index < kills; ++index) {
    // Allow useful work between kills, with identical pacing for both stores.
    // Never request the next kill until this object's destructor has run.
    std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
    auto kill_begin = bench_clock::now();
    store->kill(index);
    retired_prefix.store(index + 1, std::memory_order_release);
    auto deadline = kill_begin + std::chrono::seconds(10);
    do {
      store->collect();
      ++collection_calls;
      if (!destroyed[index].load(std::memory_order_acquire))
        std::this_thread::yield();
      if (bench_clock::now() > deadline)
        std::abort();
    } while (!destroyed[index].load(std::memory_order_acquire));
    kill_times.push_back(
        std::chrono::duration<double, std::micro>(
            bench_clock::now() - kill_begin)
            .count());
  }
  stop.store(true, std::memory_order_release);
  while (finished.load(std::memory_order_acquire) != threads)
    std::this_thread::yield();
  auto end = bench_clock::now();
  for (auto& worker : workers)
    worker.join();
  std::uint64_t total = 0;
  for (int t = 0; t < threads; ++t) {
    if (!operations[t] || operations[t] != checksums[t])
      throw std::runtime_error("worker made no progress or checksum mismatch");
    total += operations[t];
  }
  for (int i = 0; i < items; ++i)
    if (destroyed[i].load() != (i < kills))
      throw std::runtime_error("unexpected destruction before final drain");
  // Final survivor cleanup is outside all reported timing.
  store.reset();
  for (int i = 0; i < items; ++i)
    if (!destroyed[i].load())
      throw std::runtime_error("object survived final drain");
  double seconds = std::chrono::duration<double>(end - begin).count();
  std::cout
      << name << ',' << threads << ',' << items << ',' << kills << ','
      << interval_us << ',' << repeat << ',' << total << ',' << seconds << ','
      << total / seconds / 1e6 << ',' << seconds * 1e9 / total << ','
      << percentile(kill_times, .5) << ',' << percentile(kill_times, .95) << ','
      << *std::max_element(kill_times.begin(), kill_times.end()) << ','
      << collection_calls << '\n';
}

int positive(const char* text) {
  std::size_t used = 0;
  int value = std::stoi(text, &used);
  if (text[used] || value <= 0)
    throw std::invalid_argument("arguments must be positive integers");
  return value;
}

int main(int argc, char** argv) try {
  if (argc > 6 || (argc > 1 && std::string(argv[1]) == "--help")) {
    std::cerr
        << "Usage: " << argv[0]
        << " [max_threads=16] [items=256] [kills=128] [kill_interval_us=1000] [repeats=3]\n";
    return argc > 6 ? 1 : 0;
  }
  auto cpus = allowed_cpus();
  int maximum = argc > 1 ? positive(argv[1]) : 16;
  int items = argc > 2 ? positive(argv[2]) : 256;
  int kills = argc > 3 ? positive(argv[3]) : 128;
  int interval = argc > 4 ? positive(argv[4]) : 1000;
  int repeats = argc > 5 ? positive(argv[5]) : 3;
  if (kills >= items)
    throw std::invalid_argument("kills must be less than items");
  pin(cpus.back());
  std::cerr
      << "Killer pinned to CPU " << cpus.back() << "; workers share "
      << cpus.size() - 1 << " other CPUs, round-robin. Worker batches check\n"
      << "retirement every 64 touches; each touch copies, reads, releases.\n"
      << "Kill latency includes handle drain and explicit collection;\n"
      << "one object at a time, no replacement, survivors cleaned untimed.\n";
  std::cout
      << "implementation,threads,items,kills,kill_interval_us,repeat,operations,seconds,mops_per_second,aggregate_ns_per_op,kill_p50_us,kill_p95_us,kill_max_us,collection_calls\n"
      << std::fixed << std::setprecision(3);
  for (int threads = 1;;
       threads = threads > maximum / 2 ? maximum : threads * 2) {
    for (int repeat = 0; repeat < repeats; ++repeat) {
      for (int offset = 0; offset < 2; ++offset) {
        if ((offset + repeat) % 2 == 0)
          run<std_store>(
              "std_shared_ptr", threads, items, kills, interval, repeat, cpus);
        else
          run<percpu_store>(
              "percpu_int", threads, items, kills, interval, repeat, cpus);
      }
    }
    if (threads == maximum)
      break;
  }
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
