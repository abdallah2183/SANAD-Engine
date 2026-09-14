// NF/Jobs/JobSystem.cpp

#include <NF/Jobs/JobSystem.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Assert.hpp>

#include <algorithm>
#include <random>

namespace nf {

// --- WorkerThread ---

WorkerThread::WorkerThread(u32 thread_id) : m_id(thread_id) {}

void WorkerThread::start() {
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread([this] { run(); });
}

void WorkerThread::stop() {
    m_running.store(false, std::memory_order_release);
    {
        std::lock_guard lock(m_cv_mutex);
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void WorkerThread::push_job(Job job) {
    {
        std::lock_guard lock(m_queue_mutex);
        m_queue.push_back(std::move(job));
    }
    {
        std::lock_guard lock(m_cv_mutex);
    }
    m_cv.notify_one();
}

bool WorkerThread::pop_job(Job& out) {
    std::lock_guard lock(m_queue_mutex);
    if (m_queue.empty()) return false;
    out = std::move(m_queue.back());
    m_queue.pop_back();
    return true;
}

bool WorkerThread::steal_job(Job& out) {
    std::lock_guard lock(m_queue_mutex);
    if (m_queue.empty()) return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void WorkerThread::run() {
    NF_LOG_DEBUG(LogCategory::Jobs, "Worker thread {} started", m_id);

    while (m_running.load(std::memory_order_acquire)) {
        // Wait for work or stop signal
        std::unique_lock cv_lock(m_cv_mutex);
        m_cv.wait_for(cv_lock, std::chrono::milliseconds(1), [this] {
            return !m_queue.empty() || !m_running.load(std::memory_order_acquire);
        });
        cv_lock.unlock();

        // Try to execute any local jobs
        while (true) {
            Job job;
            if (pop_job(job)) {
                if (job.task) {
                    job.task();
                }
                if (job.pending_counter) {
                    job.pending_counter->fetch_sub(1, std::memory_order_acq_rel);
                }
            } else {
                break;
            }
        }
    }

    NF_LOG_DEBUG(LogCategory::Jobs, "Worker thread {} stopped", m_id);
}

// --- JobGroup ---

void JobGroup::add(std::function<void()> task, JobPriority priority) {
    // The counter has to travel *with* the job: wait() spins until it reaches
    // zero, and a worker only decrements a counter it was handed. Enqueueing the
    // bare task left m_pending incremented forever, so wait() never returned and
    // the group was unusable by any caller that actually waited on it.
    m_pending.fetch_add(1, std::memory_order_relaxed);
    JobSystem::instance().enqueue(Job(std::move(task), priority, &m_pending));
}

void JobGroup::wait() {
    while (m_pending.load(std::memory_order_acquire) > 0) {
        // Yield to avoid spinning
        std::this_thread::yield();
    }
}

// --- JobSystem ---

JobSystem& JobSystem::instance() {
    static JobSystem js;
    return js;
}

void JobSystem::init(u32 thread_count) {
    NF_ASSERT(!m_initialized.load(), "JobSystem already initialized");

    if (thread_count == 0) {
        thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        // Leave one core for the main thread
        if (thread_count > 1) --thread_count;
    }

    m_workers.reserve(thread_count);
    for (u32 i = 0; i < thread_count; ++i) {
        m_workers.emplace_back(std::make_unique<WorkerThread>(i));
    }

    m_running.store(true, std::memory_order_release);

    for (auto& w : m_workers) {
        w->start();
    }

    m_initialized.store(true, std::memory_order_release);
    NF_LOG_INFO(LogCategory::Jobs, "JobSystem initialized with {} workers", thread_count);
}

void JobSystem::shutdown() {
    if (!m_initialized.load()) return;

    m_running.store(false, std::memory_order_release);
    m_initialized.store(false, std::memory_order_release);

    for (auto& w : m_workers) {
        w->stop();
    }
    m_workers.clear();

    NF_LOG_INFO(LogCategory::Jobs, "JobSystem shut down");
}

void JobSystem::enqueue(Job job) {
    NF_ASSERT(m_initialized.load(), "JobSystem not initialized");

    // Round-robin initial assignment, then work stealing handles the rest
    u32 target;
    {
        std::lock_guard lock(m_enqueue_mutex);
        target = m_next_worker;
        m_next_worker = (m_next_worker + 1) % m_workers.size();
    }

    m_workers[target]->push_job(std::move(job));
}

void JobSystem::enqueue(std::function<void()> task, JobPriority priority) {
    enqueue(Job(std::move(task), priority));
}

void JobSystem::dispatch(DynamicArray<std::function<void()>> tasks) {
    for (auto& t : tasks) {
        enqueue(std::move(t));
    }
}

void JobSystem::dispatch_and_wait(DynamicArray<std::function<void()>> tasks) {
    std::atomic<u32> pending(static_cast<u32>(tasks.size()));

    for (auto& t : tasks) {
        Job job;
        job.task = std::move(t);
        job.priority = JobPriority::Normal;
        job.pending_counter = &pending;
        enqueue(std::move(job));
    }

    while (pending.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

void JobSystem::parallel_for(usize count, usize batch_size,
                             std::function<void(usize, usize)> fn) {
    if (count == 0) return;
    if (batch_size == 0) batch_size = 1;

    usize num_batches = (count + batch_size - 1) / batch_size;
    std::atomic<u32> pending(static_cast<u32>(num_batches));

    for (usize b = 0; b < num_batches; ++b) {
        usize start = b * batch_size;
        usize end = std::min(start + batch_size, count);

        Job job;
        job.task = [fn, start, end, &pending]() {
            fn(start, end);
            pending.fetch_sub(1, std::memory_order_acq_rel);
        };
        enqueue(std::move(job));
    }

    while (pending.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

bool JobSystem::try_steal(Job& out, u32 exclude_worker) {
    // Try stealing from random workers (excluding the specified one)
    u32 count = static_cast<u32>(m_workers.size());
    if (count <= 1) return false;

    // Simple sequential scan for now; can be randomized later
    for (u32 i = 0; i < count; ++i) {
        if (i == exclude_worker) continue;
        if (m_workers[i]->steal_job(out)) {
            return true;
        }
    }
    return false;
}

void JobSystem::set_worker_affinity(u32 /*worker_id*/) {
    // TODO: implement thread affinity
}

u32 JobSystem::get_current_worker() const {
    // TODO: implement thread-local worker tracking
    return 0;
}

// --- JobScope ---

JobScope::JobScope(const char* label) : m_label(label) {
    NF_LOG_TRACE(LogCategory::Jobs, "JobScope start: {}", m_label);
}

JobScope::~JobScope() {
    NF_LOG_TRACE(LogCategory::Jobs, "JobScope end: {}", m_label);
}

} // namespace nf
