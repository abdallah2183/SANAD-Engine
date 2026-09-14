#pragma once

// NF/Core/Memory.hpp — Memory management subsystem
// Provides allocators: general, frame (linear), pool, scratch.
// Design doc Section 89-90: allocators from day one, frame allocator for temporary data.

#include <NF/Core/Types.hpp>
#include <NF/Core/Assert.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <new>

namespace nf {

// --- Allocate aligned memory ---
void* aligned_alloc(usize size, usize alignment = 16);
void  aligned_free(void* ptr);

// --- IAllocator interface ---
class IAllocator {
public:
    virtual ~IAllocator() = default;

    [[nodiscard]] virtual void* allocate(usize size, usize alignment = 16) = 0;
    virtual void deallocate(void* ptr) = 0;

    virtual const char* name() const = 0;
};

// --- General allocator (wraps malloc/free) ---
class GeneralAllocator : public IAllocator {
public:
    [[nodiscard]] void* allocate(usize size, usize alignment) override;
    void deallocate(void* ptr) override;
    const char* name() const override { return "General"; }
};

// --- Frame allocator (linear arena, reset per frame) ---
// Thread-local. Fast O(1) alloc, no individual frees, bulk reset.
class FrameAllocator : public IAllocator {
public:
    explicit FrameAllocator(usize capacity = 2 * MiB);
    ~FrameAllocator() override;

    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;

    [[nodiscard]] void* allocate(usize size, usize alignment) override;
    void deallocate(void* ptr) override; // no-op

    void reset();

    usize used() const { return m_offset; }
    usize capacity() const { return m_capacity; }

    const char* name() const override { return "Frame"; }

private:
    byte* m_base = nullptr;
    usize m_capacity = 0;
    usize m_offset = 0;
};

// --- Pool allocator (fixed-size block allocation) ---
class PoolAllocator : public IAllocator {
public:
    PoolAllocator(usize block_size, usize block_count, usize alignment = 16);
    ~PoolAllocator() override;

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    [[nodiscard]] void* allocate(usize size, usize alignment) override;
    void deallocate(void* ptr) override;

    usize free_blocks() const { return m_free_count.load(std::memory_order_relaxed); }
    const char* name() const override { return "Pool"; }

private:
    struct FreeNode {
        FreeNode* next;
    };

    byte* m_base = nullptr;
    usize m_block_size = 0;
    usize m_block_count = 0;
    FreeNode* m_free_list = nullptr;
    std::atomic<usize> m_free_count{0};
    std::mutex m_mutex;
};

// --- Memory tracker (for debugging/profiling) ---
class MemoryTracker {
public:
    static MemoryTracker& instance();

    void track_allocation(usize size);
    void track_deallocation(usize size);

    struct Stats {
        std::atomic<usize> total_allocated{0};
        std::atomic<usize> total_deallocated{0};
        std::atomic<u64> allocation_count{0};
        std::atomic<u64> deallocation_count{0};
    };

    const Stats& stats() const { return m_stats; }

private:
    MemoryTracker() = default;
    Stats m_stats;
};

// --- Scoped allocator for new/delete tracking ---
struct MemoryScope {
    usize start_allocated = 0;
    usize start_count = 0;

    MemoryScope();
    ~MemoryScope();

    usize bytes_allocated() const;
    u64 alloc_count() const;
};

} // namespace nf

// --- Global new/delete overrides (optional, for tracking) ---
// We don't override global new/delete to avoid breaking third-party code.
// Engine code should use allocators explicitly.

// --- NF_NEW / NF_DELETE macros for allocator-aware allocation ---
#define NF_NEW(allocator, T) \
    new ((allocator).allocate(sizeof(T), alignof(T))) T

#define NF_DELETE(allocator, ptr) \
    do { \
        if (ptr) { \
            (ptr)->~T(); \
            (allocator).deallocate(ptr); \
        } \
    } while (0)

#define NF_NEW_ARRAY(allocator, T, count) \
    new ((allocator).allocate(sizeof(T) * (count), alignof(T))) T[(count)]

#define NF_DELETE_ARRAY(allocator, ptr) \
    do { \
        if (ptr) { \
            (allocator).deallocate(ptr); \
        } \
    } while (0)
