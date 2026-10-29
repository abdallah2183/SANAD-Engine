#include <NF/Test/TestFramework.hpp>
#include <NF/Core/RingAllocator.hpp>

using namespace nf;

NF_TEST(ring_allocator_basic_and_alignment) {
    RingAllocator ring(1024);
    NF_CHECK_EQ(ring.capacity(), 1024ULL);
    NF_CHECK_EQ(ring.used(), 0ULL);

    void* p1 = ring.allocate(100, 16);
    NF_CHECK(p1 != nullptr);
    NF_CHECK_EQ(reinterpret_cast<uintptr_t>(p1) % 16, 0ULL);

    void* p2 = ring.allocate(200, 32);
    NF_CHECK(p2 != nullptr);
    NF_CHECK_EQ(reinterpret_cast<uintptr_t>(p2) % 32, 0ULL);

    NF_CHECK(ring.used() >= 300);

    usize marker = ring.get_marker();
    ring.free_up_to(marker);
    NF_CHECK_EQ(ring.used(), 0ULL);
}

NF_TEST(ring_allocator_wrap_around) {
    // 512 bytes capacity
    RingAllocator ring(512);

    // Allocate 200 bytes (p1), marker m1 at ~200
    void* p1 = ring.allocate(200, 16);
    NF_CHECK(p1 != nullptr);
    usize m1 = ring.get_marker();

    // Allocate 150 bytes (p2), head at ~350, tail at 0
    void* p2 = ring.allocate(150, 16);
    NF_CHECK(p2 != nullptr);

    // Free p1: advance tail to m1 (~200). Now active data p2 is between 200 and 350
    ring.free_up_to(m1);

    // Now head is at ~350. Allocating 180 bytes won't fit at end (512 - 350 = 162),
    // but fits before tail (180 < 200), so it wraps around to start of buffer!
    void* p3 = ring.allocate(180, 16);
    NF_CHECK(p3 != nullptr);
    NF_CHECK(p3 < p2); // Wrapped before p2!
}
