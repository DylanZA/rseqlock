#include "percpu_shared_ptr.h"

#include <sched.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <iostream>
#include <vector>

using namespace std::chrono_literals;

// Count actual public remote-lock calls, including those in header templates.
// This verifies that candidate scans and post-fold cleanup do not lock cores.
std::atomic<unsigned> remote_locks{0};
std::atomic<int> fail_cpu{-1};
thread_local unsigned held_remote_locks = 0;
extern "C" int __real_rseq_mutex_spin_lock_cpu(rseq_mutex*, int) noexcept;
extern "C" void __real_rseq_mutex_spin_unlock_cpu(rseq_mutex*, int) noexcept;
extern "C" int __wrap_rseq_mutex_spin_lock_cpu(
    rseq_mutex* mutex, int cpu) noexcept {
  assert(held_remote_locks == 0); // folding must never hold two CPU locks
  remote_locks.fetch_add(1, std::memory_order_relaxed);
  int expected = cpu;
  if (fail_cpu.compare_exchange_strong(expected, -1))
    return -EIO;
  int error = __real_rseq_mutex_spin_lock_cpu(mutex, cpu);
  if (!error)
    ++held_remote_locks;
  return error;
}
extern "C" void __wrap_rseq_mutex_spin_unlock_cpu(
    rseq_mutex* mutex, int cpu) noexcept {
  assert(held_remote_locks == 1);
  --held_remote_locks;
  __real_rseq_mutex_spin_unlock_cpu(mutex, cpu);
}

struct tracked {
  explicit tracked(std::atomic<int>& destroyed, int value = 42)
      : destroyed(destroyed), value(value) {}
  ~tracked() { ++destroyed; }
  std::atomic<int>& destroyed;
  int value;
};

struct affinity {
  affinity() { assert(sched_getaffinity(0, sizeof(saved), &saved) == 0); }
  ~affinity() { assert(sched_setaffinity(0, sizeof(saved), &saved) == 0); }
  void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    assert(sched_setaffinity(0, sizeof(set), &set) == 0);
  }
  std::vector<int> cpus() const {
    std::vector<int> result;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
      if (CPU_ISSET(cpu, &saved))
        result.push_back(cpu);
    return result;
  }
  cpu_set_t saved;
};

void contended_local_lock() {
  // Hold a core lock remotely while a handle on that core tries to increment.
  // Access to internals here deliberately forces the busy-lock fallback.
  using namespace percpu_shared_detail;
  affinity mask;
  int cpu = mask.cpus().front();
  if (cpu >= RSEQ_MUTEX_MAX_CPUS)
    return;
  assert(rseq_mutex_static_init() == 0);
  std::unique_ptr<rseq_mutex, free_mutex> mutex(rseq_mutex_create());
  assert(mutex);
  slab<int> slab(mutex.get());
  auto& entry = slab.slots[0];
  entry.state.store(slot_state::active, std::memory_order_relaxed);
  entry.fallback = 1;
  std::atomic<bool> started{false}, done{false};
  std::thread worker;
  {
    remote_guard guard(mutex.get(), cpu);
    worker = std::thread([&] {
      affinity worker_mask;
      worker_mask.pin(cpu);
      started = true;
      change<1>(&entry);
      assert(entry.fallback == 2);
      change<-1>(&entry);
      done = true;
    });
    while (!started)
      std::this_thread::yield();
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(done); // must complete via the atomic even while the core is locked
    assert(entry.fallback == 1);
    assert(slab.cores[cpu].counters[0] == 0);
  }
  worker.join();
  assert(done && entry.fallback == 1);
}

void owner_lifecycle_and_locking() {
  static_assert(!std::is_copy_constructible_v<percpu_shared_owner<int>>);
  static_assert(std::is_nothrow_move_constructible_v<percpu_shared_owner<int>>);
  using entry_type = percpu_shared_detail::slot<int>;
  static_assert(alignof(entry_type) == percpu_shared_detail::cache_line_size);
  static_assert(
      offsetof(entry_type, state) % percpu_shared_detail::cache_line_size == 0);
  static_assert(
      sizeof(entry_type) - offsetof(entry_type, state) >=
      percpu_shared_detail::cache_line_size);
  static_assert(offsetof(entry_type, fallback) < offsetof(entry_type, state));

  std::atomic<int> destroyed{0};
  percpu_shared_ptr_factory<int> factory(std::nullopt);
  auto owner = factory.make<tracked>(destroyed, 17);
  auto weak = owner.weak();
  auto moved = std::move(owner);
  assert(!owner && moved->value == 17);
  auto before = remote_locks.load();
  assert(factory.collect() == 0);
  assert(factory.collect() == 0);
  assert(remote_locks == before); // live owner: flags only, no core locks

  moved.kill();
  moved.kill(); // idempotent; drop the owner's reference exactly once
  assert(!moved && !moved.weak());
  assert(weak->value == 17);
  assert(factory.collect() == 0); // fold while the weak handle is still alive
  assert(remote_locks == before + RSEQ_MUTEX_MAX_CPUS);
  before = remote_locks;
  auto copy = weak; // remains counted, exclusively atomic after kill
  weak.reset();
  assert(factory.collect() == 0 && destroyed == 0);
  assert(copy->value == 17);
  copy.reset();
  assert(factory.collect() == 1 && destroyed == 1);
  assert(remote_locks == before); // folded counters never need locking again

  // Slot reuse must clear kill/folded/pending state and restore the live owner.
  {
    auto reused = factory.make<tracked>(destroyed, 23);
    auto first = factory.make<tracked>(destroyed, 24);
    assert(factory.collect() == 0);
    assert(remote_locks == before);
    first = std::move(reused); // move assignment kills the previous owner
    assert(!reused && first->value == 23);
  } // destructor kills the remaining owner
  assert(factory.collect() == 2 && destroyed == 3);
  assert(remote_locks == before + RSEQ_MUTEX_MAX_CPUS);

  // Each newly killed object is folded in the very next collect() call.
  auto first_kill = factory.make<tracked>(destroyed);
  auto next_kill = factory.make<tracked>(destroyed);
  first_kill.kill();
  before = remote_locks;
  assert(factory.collect() == 1 && destroyed == 4);
  next_kill.kill();
  assert(factory.collect() == 1 && destroyed == 5);
  assert(remote_locks == before + 2 * RSEQ_MUTEX_MAX_CPUS);
}

void owner_transition_race() {
  affinity mask;
  auto cpus = mask.cpus();
  std::atomic<int> destroyed{0};
  percpu_shared_ptr_factory<std::int8_t> factory(std::nullopt);
  auto owner = factory.make<tracked>(destroyed);
  auto root = owner.weak();
  // Force signed deltas and half-range flushes before switching modes.
  mask.pin(cpus.front());
  std::vector<percpu_weak_ptr<tracked, std::int8_t>> batch(600, root);
  mask.pin(cpus.back());
  batch.clear();
  std::atomic<int> ready{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) {
    workers.emplace_back([&, t] {
      affinity pinned;
      pinned.pin(cpus[t % cpus.size()]);
      ++ready;
      while (!stop.load()) {
        auto first = root;
        auto second = first;
        assert(second->value == 42);
      }
    });
  }
  while (ready != 4)
    std::this_thread::yield();
  owner.kill();
  auto before = remote_locks.load();
  assert(factory.collect() == 0); // fold while updates race via the atomic
  assert(remote_locks == before + RSEQ_MUTEX_MAX_CPUS);
  before = remote_locks;
  for (int cycle = 0; cycle < 10; ++cycle)
    assert(factory.collect() == 0);
  assert(remote_locks == before && destroyed == 0);
  stop = true;
  for (auto& worker : workers)
    worker.join();
  root.reset();
  assert(factory.collect() == 1 && destroyed == 1);
  assert(remote_locks == before);
}

void owner_scheduled_and_shutdown() {
  std::atomic<int> destroyed{0};
  {
    percpu_shared_ptr_factory<int> factory(1ms);
    auto owner = factory.make<tracked>(destroyed);
    owner.kill();
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!destroyed && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(1ms);
    assert(destroyed == 1);
  }
  struct parent {
    explicit parent(percpu_shared_owner<tracked> child)
        : child(std::move(child)) {}
    percpu_shared_owner<tracked> child;
  };
  {
    percpu_shared_ptr_factory<int> factory(std::nullopt);
    factory.make<parent>(factory.make<tracked>(destroyed));
  } // parent destruction kills child; shutdown must drain both transitions
  assert(destroyed == 2);
}

void owner_fold_accounting() {
  using namespace percpu_shared_detail;
  affinity mask;
  auto cpus = mask.cpus();
  std::unique_ptr<rseq_mutex, free_mutex> mutex(rseq_mutex_create());
  assert(mutex);
  slab<std::int8_t> slab(mutex.get());
  auto& entry = slab.slots[0];
  entry.state.store(slot_state::active, std::memory_order_relaxed);
  entry.fallback = 1; // the main owner
  mask.pin(cpus.front());
  for (int i = 0; i < 600; ++i)
    change<1>(&entry);
  mask.pin(cpus.back());
  for (int i = 0; i < 599; ++i)
    change<-1>(&entry);
  entry.state.store(slot_state::dying, std::memory_order_release);
  entry.fallback.fetch_sub(1, std::memory_order_release); // kill the owner

  std::array<std::int8_t, RSEQ_MUTEX_MAX_CPUS> snapshot;
  for (int cpu = 0; cpu < RSEQ_MUTEX_MAX_CPUS; ++cpu)
    snapshot[cpu] = slab.cores[cpu].counters[0].load(std::memory_order_relaxed);
  for (int i = 0; i < 1000; ++i) {
    change<1>(&entry);
    change<-1>(&entry);
  }
  for (int cpu = 0; cpu < RSEQ_MUTEX_MAX_CPUS; ++cpu)
    assert(slab.cores[cpu].counters[0] == snapshot[cpu]);
  for (int cpu = 0; cpu < RSEQ_MUTEX_MAX_CPUS; ++cpu) {
    std::int8_t delta;
    {
      remote_guard guard(mutex.get(), cpu);
      delta = drain_cpu(entry, cpu);
    }
    assert(held_remote_locks == 0);
    entry.fallback.fetch_add(delta, std::memory_order_acq_rel);
  }
  assert(entry.fallback == 1);
  change<1>(&entry);
  assert(entry.fallback == 2);
  change<-1>(&entry);
  change<-1>(&entry);
  assert(entry.fallback == 0);
  for (auto& line : slab.cores)
    assert(line.counters[0] == 0);
}

template <class TCounter>
void half_range_flush() {
  using namespace percpu_shared_detail;
  affinity mask;
  int cpu = mask.cpus().front();
  if (cpu >= RSEQ_MUTEX_MAX_CPUS)
    return;
  mask.pin(cpu);
  std::unique_ptr<rseq_mutex, free_mutex> mutex(rseq_mutex_create());
  assert(mutex);
  slab<TCounter> block(mutex.get());
  auto& entry = block.slots[0];
  entry.state.store(slot_state::active, std::memory_order_relaxed);
  auto& count = block.cores[cpu].counters[0];
  constexpr std::int64_t threshold =
      std::numeric_limits<TCounter>::max() / 2 + 1;

  // Stop one increment short of the positive boundary, then cross it.
  entry.fallback = 1;
  count = static_cast<TCounter>(threshold - 2);
  change<1>(&entry);
  assert(count == threshold - 1 && entry.fallback == 1);
  change<1>(&entry);
  assert(count == 0 && entry.fallback == threshold + 1);
  change<1>(&entry); // resume local counting rather than staying on fallback
  assert(count == 1 && entry.fallback == threshold + 1);

  // Mirror the transfer for releases on another CPU's negative local balance.
  entry.fallback = threshold + 1;
  count = static_cast<TCounter>(-threshold + 2);
  change<-1>(&entry);
  assert(count == -threshold + 1 && entry.fallback == threshold + 1);
  change<-1>(&entry);
  assert(count == 0 && entry.fallback == 1);
  change<-1>(&entry);
  assert(count == -1 && entry.fallback == 1);
}

void slab_reclamation() {
  std::atomic<int> destroyed{0};
  percpu_shared_ptr_factory<int> factory(std::nullopt);
  constexpr auto slots = factory.slots_per_slab;
  constexpr auto initial = slots * 3 + 1;
  using handle = decltype(factory.template make<tracked>(destroyed));
  std::vector<handle> values;
  assert(factory.slab_count() == 0);
  for (std::size_t i = 0; i < initial; ++i)
    values.push_back(
        factory.template make<tracked>(destroyed, static_cast<int>(i)));
  assert(factory.slab_count() == 4);
  auto release_block =
      [&](std::size_t begin, std::size_t end, std::size_t remaining) {
        for (auto i = begin; i < end; ++i)
          values[i].reset();
        auto before = remote_locks.load();
        assert(factory.collect() == end - begin);
        assert(remote_locks == before + RSEQ_MUTEX_MAX_CPUS);
        assert(factory.slab_count() == remaining);
      };
  // Unlink a middle slab, then the oldest and newest ones. The newest is
  // partially empty, exercising removal from the available-slab list too.
  release_block(slots, 2 * slots, 3);
  release_block(0, slots, 2);
  release_block(3 * slots, initial, 1);
  for (auto i = 2 * slots; i < 3 * slots; ++i)
    assert(values[i]->value == static_cast<int>(i));

  values[2 * slots].reset();
  assert(factory.collect() == 1);
  values[2 * slots] = factory.template make<tracked>(destroyed, 1000);
  assert(factory.slab_count() == 1); // use the free slot in the retained slab
  auto extra = factory.template make<tracked>(destroyed);
  assert(factory.slab_count() == 2); // retained slab was full again
  extra.reset();
  assert(factory.collect() == 1 && factory.slab_count() == 1);
  assert(values[2 * slots]->value == 1000);
  values.clear();
  assert(factory.collect() == slots);
  assert(
      factory.slab_count() == 0 && destroyed == static_cast<int>(initial + 2));
  factory.template make<tracked>(
      destroyed); // allocate after completely draining
  assert(factory.slab_count() == 1);
  assert(factory.collect() == 1 && factory.slab_count() == 0);
}

void slab_pinned_during_destruction() {
  struct blocking_destructor {
    blocking_destructor(
        std::atomic<bool>& entered,
        std::atomic<bool>& release,
        std::atomic<int>& destroyed)
        : entered(entered), release(release), destroyed(destroyed) {}
    ~blocking_destructor() {
      entered = true;
      while (!release.load())
        std::this_thread::yield();
      ++destroyed;
    }
    std::atomic<bool>& entered;
    std::atomic<bool>& release;
    std::atomic<int>& destroyed;
  };
  std::atomic<bool> entered{false}, release{false};
  std::atomic<int> destroyed{0};
  percpu_shared_ptr_factory<int> factory(std::nullopt);
  factory.template make<blocking_destructor>(entered, release, destroyed);
  std::thread collector([&] { assert(factory.collect() == 1); });
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!entered && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  assert(entered && factory.slab_count() == 1);
  std::atomic<bool> second_started{false}, second_done{false};
  std::thread second_collector([&] {
    second_started = true;
    assert(factory.collect() == factory.slots_per_slab);
    assert(release.load()); // cannot finish before the first destructor returns
    second_done = true;
  });
  while (!second_started.load())
    std::this_thread::yield();

  using handle = decltype(factory.template make<tracked>(destroyed));
  std::vector<handle> values;
  for (std::size_t i = 0; i < factory.slots_per_slab; ++i)
    values.push_back(factory.template make<tracked>(destroyed));
  assert(factory.slab_count() == 2); // the retired slot cannot be reused yet
  values.clear();
  assert(!second_done.load());
  assert(factory.slab_count() == 2);
  release = true;
  collector.join();
  second_collector.join();
  assert(second_done.load());
  assert(factory.slab_count() == 0);
  assert(destroyed == static_cast<int>(factory.slots_per_slab + 1));
}

void interrupted_fold() {
  std::atomic<int> destroyed{0};
  percpu_shared_ptr_factory<> factory(std::nullopt);
  std::vector<percpu_weak_ptr<tracked>> references;
  const auto count = factory.slots_per_slab * 2 + 1;
  for (std::size_t i = 0; i < count; ++i) {
    auto owner = factory.make<tracked>(destroyed);
    references.push_back(owner.weak());
  }
  fail_cpu = RSEQ_MUTEX_MAX_CPUS / 2;
  bool failed = false;
  try {
    factory.collect();
  } catch (const std::system_error&) {
    failed = true;
  }
  assert(failed && fail_cpu == -1 && held_remote_locks == 0);
  assert(destroyed == 0 && factory.slab_count() == 3);
  for (const auto& reference : references)
    assert(reference->value == 42);
  auto before = remote_locks.load();
  assert(
      factory.collect() == 0); // retry the whole batch without double counting
  assert(remote_locks == before + RSEQ_MUTEX_MAX_CPUS);
  references.clear();
  before = remote_locks;
  assert(factory.collect() == count && factory.slab_count() == 0);
  assert(destroyed == static_cast<int>(count));
  assert(remote_locks == before); // completed folds never need more locking
}

void interval_validation() {
  for (auto interval : {0ms, -1ms}) {
    bool rejected = false;
    try {
      percpu_shared_ptr_factory<> factory(interval);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);
  }
  percpu_shared_ptr_factory<> manual(std::nullopt);
  manual.make<int>(42);
  assert(manual.collect() == 1);
}

void concurrent_make_and_collect() {
  percpu_shared_ptr_factory<> factory(std::nullopt);
  std::atomic<int> destroyed{0}, ready{0};
  std::atomic<bool> start{false}, done{false};
  constexpr int makers = 4;
  constexpr int rounds = 8;
  constexpr int batch_size = 32;
  std::thread collector([&] {
    while (!done.load()) {
      factory.collect();
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> threads;
  for (int t = 0; t < makers; ++t) {
    threads.emplace_back([&, t] {
      ++ready;
      while (!start.load())
        std::this_thread::yield();
      for (int round = 0; round < rounds; ++round) {
        std::vector<percpu_shared_owner<tracked>> owners;
        for (int i = 0; i < batch_size; ++i) {
          auto owner = factory.make<tracked>(destroyed, t * batch_size + i);
          auto reference = owner.weak();
          assert(reference->value == t * batch_size + i);
          owners.push_back(std::move(owner));
        }
        for (int i = 0; i < batch_size; ++i)
          assert(owners[i]->value == t * batch_size + i);
      }
    });
  }
  while (ready != makers)
    std::this_thread::yield();
  start = true;
  for (auto& thread : threads)
    thread.join();
  done = true;
  collector.join();
  factory.collect();
  assert(destroyed == makers * rounds * batch_size);
  assert(factory.slab_count() == 0);
}

void coalesced_kills() {
  percpu_shared_ptr_factory<> factory(std::nullopt);
  std::atomic<int> destroyed{0}, ready{0}, finished{0};
  std::atomic<bool> start{false};
  constexpr int threads = 4;
  constexpr int count = 128;
  std::vector<percpu_shared_owner<tracked>> owners;
  std::vector<percpu_weak_ptr<tracked>> references;
  for (int i = 0; i < count; ++i) {
    owners.push_back(factory.make<tracked>(destroyed));
    if (i % 2 == 0)
      references.push_back(owners.back().weak());
  }
  std::vector<std::thread> killers;
  for (int t = 0; t < threads; ++t) {
    killers.emplace_back([&, t] {
      ++ready;
      while (!start.load())
        std::this_thread::yield();
      for (int i = t; i < count; i += threads) {
        owners[i].kill();
        std::this_thread::yield();
      }
      ++finished;
    });
  }
  while (ready != threads)
    std::this_thread::yield();
  start = true;
  while (finished != threads)
    factory.collect();
  for (auto& killer : killers)
    killer.join();
  factory.collect();
  assert(destroyed == count / 2);
  for (const auto& reference : references)
    assert(reference->value == 42);
  references.clear();
  auto before = remote_locks.load();
  assert(factory.collect() == count / 2 && factory.slab_count() == 0);
  assert(remote_locks == before); // already folded: zero checks only
  factory.make<tracked>(destroyed); // notification must work again after idle
  assert(factory.collect() == 1 && destroyed == count + 1);
}

int main() {
  static_assert(
      percpu_shared_ptr_factory<>::slots_per_slab ==
      percpu_shared_detail::cache_line_size / sizeof(int));
  contended_local_lock();
  owner_lifecycle_and_locking();
  owner_fold_accounting();
  half_range_flush<std::int8_t>();
  half_range_flush<std::int16_t>();
  half_range_flush<int>();
  half_range_flush<std::int64_t>();
  owner_transition_race();
  owner_scheduled_and_shutdown();
  slab_reclamation();
  slab_pinned_during_destruction();
  interval_validation();
  interrupted_fold();
  concurrent_make_and_collect();
  coalesced_kills();
  std::cout << "shared_ptr tests passed\n";
}
