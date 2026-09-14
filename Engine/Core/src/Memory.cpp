// NF/Core/Memory.cpp

#include <NF/Core/Memory.hpp>
#include <NF/Core/Logger.hpp>

#include <malloc.h>
#include <cstring>
#include <iostream>

namespace nf {

// --- Aligned allocation ---

void* aligned_alloc(usize size, usize alignment) {
#ifdef _MSC_VER
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) return nullptr;
    return ptr;
#endif
}

void aligned_free(void* ptr) {
#ifdef _MSC_VER
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

// --- GeneralAllocator ---

void* GeneralAllocator::allocate(usize size, usize alignment) {
    void* ptr = nf::aligned_alloc(size, alignment);
    if (ptr) {
        MemoryTracker::instance().track_allocation(size);
    }
    return ptr;
}

void GeneralAllocator::deallocate(void* ptr) {
    if (ptr) {
        // Can't know size here; tracker is approximate
        nf::aligned_free(ptr);
        MemoryTracker::instance().track_deallocation(0);
    }
}

// --- FrameAllocator ---

FrameAllocator::FrameAllocator(usize capacity)
    : m_capacity(capacity), m_offset(0) {
    m_base = static_cast<byte*>(nf::aligned_alloc(m_capacity, 64));
    NF_VERIFY(m_base != nullptr, "FrameAllocator: failed to allocate arena");
}

FrameAllocator::~FrameAllocator() {
    if (m_base) {
        nf::aligned_free(m_base);
        m_base = nullptr;
    }
}

void* FrameAllocator::allocate(usize size, usize alignment) {
    // Align offset up
    usize current = m_offset;
    usize aligned = (current + alignment - 1) & ~(alignment - 1);

    if (aligned + size > m_capacity) {
        return nullptr; // Out of frame memory
    }

    m_offset = aligned + size;
    return m_base + aligned;
}

void FrameAllocator::deallocate(void* /*ptr*/) {
    // No-op — frame allocator doesn't free individual allocations
}

void FrameAllocator::reset() {
    m_offset = 0;
}

// --- PoolAllocator ---

PoolAllocator::PoolAllocator(usize block_size, usize block_count, usize alignment)
    : m_block_size(block_size), m_block_count(block_count) {
    // Ensure block size is at least sizeof(FreeNode) and properly aligned
    m_block_size = (block_size + alignment - 1) & ~(alignment - 1);
    if (m_block_size < sizeof(FreeNode)) {
        m_block_size = sizeof(FreeNode);
    }

    usize total = m_block_size * block_count;
    m_base = static_cast<byte*>(nf::aligned_alloc(total, alignment));

    // Build free list
    m_free_list = nullptr;
    for (usize i = 0; i < block_count; ++i) {
        auto* node = reinterpret_cast<FreeNode*>(m_base + i * m_block_size);
        node->next = m_free_list;
        m_free_list = node;
    }
    m_free_count.store(block_count, std::memory_order_relaxed);
}

PoolAllocator::~PoolAllocator() {
    if (m_base) {
        nf::aligned_free(m_base);
        m_base = nullptr;
    }
}

void* PoolAllocator::allocate(usize size, usize /*alignment*/) {
    if (size > m_block_size) return nullptr;

    std::lock_guard lock(m_mutex);
    if (!m_free_list) return nullptr;

    FreeNode* node = m_free_list;
    m_free_list = node->next;
    m_free_count.fetch_sub(1, std::memory_order_relaxed);
    return node;
}

void PoolAllocator::deallocate(void* ptr) {
    if (!ptr) return;

    std::lock_guard lock(m_mutex);
    auto* node = reinterpret_cast<FreeNode*>(ptr);
    node->next = m_free_list;
    m_free_list = node;
    m_free_count.fetch_add(1, std::memory_order_relaxed);
}

// --- MemoryTracker ---

MemoryTracker& MemoryTracker::instance() {
    static MemoryTracker tracker;
    return tracker;
}

void MemoryTracker::track_allocation(usize size) {
    m_stats.total_allocated.fetch_add(size, std::memory_order_relaxed);
    m_stats.allocation_count.fetch_add(1, std::memory_order_relaxed);
}

void MemoryTracker::track_deallocation(usize size) {
    m_stats.total_deallocated.fetch_add(size, std::memory_order_relaxed);
    m_stats.deallocation_count.fetch_add(1, std::memory_order_relaxed);
}

// --- MemoryScope ---

MemoryScope::MemoryScope() {
    auto& stats = MemoryTracker::instance().stats();
    start_allocated = stats.total_allocated.load(std::memory_order_relaxed) -
                      stats.total_deallocated.load(std::memory_order_relaxed);
    start_count = stats.allocation_count.load(std::memory_order_relaxed) -
                  stats.deallocation_count.load(std::memory_order_relaxed);
}

MemoryScope::~MemoryScope() = default;

usize MemoryScope::bytes_allocated() const {
    auto& stats = MemoryTracker::instance().stats();
    usize current = stats.total_allocated.load(std::memory_order_relaxed) -
                    stats.total_deallocated.load(std::memory_order_relaxed);
    return current - start_allocated;
}

u64 MemoryScope::alloc_count() const {
    auto& stats = MemoryTracker::instance().stats();
    u64 current = stats.allocation_count.load(std::memory_order_relaxed) -
                  stats.deallocation_count.load(std::memory_order_relaxed);
    return current - start_count;
}

} // namespace nf
