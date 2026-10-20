// NF/Core/Memory.cpp

#include <NF/Core/Memory.hpp>
#include <NF/Core/Logger.hpp>

#include <malloc.h>
#include <cstddef>
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

namespace {

// Every block carries a small header so deallocate can report the size back
// to the tracker. Without it, GeneralAllocator::deallocate could only report
// "0 bytes freed", which meant total_deallocated never advanced and
// MemoryScope::bytes_allocated() could only grow — the scope API was
// therefore unable to report a clean scope.
//
// Layout:  [ size ][ offset ][ ...padding... ][ payload ]
//                     ^ the payload starts here, aligned as requested.
// The offset field sits immediately before the payload, so deallocate can
// find the base without a lookup table.
struct AllocHeader {
    usize size;   // payload size in bytes
    usize offset; // bytes from the base pointer to the payload
};

constexpr usize kHeaderBytes = sizeof(AllocHeader);

} // namespace

void* GeneralAllocator::allocate(usize size, usize alignment) {
    if (alignment == 0) alignment = alignof(std::max_align_t);
    // The offset must be a multiple of the alignment so the payload keeps it,
    // and must be at least large enough for the two header fields.
    const usize offset = (kHeaderBytes + alignment - 1) & ~(alignment - 1);
    const usize total = offset + size;

    byte* base = static_cast<byte*>(nf::aligned_alloc(total, alignment));
    if (!base) return nullptr;

    AllocHeader* header = reinterpret_cast<AllocHeader*>(base);
    header->size = size;
    header->offset = offset;

    MemoryTracker::instance().track_allocation(size);
    return base + offset;
}

void GeneralAllocator::deallocate(void* ptr) {
    if (!ptr) return;
    byte* payload = static_cast<byte*>(ptr);
    // The offset lives in the two words before the payload; the size in the
    // two words before that. Both were written by allocate().
    const usize offset = *reinterpret_cast<const usize*>(payload - sizeof(usize));
    const usize size = *reinterpret_cast<const usize*>(payload - sizeof(usize) - sizeof(usize));
    byte* base = payload - offset;
    nf::aligned_free(base);
    MemoryTracker::instance().track_deallocation(size);
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
    : m_block_size(block_size), m_block_count(block_count), m_alignment(alignment) {
    if (m_alignment == 0) m_alignment = alignof(std::max_align_t);
    // Ensure block size is at least sizeof(FreeNode) and properly aligned.
    m_block_size = (m_block_size + m_alignment - 1) & ~(m_alignment - 1);
    if (m_block_size < sizeof(FreeNode)) {
        m_block_size = sizeof(FreeNode);
    }
    // The floor above can break the alignment invariant (sizeof(FreeNode) is
    // 8 bytes, which is not a multiple of 16 or 64), and the block stride has
    // to stay a multiple of the alignment or every block after the first
    // would be misaligned. Round back up.
    m_block_size = (m_block_size + m_alignment - 1) & ~(m_alignment - 1);

    usize total = m_block_size * block_count;
    m_base = static_cast<byte*>(nf::aligned_alloc(total, m_alignment));
    NF_VERIFY(m_base != nullptr, "PoolAllocator: failed to allocate pool");

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

void* PoolAllocator::allocate(usize size, usize alignment) {
    if (size > m_block_size) return nullptr;
    // A block can only satisfy an alignment stricter than the pool's own if
    // the stride happens to be a multiple of it. Refuse rather than hand out
    // a misaligned pointer.
    if (alignment > m_alignment && (m_block_size % alignment) != 0) return nullptr;

    std::lock_guard lock(m_mutex);
    if (!m_free_list) return nullptr;

    FreeNode* node = m_free_list;
    m_free_list = node->next;
    m_free_count.fetch_sub(1, std::memory_order_relaxed);
    return node;
}

void PoolAllocator::deallocate(void* ptr) {
    if (!ptr) return;
    // A pointer that did not come from this pool would corrupt the free list
    // and hand out overlapping memory on the next allocation. Only accept
    // pointers inside our own block range.
    const byte* p = static_cast<const byte*>(ptr);
    if (p < m_base || p >= m_base + m_block_size * m_block_count) return;
    if (((p - m_base) % m_block_size) != 0) return;

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
