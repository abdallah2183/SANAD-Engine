#include <NF/Core/RingAllocator.hpp>
#include <NF/Core/Assert.hpp>

#include <algorithm>

namespace nf {

RingAllocator::RingAllocator(usize capacity)
    : m_capacity(capacity) {
    NF_ASSERT(capacity > 0, "RingAllocator capacity must be > 0");
    // Allocate base with 64-byte alignment to support up to 64-byte aligned requests (AVX-512)
    m_base = static_cast<byte*>(aligned_alloc(capacity, 64));
    m_head.store(0, std::memory_order_relaxed);
    m_tail.store(0, std::memory_order_relaxed);
}

RingAllocator::~RingAllocator() {
    if (m_base) {
        aligned_free(m_base);
        m_base = nullptr;
    }
}

void* RingAllocator::allocate(usize size, usize alignment) {
    if (size == 0) return nullptr;

    std::lock_guard<std::mutex> lock(m_mutex);

    usize head = m_head.load(std::memory_order_relaxed);
    usize tail = m_tail.load(std::memory_order_relaxed);

    // If completely empty, reset to start of buffer
    if (head == tail) {
        head = 0;
        tail = 0;
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

    auto align_offset = [&](usize off) -> usize {
        const uintptr_t raw_ptr = reinterpret_cast<uintptr_t>(m_base + off);
        const uintptr_t aligned_ptr = (raw_ptr + (alignment - 1)) & ~(static_cast<uintptr_t>(alignment - 1));
        return static_cast<usize>(aligned_ptr - reinterpret_cast<uintptr_t>(m_base));
    };

    const usize aligned_head = align_offset(head);

    if (head >= tail) {
        // Space from head to end of buffer
        if (aligned_head + size <= m_capacity) {
            m_head.store(aligned_head + size, std::memory_order_relaxed);
            return m_base + aligned_head;
        }

        // Try wrapping around to 0
        const usize wrap_aligned = align_offset(0);
        if (wrap_aligned + size < tail) {
            m_head.store(wrap_aligned + size, std::memory_order_relaxed);
            return m_base + wrap_aligned;
        }

        return nullptr; // Out of memory
    } else {
        // Space strictly between head and tail
        if (aligned_head + size < tail) {
            m_head.store(aligned_head + size, std::memory_order_relaxed);
            return m_base + aligned_head;
        }

        return nullptr; // Out of memory
    }
}

void RingAllocator::deallocate(void* /*ptr*/) {
    // Linear ring buffers do not support arbitrary individual deallocations.
}

void RingAllocator::free_up_to(usize marker) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (marker <= m_capacity) {
        m_tail.store(marker, std::memory_order_relaxed);
    }
}

usize RingAllocator::used() const {
    const usize head = m_head.load(std::memory_order_relaxed);
    const usize tail = m_tail.load(std::memory_order_relaxed);
    if (head >= tail) {
        return head - tail;
    } else {
        return (m_capacity - tail) + head;
    }
}

} // namespace nf
