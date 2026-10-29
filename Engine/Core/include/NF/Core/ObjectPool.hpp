#pragma once

#include <NF/Core/Types.hpp>
#include <vector>
#include <memory>
#include <mutex>
#include <utility>
#include <cassert>

namespace nf {

/// Cache-friendly, chunked object pool that minimizes dynamic allocations during runtime.
/// Provides O(1) acquire and release operations. Objects reside in stable memory blocks
/// so pointers remain valid across pool expansions.
template<typename T, size_t ChunkSize = 64>
class ObjectPool {
    static_assert(ChunkSize > 0, "ChunkSize must be greater than zero");

public:
    ObjectPool() = default;
    ~ObjectPool() {
        clear();
    }

    // Non-copyable, movable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    ObjectPool(ObjectPool&& other) noexcept
        : m_chunks(std::move(other.m_chunks))
        , m_free_list(std::move(other.m_free_list))
        , m_active_count(other.m_active_count)
        , m_capacity(other.m_capacity) {
        other.m_active_count = 0;
        other.m_capacity = 0;
    }

    ObjectPool& operator=(ObjectPool&& other) noexcept {
        if (this != &other) {
            clear();
            m_chunks = std::move(other.m_chunks);
            m_free_list = std::move(other.m_free_list);
            m_active_count = other.m_active_count;
            m_capacity = other.m_capacity;
            other.m_active_count = 0;
            other.m_capacity = 0;
        }
        return *this;
    }

    /// Acquires an object from the pool, default-constructing it.
    T* acquire() {
        if (m_free_list.empty()) {
            allocate_chunk();
        }
        T* ptr = m_free_list.back();
        m_free_list.pop_back();
        new (static_cast<void*>(ptr)) T();
        ++m_active_count;
        return ptr;
    }

    /// Constructs an object in-place using the supplied arguments.
    template<typename... Args>
    T* create(Args&&... args) {
        if (m_free_list.empty()) {
            allocate_chunk();
        }
        T* ptr = m_free_list.back();
        m_free_list.pop_back();
        new (static_cast<void*>(ptr)) T(std::forward<Args>(args)...);
        ++m_active_count;
        return ptr;
    }

    /// Releases an object back to the pool, invoking its destructor.
    void release(T* ptr) {
        if (!ptr) return;
        ptr->~T();
        m_free_list.push_back(ptr);
        if (m_active_count > 0) {
            --m_active_count;
        }
    }

    /// Pre-allocates memory for at least the specified number of objects.
    void reserve(size_t count) {
        while (m_capacity < count) {
            allocate_chunk();
        }
    }

    /// Destroys all active objects and resets the free list.
    void clear() {
        // We only destroy pool chunks. Note that any active pointers held by callers
        // will become invalid when their storage is deallocated.
        m_chunks.clear();
        m_free_list.clear();
        m_active_count = 0;
        m_capacity = 0;
    }

    size_t capacity() const { return m_capacity; }
    size_t active_count() const { return m_active_count; }
    size_t free_count() const { return m_free_list.size(); }
    bool empty() const { return m_active_count == 0; }

private:
    struct Chunk {
        alignas(T) u8 storage[ChunkSize * sizeof(T)];
    };

    void allocate_chunk() {
        auto chunk = std::make_unique<Chunk>();
        T* base = reinterpret_cast<T*>(chunk->storage);
        // Push items in reverse order so that contiguous indices are popped forward
        for (size_t i = ChunkSize; i > 0; --i) {
            m_free_list.push_back(base + (i - 1));
        }
        m_chunks.push_back(std::move(chunk));
        m_capacity += ChunkSize;
    }

    std::vector<std::unique_ptr<Chunk>> m_chunks;
    std::vector<T*> m_free_list;
    size_t m_active_count = 0;
    size_t m_capacity = 0;
};

/// Thread-safe variant of ObjectPool guarded by a mutex.
template<typename T, size_t ChunkSize = 64>
class ConcurrentObjectPool {
public:
    template<typename... Args>
    T* create(Args&&... args) {
        std::lock_guard lock(m_mutex);
        return m_pool.create(std::forward<Args>(args)...);
    }

    T* acquire() {
        std::lock_guard lock(m_mutex);
        return m_pool.acquire();
    }

    void release(T* ptr) {
        std::lock_guard lock(m_mutex);
        m_pool.release(ptr);
    }

    void reserve(size_t count) {
        std::lock_guard lock(m_mutex);
        m_pool.reserve(count);
    }

    size_t active_count() const {
        std::lock_guard lock(m_mutex);
        return m_pool.active_count();
    }

    size_t capacity() const {
        std::lock_guard lock(m_mutex);
        return m_pool.capacity();
    }

private:
    mutable std::mutex m_mutex;
    ObjectPool<T, ChunkSize> m_pool;
};

} // namespace nf
