// Lock-free single-producer, single-consumer ring buffer.
//
// Power-of-two capacity; head_ and tail_ only ever increase, and the slot is
// counter & (Capacity - 1). Empty when head == tail, full when
// tail - head == Capacity.
//
// Memory ordering:
//   push: write the slot, then tail_.store(release)
//   pop:  tail_.load(acquire), then read the slot
// so a consumer that sees the new tail also sees the slot written before it.
// The mirror pair on head_ stops the producer overwriting a slot that is still
// being read. Each side loads its own counter relaxed, since only it writes it.
//
// Each counter has its own cache line (no false sharing between the two
// cores), and each side keeps a cached copy of the other side's counter,
// re-reading the shared one only when the ring looks full or empty.
// SpscRing<T, N, false> and SpscRingNoCache turn those two choices off for
// bench_spsc; docs/phase4/01-spsc.md has the reasoning and the numbers.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace orderflow {

// 64 bytes on x86-64. std::hardware_destructive_interference_size would say
// so too, but GCC warns that its value can differ between compilations, which
// would make it unsafe in a header's ABI.
inline constexpr std::size_t kCacheLine = 64;

// Pad = false packs both counters into one cache line: the false-sharing
// ablation in bench_spsc. Production code uses the default.
template <class T, std::size_t Capacity, bool Pad = true>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_nothrow_copy_assignable_v<T> || std::is_nothrow_move_assignable_v<T>);
    static constexpr std::size_t kMask = Capacity - 1;
    static constexpr std::size_t kAlign = Pad ? kCacheLine : alignof(std::atomic<std::uint64_t>);

public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Producer only.
    template <class U>
    bool try_push(U&& value) noexcept {
        const std::uint64_t t = tail_.v.load(std::memory_order_relaxed);
        if (t - head_cache_.v >= Capacity) {
            head_cache_.v = head_.v.load(std::memory_order_acquire);
            if (t - head_cache_.v >= Capacity) return false;  // really full
        }
        slots_[t & kMask] = std::forward<U>(value);
        tail_.v.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer only.
    bool try_pop(T& out) noexcept {
        const std::uint64_t h = head_.v.load(std::memory_order_relaxed);
        if (h == tail_cache_.v) {
            tail_cache_.v = tail_.v.load(std::memory_order_acquire);
            if (h == tail_cache_.v) return false;  // really empty
        }
        out = std::move(slots_[h & kMask]);
        head_.v.store(h + 1, std::memory_order_release);
        return true;
    }

    // Approximate, for monitoring only: the two loads are not one snapshot.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return static_cast<std::size_t>(tail_.v.load(std::memory_order_acquire) -
                                        head_.v.load(std::memory_order_acquire));
    }

private:
    template <class V>
    struct alignas(kAlign) Aligned {
        V v{};
    };
    // Producer's line: its counter and its cached view of the consumer.
    Aligned<std::atomic<std::uint64_t>> tail_;
    Aligned<std::uint64_t> head_cache_;
    // Consumer's line: its counter and its cached view of the producer.
    Aligned<std::atomic<std::uint64_t>> head_;
    Aligned<std::uint64_t> tail_cache_;
    alignas(kCacheLine) std::array<T, Capacity> slots_{};
};

// The textbook version without cached counters: every push and pop loads the
// other side's shared counter, a cross-core cache-line read per operation.
// The cached-counter ablation in bench_spsc.
template <class T, std::size_t Capacity>
class SpscRingNoCache {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);
    static constexpr std::size_t kMask = Capacity - 1;

public:
    template <class U>
    bool try_push(U&& value) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) >= Capacity) return false;
        slots_[t & kMask] = std::forward<U>(value);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    bool try_pop(T& out) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;
        out = std::move(slots_[h & kMask]);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

private:
    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
    alignas(kCacheLine) std::array<T, Capacity> slots_{};
};

}  // namespace orderflow
