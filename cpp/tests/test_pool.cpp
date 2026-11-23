// NodePool: blocks are distinct, aligned, reused after free; the pooled
// order map behaves exactly like StdOrderMap on a random workload.
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "orderflow/order_map.hpp"
#include "orderflow/pool.hpp"

namespace {

using namespace orderflow;

TEST(NodePool, BlocksAreDistinctAlignedAndReused) {
    NodePool pool;
    std::set<void*> seen;
    std::vector<void*> blocks;
    for (int i = 0; i < 10'000; ++i) {  // more than one 64 KiB chunk of 48-byte blocks
        void* p = pool.allocate(48);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % NodePool::kGranule, 0u);
        EXPECT_TRUE(seen.insert(p).second) << "a live block was handed out twice";
        blocks.push_back(p);
    }
    EXPECT_EQ(pool.live(), 10'000u);
    pool.deallocate(blocks[123], 48);
    EXPECT_EQ(pool.allocate(48), blocks[123]);  // LIFO reuse: the freed block comes back first
    for (void* p : blocks) pool.deallocate(p, 48);
    EXPECT_EQ(pool.live(), 0u);
}

TEST(NodePool, SizeClassesDoNotMix) {
    NodePool pool;
    void* small = pool.allocate(16);
    pool.deallocate(small, 16);
    void* big = pool.allocate(64);
    EXPECT_NE(big, small);  // a 16-byte block must not be returned for a 64-byte request
}

TEST(PooledStdOrderMap, AgreesWithStdOrderMap) {
    StdOrderMap<int> ref(16);
    PooledStdOrderMap<int> pooled(16);
    std::mt19937_64 rng(11);
    for (int i = 0; i < 200'000; ++i) {
        const std::uint64_t k = 1 + rng() % 5'000;
        switch (rng() % 3) {
            case 0:
                ASSERT_EQ(ref.insert(k, i), pooled.insert(k, i));
                break;
            case 1:
                ASSERT_EQ(ref.erase(k), pooled.erase(k));
                break;
            default: {
                const int* a = ref.find(k);
                const int* b = pooled.find(k);
                ASSERT_EQ(a == nullptr, b == nullptr);
                if (a) {
                    ASSERT_EQ(*a, *b);
                }
            }
        }
        ASSERT_EQ(ref.size(), pooled.size());
    }
}

}  // namespace
