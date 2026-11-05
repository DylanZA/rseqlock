#pragma once

#include <rseqmutex.h>
#include <cstddef>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

// An experimental, factory-scoped shared pointer. The factory must outlive
// every pointer it creates. As with std::shared_ptr, separate handles may be
// used concurrently, but mutating the same handle requires synchronization.
namespace percpu_shared_detail {

constexpr std::size_t cache_line_size =
    std::hardware_destructive_interference_size;
// Sum signed deltas modulo 2^64, including the signed atomic's defined wrapping
// arithmetic. The actual number of live handles is strictly below 2^64 in this
// 64-bit address space, so an exact zero cannot alias a nonzero live count.

// A slab packs one signed counter per object into each CPU's cache line.
// TCounter width determines the number of object slots sharing that slab.
template <class TCounter>
struct layout {
  static_assert(
      std::is_integral_v<TCounter> && std::is_signed_v<TCounter>,
      "reference counters must be signed integers");
  static_assert(
      std::is_same_v<TCounter, std::remove_cv_t<TCounter>> &&
          sizeof(TCounter) <= sizeof(std::int64_t),
      "use an unqualified signed integer of at most 64 bits");
  static constexpr std::size_t slots = cache_line_size / sizeof(TCounter);
  static_assert(std::atomic<TCounter>::is_always_lock_free);
  static_assert(sizeof(std::atomic<TCounter>) == sizeof(TCounter));
  struct alignas(cache_line_size) line {
    line() noexcept {
      for (auto& counter : counters)
        std::atomic_init(
            &counter, TCounter{0}); // initialize under C++17 as well
    }
    // Local counters use load/store under the corresponding core lock.
    // Half-range flushes transfer their balance to the fallback atomic.
    std::atomic<TCounter> counters[slots];
  };
  static_assert(sizeof(line) == cache_line_size);
};

struct free_mutex {
  void operator()(rseq_mutex* mutex) const { rseq_mutex_free(mutex); }
};

// Folding holds only one remote CPU lock at a time.
class remote_guard {
 public:
  remote_guard(rseq_mutex* mutex, int cpu) : mutex_(mutex), cpu_(cpu) {
    int error = rseq_mutex_spin_lock_cpu(mutex_, cpu_);
    if (error)
      throw std::system_error(-error, std::generic_category(), "rseq CPU lock");
  }
  ~remote_guard() { rseq_mutex_spin_unlock_cpu(mutex_, cpu_); }
  remote_guard(const remote_guard&) = delete;
  remote_guard& operator=(const remote_guard&) = delete;
  remote_guard(remote_guard&&) = delete;
  remote_guard& operator=(remote_guard&&) = delete;

 private:
  rseq_mutex* mutex_;
  int cpu_;
};

// dead also covers a reserved slot whose initialization has not completed.
enum class slot_state { dead, active, dying };

template <class T>
struct list_links {
  T* previous = nullptr;
  T* next = nullptr;
};

// Non-owning hooks embedded in slabs/slots. The factory metadata mutex protects
// every list. A slot's single hook moves between free, active, pending and
// retired lists; it never belongs to two of these at once.
template <class T, list_links<T> T::* Link>
class intrusive_list {
 public:
  intrusive_list() = default;
  intrusive_list(const intrusive_list&) = delete;
  intrusive_list& operator=(const intrusive_list&) = delete;
  T* front() const noexcept { return first_; }
  bool empty() const noexcept { return !first_; }
  static T* next(const T& value) noexcept { return (value.*Link).next; }
  void push_front(T& value) noexcept {
    auto& link = value.*Link;
    link.previous = nullptr;
    link.next = first_;
    if (first_)
      (first_->*Link).previous = &value;
    first_ = &value;
  }
  void erase(T& value) noexcept {
    auto& link = value.*Link;
    if (link.previous)
      (link.previous->*Link).next = link.next;
    else
      first_ = link.next;
    if (link.next)
      (link.next->*Link).previous = link.previous;
    link = {};
  }
  T* pop_front() noexcept {
    auto* value = first_;
    if (value)
      erase(*value);
    return value;
  }

 private:
  T* first_ = nullptr;
};

template <class TCounter>
struct slab;

template <class TCounter>
struct alignas(cache_line_size) slot {
  slab<TCounter>* owner = nullptr;
  std::size_t index = 0;
  void* object = nullptr;
  void (*destroy)(void*) noexcept = nullptr;
  list_links<slot> link;
  // Busy local locks and half-range counter flushes use this signed atomic.
  // Also carries the initial reference created by the factory.
  std::atomic<std::int64_t> fallback{0};
  TCounter fold_delta = 0; // collector scratch, applied after releasing a CPU
  // This occupies an entire separate cache line. Neither other
  // objects' states nor this object's counter updates share the state's line.
  alignas(cache_line_size) std::atomic<slot_state> state{slot_state::dead};

  // Release-publish the initialized fields after reservation linked the slot.
  // The collector must acquire-load state before reading any of these fields.
  void init(void* value, void (*deleter)(void*) noexcept) noexcept {
    fallback.store(1, std::memory_order_relaxed);
    destroy = deleter;
    object = value;
    state.store(slot_state::active, std::memory_order_release);
  }

  bool slow() const noexcept {
    return state.load(std::memory_order_acquire) != slot_state::active;
  }
};

template <class TCounter>
using slot_list = intrusive_list<slot<TCounter>, &slot<TCounter>::link>;

template <class TCounter>
struct slab {
  explicit slab(rseq_mutex* mutex, std::atomic<bool>* kills = nullptr)
      : mutex(mutex), new_kills(kills) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
      slots[i].owner = this;
      slots[i].index = i;
      free_slots.push_front(slots[i]);
    }
  }
  std::array<typename layout<TCounter>::line, RSEQ_MUTEX_MAX_CPUS> cores{};
  std::array<slot<TCounter>, layout<TCounter>::slots> slots{};
  rseq_mutex* mutex;
  std::atomic<bool>* new_kills;
  list_links<slab> link;
  list_links<slab> available_link;
  slot_list<TCounter> free_slots;
  slot_list<TCounter> active_slots;
  // Includes pending slots and retired objects whose destructor is running.
  std::size_t used = 0;
};

template <int Delta, class TCounter>
[[gnu::always_inline]] inline void change(slot<TCounter>* entry) noexcept {
  static_assert(Delta == 1 || Delta == -1);
  auto& slab = *entry->owner;
  int cpu = rseq_mutex_try_lock(slab.mutex);
  if (cpu >= 0) {
    // Read the slot state only inside the lock so no delayed updater writes a
    // per-core count after the collector has folded it into the atomic.
    if (!entry->slow()) {
      auto& count = slab.cores[cpu].counters[entry->index];
      constexpr auto half_range = std::numeric_limits<TCounter>::max() / 2 + 1;
      // Every update leaves the local count strictly between these bounds,
      // so adding either delta cannot overflow, even for a 64-bit counter.
      auto value =
          static_cast<TCounter>(count.load(std::memory_order_relaxed) + Delta);
      bool flush = Delta > 0 ? value >= half_range : value <= -half_range;
      if (flush) {
        // Include this operation in the transfer and keep the core locked
        // until both parts finish, so folding cannot observe a partial flush.
        entry->fallback.fetch_add(value, std::memory_order_acq_rel);
        count.store(0, std::memory_order_relaxed);
      } else {
        count.store(value, std::memory_order_relaxed);
      }
      rseq_mutex_release(slab.mutex, cpu);
      return;
    }
    rseq_mutex_release(slab.mutex, cpu);
  }
  // No waiting for a busy core and no global lock on either update path.
  // This handle still owns its reference until the decrement commits. Do not
  // touch the slot after that RMW: the collector may immediately reclaim it.
  entry->fallback.fetch_add(
      Delta, Delta > 0 ? std::memory_order_relaxed : std::memory_order_release);
}

template <class TCounter>
TCounter drain_cpu(slot<TCounter>& entry, int cpu) noexcept {
  // Caller holds this CPU's lock. Only extract the delta here; updating the
  // shared fallback atomic is done after unlocking the CPU.
  auto& counter = entry.owner->cores[cpu].counters[entry.index];
  auto delta = counter.load(std::memory_order_relaxed);
  counter.store(0, std::memory_order_relaxed);
  return delta;
}

} // namespace percpu_shared_detail

template <class TCounter = int>
class percpu_shared_ptr_factory;
template <class T, class TCounter = int>
class percpu_shared_owner;

template <class T, class TCounter = int>
class percpu_shared_ptr {
  static_assert(!std::is_array_v<T> && !std::is_void_v<T>);
  using slot = percpu_shared_detail::slot<TCounter>;

 public:
  using element_type = T;
  using counter_type = TCounter;

  constexpr percpu_shared_ptr() noexcept = default;
  constexpr percpu_shared_ptr(std::nullptr_t) noexcept {}
  percpu_shared_ptr(const percpu_shared_ptr& other) noexcept
      : entry_(other.entry_) {
    if (entry_)
      percpu_shared_detail::change<1>(entry_);
  }
  percpu_shared_ptr(percpu_shared_ptr&& other) noexcept
      : entry_(std::exchange(other.entry_, nullptr)) {}
  ~percpu_shared_ptr() {
    if (entry_)
      percpu_shared_detail::change<-1>(entry_);
  }
  percpu_shared_ptr& operator=(const percpu_shared_ptr& other) noexcept {
    if (this != &other)
      percpu_shared_ptr(other).swap(*this);
    return *this;
  }
  percpu_shared_ptr& operator=(percpu_shared_ptr&& other) noexcept {
    if (this != &other)
      percpu_shared_ptr(std::move(other)).swap(*this);
    return *this;
  }
  percpu_shared_ptr& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }
  void reset() noexcept { percpu_shared_ptr().swap(*this); }
  void swap(percpu_shared_ptr& other) noexcept {
    std::swap(entry_, other.entry_);
  }
  friend void swap(percpu_shared_ptr& a, percpu_shared_ptr& b) noexcept {
    a.swap(b);
  }
  T* get() const noexcept {
    return entry_ ? static_cast<T*>(entry_->object) : nullptr;
  }
  T& operator*() const noexcept { return *get(); }
  T* operator->() const noexcept { return get(); }
  explicit operator bool() const noexcept { return entry_ != nullptr; }

 private:
  friend class percpu_shared_ptr_factory<TCounter>;
  friend class percpu_shared_owner<T, TCounter>;
  explicit percpu_shared_ptr(slot* entry) noexcept : entry_(entry) {}
  slot* entry_ = nullptr;
};

// "Weak" means subordinate to the main owner, not std::weak_ptr semantics:
// these are counted handles that keep the pointee alive through shutdown.
template <class T, class TCounter = int>
using percpu_weak_ptr = percpu_shared_ptr<T, TCounter>;

template <class T, class TCounter>
class percpu_shared_owner {
  static_assert(!std::is_array_v<T> && !std::is_void_v<T>);
  using slot = percpu_shared_detail::slot<TCounter>;

 public:
  percpu_shared_owner() noexcept = default;
  percpu_shared_owner(const percpu_shared_owner&) = delete;
  percpu_shared_owner& operator=(const percpu_shared_owner&) = delete;
  percpu_shared_owner(percpu_shared_owner&& other) noexcept
      : entry_(std::exchange(other.entry_, nullptr)) {}
  percpu_shared_owner& operator=(percpu_shared_owner&& other) noexcept {
    if (this != &other) {
      kill();
      entry_ = std::exchange(other.entry_, nullptr);
    }
    return *this;
  }
  ~percpu_shared_owner() { kill(); }

  void kill() noexcept {
    if (auto* entry = std::exchange(entry_, nullptr)) {
      entry->state.store(
          percpu_shared_detail::slot_state::dying, std::memory_order_release);
      // Notify before dropping the initial reference: it pins the slot/slab
      // until the notification is committed. The collector acquire-consumes
      // this coalesced flag before scanning states, so no kill is lost.
      if (auto* notification = entry->owner->new_kills)
        notification->exchange(true, std::memory_order_release);
      // Drop the owner's initial reference in atomic mode. A collector can
      // reclaim immediately after this RMW, so do not touch the slot again.
      entry->fallback.fetch_sub(1, std::memory_order_release);
    }
  }
  void reset() noexcept { kill(); }
  percpu_weak_ptr<T, TCounter> weak() const noexcept {
    if (entry_)
      percpu_shared_detail::change<1>(entry_);
    return percpu_weak_ptr<T, TCounter>(entry_);
  }
  T* get() const noexcept {
    return entry_ ? static_cast<T*>(entry_->object) : nullptr;
  }
  T& operator*() const noexcept { return *get(); }
  T* operator->() const noexcept { return get(); }
  explicit operator bool() const noexcept { return entry_ != nullptr; }

 private:
  friend class percpu_shared_ptr_factory<TCounter>;
  explicit percpu_shared_owner(slot* entry) noexcept : entry_(entry) {}
  slot* entry_ = nullptr;
};

template <class TCounter>
class percpu_shared_ptr_factory {
  using slab = percpu_shared_detail::slab<TCounter>;
  using slot = percpu_shared_detail::slot<TCounter>;
  using slot_list = percpu_shared_detail::slot_list<TCounter>;
  using slab_list = percpu_shared_detail::intrusive_list<slab, &slab::link>;
  using available_list =
      percpu_shared_detail::intrusive_list<slab, &slab::available_link>;

 public:
  static constexpr std::size_t cache_line_size =
      percpu_shared_detail::cache_line_size;
  static constexpr std::size_t slots_per_slab =
      percpu_shared_detail::layout<TCounter>::slots;

  // Normally collect once a minute. nullopt disables the worker; a supplied
  // interval must be strictly positive.
  explicit percpu_shared_ptr_factory(
      std::optional<std::chrono::milliseconds> interval =
          std::chrono::minutes(1)) {
    if (interval && interval->count() <= 0)
      throw std::invalid_argument("collection interval must be positive");
    if (rseq_mutex_static_init())
      throw std::runtime_error("rseq/membarrier support is unavailable");
    mutex_.reset(rseq_mutex_create());
    if (!mutex_)
      throw std::bad_alloc();
    if (interval) {
      worker_ = std::thread([this, interval = *interval] {
        std::unique_lock<std::mutex> lock(schedule_mutex_);
        while (!wake_.wait_for(lock, interval, [this] { return stop_; })) {
          lock.unlock();
          try {
            collect();
          } catch (const std::bad_alloc&) {
            // Keep the objects alive and retry at the next scheduled scan.
          }
          lock.lock();
        }
      });
    }
  }

  percpu_shared_ptr_factory(const percpu_shared_ptr_factory&) = delete;
  percpu_shared_ptr_factory& operator=(const percpu_shared_ptr_factory&) =
      delete;
  percpu_shared_ptr_factory(percpu_shared_ptr_factory&&) = delete;
  percpu_shared_ptr_factory& operator=(percpu_shared_ptr_factory&&) = delete;

  ~percpu_shared_ptr_factory() {
    {
      std::lock_guard<std::mutex> lock(schedule_mutex_);
      stop_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable())
      worker_.join();
    // Destructors can kill other managed objects, so drain to a fixed point
    // without waiting for the minute timer during shutdown.
    for (;;) {
      auto destroyed = collect();
      if (!destroyed && pending_.empty())
        break;
    }
    if (!slabs_.empty())
      std::terminate(); // live handles/cycles violate the lifetime contract
  }

  template <class T, class... Args>
  auto make(Args&&... args) {
    static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>);
    auto object = std::make_unique<T>(std::forward<Args>(args)...);
    auto* entry = reserve_slot();
    // No throwing operations remain after reservation. init() publishes the
    // fields with a release store, without reacquiring the metadata lock.
    entry->init(object.release(), [](void* p) noexcept {
      delete static_cast<T*>(p);
    });
    return percpu_shared_owner<T, TCounter>(entry);
  }

  // Snapshot for inspecting retained allocation, including running destructors.
  std::size_t slab_count() {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    std::size_t count = 0;
    for (auto* block = slabs_.front(); block; block = slab_list::next(*block))
      ++count;
    return count;
  }

  // Calls are serialized through destructor completion, alongside make() and
  // operations on distinct handles. Destructors must not call collect() on
  // this factory recursively. Killed objects are folded in the same call.
  // Returns the number of objects destroyed by this call.
  std::size_t collect() {
    std::lock_guard<std::mutex> collection_lock(collection_mutex_);
    // Retired slots pin their slab until destruction outside the metadata lock.
    slot_list retired;
    try {
      std::lock_guard<std::mutex> lock(metadata_mutex_);
      if (slabs_.empty())
        return 0;

      // Poll only folded objects. The relaxed test leaves nonzero fallback
      // cache lines shared with readers; the RMW confirms a possible zero in
      // modification order and acquires the release sequence before deletion.
      for (auto* entry = dying_.front(); entry;) {
        auto* next = slot_list::next(*entry);
        if (has_zero_references(*entry)) {
          dying_.erase(*entry);
          retired.push_front(*entry);
        }
        entry = next;
      }
      discover_kills();
      // Keep the entire batch linked until every CPU has been drained.
      fold_pending();
      while (auto* entry = pending_.pop_front()) {
        if (has_zero_references(*entry))
          retired.push_front(*entry);
        else
          dying_.push_front(*entry);
      }
    } catch (...) {
      destroy_retired(retired);
      throw;
    }
    return destroy_retired(retired);
  }

 private:
  static bool has_zero_references(slot& entry) noexcept {
    return entry.fallback.load(std::memory_order_relaxed) == 0 &&
        entry.fallback.fetch_add(0, std::memory_order_acq_rel) == 0;
  }

  void discover_kills() noexcept {
    if (!new_kills_.load(std::memory_order_acquire))
      return;
    // All notifiers use release RMWs. This acquire consumes their release
    // sequence, publishing every kill that preceded this exchange. A kill
    // arriving during the scan leaves true for a subsequent collection call.
    new_kills_.exchange(false, std::memory_order_acq_rel);
    for (auto* block = slabs_.front(); block; block = slab_list::next(*block)) {
      for (auto* entry = block->active_slots.front(); entry;) {
        auto* next = slot_list::next(*entry);
        if (entry->state.load(std::memory_order_acquire) ==
            percpu_shared_detail::slot_state::dying) {
          block->active_slots.erase(*entry);
          pending_.push_front(*entry);
        }
        entry = next;
      }
    }
  }

  void fold_pending() {
    if (pending_.empty())
      return;
    // One lock per CPU for the whole batch, holding at most one at a time.
    // Nothing is marked folded or unlinked until all CPUs succeed.
    for (int cpu = 0; cpu < RSEQ_MUTEX_MAX_CPUS; ++cpu) {
      {
        percpu_shared_detail::remote_guard guard(mutex_.get(), cpu);
        for (auto* entry = pending_.front(); entry;
             entry = slot_list::next(*entry))
          entry->fold_delta = percpu_shared_detail::drain_cpu(*entry, cpu);
      }
      // These RMWs touch shared cache lines, so keep them outside the CPU
      // lock. Complete the transfer before the next lock can fail; retries
      // then see zeros on CPUs already drained, without losing any counts.
      for (auto* entry = pending_.front(); entry;
           entry = slot_list::next(*entry)) {
        if (entry->fold_delta != 0)
          entry->fallback.fetch_add(
              entry->fold_delta, std::memory_order_acq_rel);
      }
    }
  }

  slot* reserve_slot() {
    // Declare the spare before the lock so a redundant allocation is freed
    // after unlocking if another maker supplied available slots meanwhile.
    std::unique_ptr<slab> spare;
    std::unique_lock<std::mutex> lock(metadata_mutex_);
    if (available_.empty()) {
      lock.unlock();
      spare = std::make_unique<slab>(mutex_.get(), &new_kills_);
      lock.lock();
      if (available_.empty()) {
        slabs_.push_front(*spare);
        available_.push_front(*spare);
        spare.release();
      }
    }
    auto* block = available_.front();
    auto* entry = block->free_slots.pop_front();
    if (block->free_slots.empty())
      available_.erase(*block);
    // A dead state makes this reserved slot safe for the collector to skip.
    ++block->used;
    block->active_slots.push_front(*entry);
    return entry;
  }

  void release_retired(slot& entry) noexcept {
    entry.destroy(entry.object);
    release_slot(entry);
  }

  void release_slot(slot& entry) noexcept {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    auto* block = entry.owner;
    entry.object = nullptr;
    entry.destroy = nullptr;
    entry.state.store(
        percpu_shared_detail::slot_state::dead, std::memory_order_relaxed);
    if (--block->used == 0) {
      // Free slots belong only to this slab's list; no global slot pointers
      // need fixing up when its last occupied/retired slot is released.
      if (!block->free_slots.empty())
        available_.erase(*block);
      slabs_.erase(*block);
      delete block;
    } else {
      if (block->free_slots.empty())
        available_.push_front(*block);
      block->free_slots.push_front(entry);
    }
  }

  std::size_t destroy_retired(slot_list& retired) noexcept {
    // The collection lock remains held, but make() and handle updates may run.
    // Cleanup is also used after a partial fold fails, outside metadata locks.
    std::size_t destroyed = 0;
    while (auto* entry = retired.pop_front()) {
      release_retired(*entry);
      ++destroyed;
    }
    return destroyed;
  }

  std::unique_ptr<rseq_mutex, percpu_shared_detail::free_mutex> mutex_;
  std::mutex collection_mutex_;
  std::mutex metadata_mutex_;
  slab_list slabs_;
  available_list available_; // slabs with free slots
  slot_list pending_;
  slot_list dying_; // folded objects waiting for their remaining handles
  std::atomic<bool> new_kills_{false};
  std::mutex schedule_mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  std::thread worker_;
};
