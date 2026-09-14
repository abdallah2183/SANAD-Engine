// CoreTests/test_memory.cpp

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Memory.hpp>

using namespace nf;

NF_TEST(test_aligned_alloc) {
    void* ptr = nf::aligned_alloc(256, 64);
    NF_CHECK(ptr != nullptr);
    usize addr = reinterpret_cast<usize>(ptr);
    NF_CHECK_EQ(addr % 64, usize(0));
    nf::aligned_free(ptr);
}

NF_TEST(test_general_allocator) {
    GeneralAllocator alloc;
    void* ptr = alloc.allocate(128, 16);
    NF_CHECK(ptr != nullptr);
    alloc.deallocate(ptr);
}

NF_TEST(test_frame_allocator) {
    FrameAllocator alloc(4096);

    void* a = alloc.allocate(64, 16);
    void* b = alloc.allocate(128, 16);

    NF_CHECK(a != nullptr);
    NF_CHECK(b != nullptr);
    NF_CHECK(a != b);

    usize a_offset = static_cast<byte*>(a) - static_cast<byte*>(a) + 0;
    (void)a_offset;

    alloc.reset();
    NF_CHECK_EQ(alloc.used(), usize(0));

    // After reset, can allocate again
    void* c = alloc.allocate(64, 16);
    NF_CHECK(c != nullptr);
}

NF_TEST(test_pool_allocator) {
    PoolAllocator alloc(sizeof(u64), 16);

    // Allocate all blocks
    void* blocks[16];
    for (int i = 0; i < 16; ++i) {
        blocks[i] = alloc.allocate(sizeof(u64), 8);
        NF_CHECK(blocks[i] != nullptr);
    }

    // Pool should be empty
    NF_CHECK_EQ(alloc.free_blocks(), usize(0));

    // Free one and reallocate
    alloc.deallocate(blocks[0]);
    NF_CHECK_EQ(alloc.free_blocks(), usize(1));

    void* reuse = alloc.allocate(sizeof(u64), 8);
    NF_CHECK(reuse != nullptr);
}

NF_TEST(test_memory_tracker) {
    MemoryTracker::instance().track_allocation(100);
    NF_CHECK(MemoryTracker::instance().stats().total_allocated.load() >= 100);
}
