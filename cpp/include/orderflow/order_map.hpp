// Order reference number -> order, the L3 book's other hot structure.
//
// Every execute, cancel, delete and replace message names its order only by
// an 8-byte reference number, so the book does one lookup per message (two
// for a replace). On 01302019 that is ~200 million lookups, and up to a few
// million orders are live at once. Two implementations, same interface:
//
//   StdOrderMap        std::unordered_map. The standard guarantees that an
//                      element's address never changes, which forces separate
//                      chaining: each bucket points to heap-allocated nodes. A
//                      lookup is hash -> bucket array -> node pointer -> node,
//                      usually two dependent cache misses, plus a malloc and a
//                      free per order.
//
//   OpenAddressingMap  one flat array of {key, value} slots and linear probing:
//                      hash to a slot and walk forward until the key or an
//                      empty slot. A lookup is usually one cache miss, and
//                      neighbouring probes share the cache line. No per-order
//                      allocation. The cost: no stable addresses (a pointer
//                      from find() is valid only until the next insert or
//                      erase), and erase must repair the probe chain.
//
// Interface: find(key) -> V* or const V* (nullptr if absent), insert(key, value)
// -> bool (false if the key exists), erase(key) -> bool, size().
#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orderflow {

template <class V>
class StdOrderMap {
public:
    explicit StdOrderMap(std::size_t expected = 1 << 16) { map_.reserve(expected); }

    [[nodiscard]] V* find(std::uint64_t key) {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const V* find(std::uint64_t key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    bool insert(std::uint64_t key, const V& value) { return map_.try_emplace(key, value).second; }
    bool erase(std::uint64_t key) { return map_.erase(key) == 1; }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

private:
    std::unordered_map<std::uint64_t, V> map_;
};

// Key 0 marks an empty slot, so 0 cannot be stored. ITCH never assigns order
// reference 0 to a displayed order (the book counts any that appear).
template <class V>
class OpenAddressingMap {
    struct Slot {
        std::uint64_t key = 0;
        V value{};
    };

public:
    explicit OpenAddressingMap(std::size_t expected = 1 << 16) {
        rehash(std::bit_ceil(expected * 2 < 16 ? std::size_t{16} : expected * 2));
    }

    [[nodiscard]] const V* find(std::uint64_t key) const {
        assert(key != 0);
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key) return &slots_[i].value;
            if (slots_[i].key == 0) return nullptr;
        }
    }
    [[nodiscard]] V* find(std::uint64_t key) {
        return const_cast<V*>(std::as_const(*this).find(key));
    }

    bool insert(std::uint64_t key, const V& value) {
        assert(key != 0);
        // Keep the table at most half full: linear probing's expected probe
        // count grows like 1 / (1 - load)^2, so it stays short at 0.5.
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key) return false;
            if (slots_[i].key == 0) {
                slots_[i] = {key, value};
                ++size_;
                return true;
            }
        }
    }

    // Backward-shift deletion. Emptying a slot could cut a probe chain: a key
    // stored further along, which probed past this slot when it was full,
    // would become unreachable. So after emptying slot `hole`, walk the run of
    // occupied slots after it and pull back any key whose home is at or before
    // the hole (it may legally sit in the hole). That leaves the table exactly
    // as if the erased key had never been inserted, with no tombstones.
    bool erase(std::uint64_t key) {
        assert(key != 0);
        std::size_t hole = home(key);
        while (slots_[hole].key != key) {
            if (slots_[hole].key == 0) return false;
            hole = (hole + 1) & mask_;
        }
        for (std::size_t j = (hole + 1) & mask_; slots_[j].key != 0; j = (j + 1) & mask_) {
            const std::size_t h = home(slots_[j].key);
            // Distances along the probe direction, wrapping around the table.
            if (((j - h) & mask_) >= ((j - hole) & mask_)) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole] = Slot{};
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    // Fibonacci hashing: multiply by 2^64 / golden ratio and keep the top bits.
    // ITCH reference numbers are nearly sequential, and taking their low bits
    // directly would pile neighbouring orders into neighbouring slots; the
    // multiply spreads every input bit into the top bits.
    [[nodiscard]] std::size_t home(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    void rehash(std::size_t capacity) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(capacity, Slot{});
        mask_ = capacity - 1;
        shift_ = 64 - std::countr_zero(capacity);
        size_ = 0;
        for (const Slot& s : old) {
            if (s.key != 0) {
                std::size_t i = home(s.key);
                while (slots_[i].key != 0) i = (i + 1) & mask_;
                slots_[i] = s;
                ++size_;
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    int shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace orderflow
