#pragma once

// NF/Core/Threading.hpp — Threading primitives

#include <NF/Core/Types.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
    #include <emmintrin.h>
    #define NF_HAS_PAUSE 1
#else
    #define NF_HAS_PAUSE 0
#endif

namespace nf {

// Thread-safe queue
template<typename T>
class ConcurrentQueue {
public:
    void push(T value) {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push(std::move(value));
        }
        m_cv.notify_one();
    }

    bool try_pop(T& out) {
        std::lock_guard lock(m_mutex);
        if (m_queue.empty()) return false;
        out = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    bool pop(T& out) {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_queue.empty(); });
        out = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    bool empty() const {
        std::lock_guard lock(m_mutex);
        return m_queue.empty();
    }

    usize size() const {
        std::lock_guard lock(m_mutex);
        return m_queue.size();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<T> m_queue;
};

// Spinlock — low-overhead lock for very short critical sections
class Spinlock {
public:
    void lock() {
        while (m_flag.test_and_set(std::memory_order_acquire)) {
            // Spin
#if NF_HAS_PAUSE
            _mm_pause(); // x86 pause hint
#else
            std::this_thread::yield();
#endif
        }
    }

    void unlock() {
        m_flag.clear(std::memory_order_release);
    }

    bool try_lock() {
        return !m_flag.test_and_set(std::memory_order_acquire);
    }

private:
    std::atomic_flag m_flag{};
};

// Read-write spinlock
class RWSpinlock {
public:
    void lock_read() {
        u32 expected;
        do {
            expected = m_count.load(std::memory_order_acquire);
            if (expected != WRITE_LOCK) {
                if (m_count.compare_exchange_weak(expected, expected + 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                    return;
                }
            }
#if NF_HAS_PAUSE
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        } while (true);
    }

    void unlock_read() {
        m_count.fetch_sub(1, std::memory_order_release);
    }

    void lock_write() {
        u32 expected = 0;
        while (!m_count.compare_exchange_weak(expected, WRITE_LOCK,
            std::memory_order_acquire, std::memory_order_relaxed)) {
            expected = 0;
#if NF_HAS_PAUSE
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
    }

    void unlock_write() {
        m_count.store(0, std::memory_order_release);
    }

private:
    static constexpr u32 WRITE_LOCK = 0x80000000u;
    std::atomic<u32> m_count{0};
};

// Thread pool (lightweight, used by Core — full job system is in NFJobs)
class ThreadPool {
public:
    explicit ThreadPool(u32 thread_count = 0);
    ~ThreadPool();

    void enqueue(std::function<void()> task);

    void wait_all();

    u32 thread_count() const { return static_cast<u32>(m_threads.size()); }

private:
    void worker();

    std::vector<std::thread> m_threads;
    ConcurrentQueue<std::function<void()>> m_tasks;
    std::atomic<bool> m_running{true};
    std::atomic<u32> m_active_tasks{0};
    std::mutex m_wait_mutex;
    std::condition_variable m_wait_cv;
};

// Thread-local utility
u32 current_thread_id();

} // namespace nf
