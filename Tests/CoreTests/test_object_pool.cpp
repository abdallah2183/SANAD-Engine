#include <NF/Test/TestFramework.hpp>
#include <NF/Core/ObjectPool.hpp>

#include <string>
#include <thread>
#include <vector>

namespace {

struct PoolTestItem {
    int id = 0;
    std::string tag;

    PoolTestItem() : id(0), tag("default") {}
    PoolTestItem(int i, std::string t) : id(i), tag(std::move(t)) {}
};

} // namespace

NF_TEST(object_pool_acquire_and_release) {
    nf::ObjectPool<PoolTestItem, 4> pool;
    NF_CHECK(pool.empty());
    NF_CHECK_EQ(pool.active_count(), 0u);

    PoolTestItem* a = pool.acquire();
    NF_CHECK(a != nullptr);
    NF_CHECK_EQ(a->id, 0);
    NF_CHECK(a->tag == "default");
    NF_CHECK_EQ(pool.active_count(), 1u);
    NF_CHECK_EQ(pool.capacity(), 4u);

    a->id = 42;
    pool.release(a);
    NF_CHECK_EQ(pool.active_count(), 0u);
    NF_CHECK_EQ(pool.free_count(), 4u);

    // Re-acquire should reuse the slot
    PoolTestItem* b = pool.acquire();
    NF_CHECK(b != nullptr);
    NF_CHECK_EQ(b->id, 0); // re-constructed
    NF_CHECK_EQ(pool.active_count(), 1u);
    pool.release(b);
}

NF_TEST(object_pool_chunk_expansion) {
    nf::ObjectPool<PoolTestItem, 4> pool;
    std::vector<PoolTestItem*> items;

    // Allocate 10 items (across 3 chunks: 4 + 4 + 4 = 12 cap)
    for (int i = 0; i < 10; ++i) {
        items.push_back(pool.create(i, "tag_" + std::to_string(i)));
    }

    NF_CHECK_EQ(pool.active_count(), 10u);
    NF_CHECK_EQ(pool.capacity(), 12u);

    // Verify all items retain their values (pointers didn't move)
    for (int i = 0; i < 10; ++i) {
        NF_CHECK_EQ(items[i]->id, i);
        NF_CHECK(items[i]->tag == "tag_" + std::to_string(i));
    }

    // Release all
    for (auto* item : items) {
        pool.release(item);
    }
    NF_CHECK_EQ(pool.active_count(), 0u);
    NF_CHECK_EQ(pool.free_count(), 12u);
}

NF_TEST(object_pool_reserve_and_clear) {
    nf::ObjectPool<PoolTestItem, 8> pool;
    pool.reserve(20);
    // Should allocate ceil(20/8)*8 = 24
    NF_CHECK_EQ(pool.capacity(), 24u);
    NF_CHECK_EQ(pool.free_count(), 24u);

    pool.clear();
    NF_CHECK_EQ(pool.capacity(), 0u);
    NF_CHECK_EQ(pool.free_count(), 0u);
}

NF_TEST(concurrent_object_pool_threads) {
    nf::ConcurrentObjectPool<PoolTestItem, 16> pool;
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 50;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pool, t]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                PoolTestItem* item = pool.create(t * 1000 + i, "thr");
                NF_CHECK(item != nullptr);
                std::this_thread::yield();
                pool.release(item);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    NF_CHECK_EQ(pool.active_count(), 0u);
}
