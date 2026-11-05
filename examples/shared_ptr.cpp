#include "percpu_shared_ptr.h"

#include <iostream>
#include <string>
#include <thread>

struct message {
  explicit message(std::string text) : text(std::move(text)) {}
  ~message() { std::cout << "Deleted: " << text << '\n'; }
  std::string text;
};

int main() {
  // The default factory collects every minute; nullopt selects manual cycles.
  percpu_shared_ptr_factory<> factory(std::nullopt);
  auto owner = factory.make<message>("hello from percpu_shared_ptr");
  auto reference = owner.weak();
  std::thread worker([copy = reference] {
    auto another = copy;
    std::cout << another->text << '\n';
  });
  worker.join();
  owner.kill(); // also called automatically on owner destruction
  factory.collect(); // fold one CPU at a time into the atomic
  std::cout << reference->text << '\n'; // counted handles survive kill
  reference.reset();
  auto reclaimed = factory.collect(); // atomic zero needs no more core locks
  std::cout << "Reclaimed " << reclaimed << " object(s)\n";

  percpu_shared_ptr_factory<std::int16_t> small_counters;
  auto value = small_counters.make<int>(42);
  std::cout << *value << ", " << small_counters.slots_per_slab
            << " counters per cache line\n";
}
