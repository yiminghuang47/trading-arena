// minimal open-addressing hash map (u64 -> i32).
//
// Used for order_id -> order-pool-index lookups on cancel/modify. Pre-sized at
// construction and never rehashes, so it performs no allocation on the hot path.
// Linear probing with tombstones; capacity is a power of two.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace arena {

class OpenAddrMap {
 public:
  static constexpr std::int32_t kAbsent = -1;

  // capacity is rounded up to a power of two >= 2 * expected entries.
  explicit OpenAddrMap(std::size_t expected) {
    std::size_t cap = 8;
    while (cap < expected * 2) cap <<= 1;
    slots_.assign(cap, Slot{});
    mask_ = cap - 1;
  }

  void insert(std::uint64_t key, std::int32_t val) {
    std::size_t i = hash(key) & mask_;
    std::size_t first_tomb = kNoSlot;
    for (;;) {
      Slot& s = slots_[i];
      if (s.state == State::Empty) {
        Slot& dst = (first_tomb == kNoSlot) ? s : slots_[first_tomb];
        dst = {key, val, State::Full};
        ++size_;
        return;
      }
      if (s.state == State::Tomb) {
        if (first_tomb == kNoSlot) first_tomb = i;
      } else if (s.key == key) {  // overwrite existing
        s.val = val;
        return;
      }
      i = (i + 1) & mask_;
    }
  }

  std::int32_t find(std::uint64_t key) const {
    std::size_t i = hash(key) & mask_;
    for (;;) {
      const Slot& s = slots_[i];
      if (s.state == State::Empty) return kAbsent;
      if (s.state == State::Full && s.key == key) return s.val;
      i = (i + 1) & mask_;
    }
  }

  void erase(std::uint64_t key) {
    std::size_t i = hash(key) & mask_;
    for (;;) {
      Slot& s = slots_[i];
      if (s.state == State::Empty) return;
      if (s.state == State::Full && s.key == key) {
        s.state = State::Tomb;
        --size_;
        return;
      }
      i = (i + 1) & mask_;
    }
  }

  std::size_t size() const noexcept { return size_; }

 private:
  enum class State : std::uint8_t { Empty = 0, Full = 1, Tomb = 2 };
  struct Slot {
    std::uint64_t key = 0;
    std::int32_t  val = 0;
    State         state = State::Empty;
  };

  static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

  // splitmix64 finalizer — good dispersion for sequential / structured keys.
  static std::uint64_t hash(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

}  