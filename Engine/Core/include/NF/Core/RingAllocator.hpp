#pragma once

// NF/Core/RingAllocator.hpp — Circular Ring Buffer Allocator for Transient Data
// High-throughput linear allocation for frame transient data, command buffers, and GPU staging.

#include <NF/Core/Types.hpp>
#include <NF/Core/Memory.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>

namespace nf {

class RingAllocator : public IAllocator {
public:
    explicit RingAllocator(usize capacity = 4 * MiB);
    ~RingAllocator() override;

    RingAllocator(const RingAllocator&) = delete;
    RingAllocator& operator=(const RingAllocator&) = delete;

    [[nodiscard]] void* allocate(usize size, usize alignment = 16) override;
    void deallocate(void* ptr) override; // No-op in ring allocator

    /// Releases all allocations up to `marker` (e.g. after a frame completes or GPU fence signals).
    void free_up_to(usize marker);

    usize get_marker() const { return m_head.load(std::memory_order_relaxed); }
    usize used() const;
    usize capacity() const { return m_capacity; }

    const char* name() const override { return "Ring"; }

private:
    byte* m_base = nullptr;
    usize m_capacity = 0;
    std::atomic<usize> m_head{0};
    std::atomic<usize> m_tail{0};
    mutable std::mutex m_mutex;
};

} // namespace nf
