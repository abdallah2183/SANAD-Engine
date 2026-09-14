#pragma once

// NF/Jobs/JobSystem.hpp — Job system with work-stealing scheduler
// Design doc Sections 91: job dependencies, priorities, cancellation,
// work stealing, task groups, fibers optional.

#include <NF/Core/Types.hpp>
#include <NF/Core/Threading.hpp>
#include <NF/Core/Containers.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nf {

enum class JobPriority : u8 {
    High   = 0,
    Normal = 1,
    Low    = 2,
    Count  = 3
};

// Job — a unit of work
struct Job {
    std::function<void()> task;
    JobPriority priority = JobPriority::Normal;
    std::atomic<u32>*  pending_counter = nullptr; // Optional: decremented when done

    Job() = default;
    Job(std::function<void()> t, JobPriority p = JobPriority::Normal,
        std::atomic<u32>* counter = nullptr)
        : task(std::move(t)), priority(p), pending_counter(counter) {}
};

// JobGroup — a group of jobs that can be waited on collectively
class JobGroup {
public:
    JobGroup() = default;

    void add(std::function<void()> task, JobPriority priority = JobPriority::Normal);
    void wait();

    u32 pending() const { return m_pending.load(std::memory_order_acquire); }

private:
    std::atomic<u32> m_pending{0};
};

// WorkerThread — each worker has its own deque and can steal from others
class WorkerThread {
public:
    explicit WorkerThread(u32 thread_id);

    void start();
    void stop();

    void push_job(Job job);
    bool pop_job(Job& out);

    // Steal a job from this worker's deque
    bool steal_job(Job& out);

    u32 id() const { return m_id; }

    std::deque<Job>& queue() { return m_queue; }
    std::mutex& queue_mutex() { return m_queue_mutex; }

    u32 queue_size() const { return static_cast<u32>(m_queue.size()); }

private:
    void run();

    u32 m_id;
    std::deque<Job> m_queue;
    mutable std::mutex m_queue_mutex;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::condition_variable m_cv;
    std::mutex m_cv_mutex;
};

// JobSystem — singleton scheduler
class JobSystem {
public:
    static JobSystem& instance();

    void init(u32 thread_count = 0);
    void shutdown();

    void enqueue(Job job);
    void enqueue(std::function<void()> task, JobPriority priority = JobPriority::Normal);

    // Convenience: dispatch a batch and wait
    void dispatch(DynamicArray<std::function<void()>> tasks);
    void dispatch_and_wait(DynamicArray<std::function<void()>> tasks);

    // Parallel for
    void parallel_for(usize count, usize batch_size, std::function<void(usize, usize)> fn);

    u32 thread_count() const { return static_cast<u32>(m_workers.size()); }

    // Set affinity: subsequent enqueued jobs from this thread prefer to run here
    void set_worker_affinity(u32 worker_id);
    u32 get_current_worker() const;

    bool is_initialized() const { return m_initialized.load(std::memory_order_acquire); }

private:
    JobSystem() = default;

    void worker_main(u32 worker_id);

    // Find a job to steal from other workers
    bool try_steal(Job& out, u32 exclude_worker);

    DynamicArray<std::unique_ptr<WorkerThread>> m_workers;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};
    u32 m_next_worker = 0;
    std::mutex m_enqueue_mutex;
};

// Scoped profiling for jobs
class JobScope {
public:
    explicit JobScope(const char* label);
    ~JobScope();
private:
    const char* m_label;
};

// Convenience macros
#define NF_JOB_SCHEDULE(fn) ::nf::JobSystem::instance().enqueue(fn)
#define NF_JOB_SCHEDULE_PRIORITY(fn, pri) ::nf::JobSystem::instance().enqueue(fn, pri)

} // namespace nf
