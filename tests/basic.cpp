#include <rseqmutex.h>
#include <sched.h>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {
int const kMaxCores = 64;

void check(bool x, std::string place = {}) {
  if (!x) {
    if (!place.empty()) {
      std::cerr << "die: " << place << " \n";
    }
    std::exit(-1);
  }
}

struct Free {
  void operator()(struct rseq_mutex* m) { rseq_mutex_free(m); }
};

struct Checker {
  struct CacheLine {
    volatile uint64_t counter = 0;
    std::array<unsigned char, 128> foo;
  };
  std::unique_ptr<struct rseq_mutex, Free> mutex =
      std::unique_ptr<struct rseq_mutex, Free>(rseq_mutex_create());
  std::array<CacheLine, kMaxCores> cores;
  std::mutex start_mutex;

  void one(int thread) {
    int cpu;
    do {
      cpu = rseq_mutex_try_lock(mutex.get());
    } while (cpu < 0);

    auto& line = cores[cpu];
    int was = line.counter;
    for (int i = 1; i < 10000; i++) {
      line.counter = i;
    }
    line.counter = 0;
    rseq_mutex_release(mutex.get(), cpu);
    check(was == 0);
  }

  void check_invariants(int cpu) {
    int ret = rseq_mutex_spin_lock_cpu(mutex.get(), cpu);
    check(ret == 0, "lock success");
    check(cores[cpu].counter == 0, "counter == 0");
    rseq_mutex_spin_unlock_cpu(mutex.get(), cpu);
  }
};

} // namespace

int main(int argc, char* argv[]) {
  check(rseq_mutex_static_init() == 0, "static init");
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  int const kThreads = std::thread::hardware_concurrency() * 4;
  int const kIter = 500000;
  std::vector<std::thread> threads;
  Checker c;
  {
    std::lock_guard<std::mutex> guard(c.start_mutex);
    for (int i = 0; i < kThreads; i++) {
      threads.emplace_back([&c, kIter, i, end]() mutable {
        {
          std::lock_guard<std::mutex> guard(c.start_mutex);
        }
        while (true) {
          for (int j = 0; j < kIter; j++) {
            c.one(i);
          }
          break;
        }
      });
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  for (int j = 0; j < kMaxCores; j++) {
    c.check_invariants(j);
  }

  for (auto& t : threads) {
    t.join();
  }
  std::cerr << "Done\n";
  return 0;
}
