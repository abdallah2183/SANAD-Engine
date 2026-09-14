// NF/Core/Threading.cpp

#include <NF/Core/Threading.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sched.h>
#endif

namespace nf {

// --- ThreadPool ---

ThreadPool::ThreadPool(u32 thread_count) {
    if (thread_count == 0) {
        thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
    }

    m_threads.reserve(thread_count);
    for (u32 i = 0; i < thread_count; ++i) {
        m_threads.emplace_back([this] { worker(); });
    }
}

ThreadPool::~ThreadPool() {
    m_running.store(false, std::memory_order_release);
    // Push dummy tasks to wake all threads
    for (usize i = 0; i < m_threads.size(); ++i) {
        m_tasks.push([] {});
    }

    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::enqueue(std::function<void()> task) {
    m_active_tasks.fetch_add(1, std::memory_order_relaxed);
    m_tasks.push(std::move(task));
}

void ThreadPool::wait_all() {
    std::unique_lock lock(m_wait_mutex);
    m_wait_cv.wait(lock, [this] {
        return m_active_tasks.load(std::memory_order_acquire) == 0;
    });
}

void ThreadPool::worker() {
    while (m_running.load(std::memory_order_acquire)) {
        std::function<void()> task;
        if (m_tasks.pop(task)) {
            task();
            if (m_active_tasks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard lock(m_wait_mutex);
                m_wait_cv.notify_all();
            }
        }
    }
}

// --- Thread ID ---

static std::atomic<u32> s_thread_counter{0};
thread_local u32 s_thread_id = 0;

u32 current_thread_id() {
    if (s_thread_id == 0) {
        s_thread_id = ++s_thread_counter;
    }
    return s_thread_id;
}

} // namespace nf
