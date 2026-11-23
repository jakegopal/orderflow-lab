// NodePool: O(1) allocation for small fixed-size objects.
//
// One intrusive free list per size class (multiples of 16 bytes up to 256),
// refilled from 64 KiB chunks. Allocation pops a free block and deallocation
// pushes it back; memory returns to the system when the pool is destroyed.
// Not thread-safe: a book is owned by one thread.
//
// PoolAllocator<T> adapts it to the standard Allocator interface (single
// objects from the pool, arrays from operator new), so PooledStdOrderMap is
// std::unordered_map with only the allocator changed. Comparing it with
// StdOrderMap separates the cost of malloc from the cost of the node layout.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orderflow {

class NodePool {
public:
    static constexpr std::size_t kGranule = 16;  // also the alignment every block gets
    static constexpr std::size_t kMaxSize = 256;
    static constexpr std::size_t kChunk = 64 * 1024;

    NodePool() = default;
    NodePool(const NodePool&) = delete;
    NodePool& operator=(const NodePool&) = delete;

    [[nodiscard]] static constexpr bool handles(std::size_t size, std::size_t align) noexcept {
        return size <= kMaxSize && align <= kGranule;
    }

    [[nodiscard]] void* allocate(std::size_t size) {
        assert(size > 0 && size <= kMaxSize);
        const std::size_t c = class_of(size);
        if (free_[c] == nullptr) refill(c);
        FreeBlock* b = free_[c];
        free_[c] = b->next;
        ++live_;
        return b;
    }

    void deallocate(void* p, std::size_t size) noexcept {
        const std::size_t c = class_of(size);
        auto* b = static_cast<FreeBlock*>(p);
        b->next = free_[c];
        free_[c] = b;
        --live_;
    }

    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    [[nodiscard]] std::size_t reserved_bytes() const noexcept { return chunks_.size() * kChunk; }

private:
    struct FreeBlock {
        FreeBlock* next;
    };
    static constexpr std::size_t kClasses = kMaxSize / kGranule;

    [[nodiscard]] static constexpr std::size_t class_of(std::size_t size) noexcept {
        return (size + kGranule - 1) / kGranule - 1;  // 1..16 -> 0, 17..32 -> 1, ...
    }

    void refill(std::size_t c) {
        const std::size_t block = (c + 1) * kGranule;
        auto chunk = std::make_unique_for_overwrite<std::byte[]>(kChunk);
        std::byte* base = chunk.get();
        // Thread the chunk's blocks into the free list, lowest address first,
        // so consecutive allocations are adjacent in memory.
        const std::size_t n = kChunk / block;
        for (std::size_t i = n; i-- > 0;) {
            auto* b = new (base + i * block) FreeBlock{free_[c]};
            free_[c] = b;
        }
        chunks_.push_back(std::move(chunk));
    }

    FreeBlock* free_[kClasses]{};
    std::vector<std::unique_ptr<std::byte[]>> chunks_;
    std::size_t live_ = 0;
};

template <class T>
class PoolAllocator {
public:
    using value_type = T;

    explicit PoolAllocator(NodePool& pool) noexcept : pool_(&pool) {}
    template <class U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept : pool_(other.pool()) {}  // NOLINT: rebinding

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n == 1 && NodePool::handles(sizeof(T), alignof(T))) return static_cast<T*>(pool_->allocate(sizeof(T)));
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
    }
    void deallocate(T* p, std::size_t n) noexcept {
        if (n == 1 && NodePool::handles(sizeof(T), alignof(T))) return pool_->deallocate(p, sizeof(T));
        ::operator delete(p, n * sizeof(T), std::align_val_t{alignof(T)});
    }

    [[nodiscard]] NodePool* pool() const noexcept { return pool_; }
    template <class U>
    bool operator==(const PoolAllocator<U>& o) const noexcept {
        return pool_ == o.pool();
    }

private:
    NodePool* pool_;
};

// StdOrderMap's design (std::unordered_map, chained nodes) with every node
// drawn from a NodePool: the same data structure with a cheaper allocator, to
// separate "what the allocator costs" from "what the node layout costs".
template <class V>
class PooledStdOrderMap {
    using Alloc = PoolAllocator<std::pair<const std::uint64_t, V>>;
    using Map = std::unordered_map<std::uint64_t, V, std::hash<std::uint64_t>, std::equal_to<>, Alloc>;

public:
    explicit PooledStdOrderMap(std::size_t expected = 1 << 16) : map_(0, std::hash<std::uint64_t>{}, std::equal_to<>{}, Alloc(*pool_)) {
        map_.reserve(expected);
    }

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
    // The pool lives on the heap so its address survives a move of the map.
    std::unique_ptr<NodePool> pool_ = std::make_unique<NodePool>();
    Map map_;
};

}  // namespace orderflow
