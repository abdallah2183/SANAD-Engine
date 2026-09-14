// JobTests/test_jobs.cpp

#include <NF/Test/TestFramework.hpp>
#include <NF/Jobs/JobSystem.hpp>

#include <chrono>
#include <thread>

using namespace nf;

NF_TEST(test_job_system_init) {
    JobSystem::instance().init(2);
    NF_CHECK(JobSystem::instance().is_initialized());
    NF_CHECK(JobSystem::instance().thread_count() >= 2);
    JobSystem::instance().shutdown();
}

NF_TEST(test_job_single) {
    JobSystem::instance().init(2);

    std::atomic<bool> done(false);
    JobSystem::instance().enqueue([&] { done.store(true); });

    // Wait for it
    while (!done.load()) {
        std::this_thread::yield();
    }

    NF_CHECK(done.load());
    JobSystem::instance().shutdown();
}

NF_TEST(test_job_parallel_for) {
    JobSystem::instance().init(4);

    constexpr usize N = 1000;
    std::vector<i32> results(N, 0);

    JobSystem::instance().parallel_for(N, 64, [&](usize start, usize end) {
        for (usize i = start; i < end; ++i) {
            results[i] = static_cast<i32>(i);
        }
    });

    // Check all values were set
    for (usize i = 0; i < N; ++i) {
        NF_CHECK_EQ(results[i], static_cast<i32>(i));
    }

    JobSystem::instance().shutdown();
}

NF_TEST(test_job_dispatch_and_wait) {
    JobSystem::instance().init(2);

    std::atomic<i32> counter(0);

    DynamicArray<std::function<void()>> tasks;
    for (int i = 0; i < 100; ++i) {
        tasks.push_back([&counter] { counter.fetch_add(1); });
    }

    JobSystem::instance().dispatch_and_wait(std::move(tasks));

    NF_CHECK_EQ(counter.load(), 100);
    JobSystem::instance().shutdown();
}

// JobGroup was never exercised by this suite, which is how it shipped with
// add() incrementing the pending counter without ever handing it to the job —
// so wait() spun forever and the group was unusable by anyone who waited on it.
NF_TEST(test_job_group_wait_returns) {
    JobSystem::instance().init(2);

    JobGroup group;
    std::atomic<u32> ran{0};

    for (u32 i = 0; i < 8; ++i) {
        group.add([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }

    // Bounded poll before calling wait(): a group whose counter is never
    // decremented would hang wait() forever, and a hanging test blocks the whole
    // suite instead of failing it. Polling first turns the hang into a failure.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (group.pending() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    NF_CHECK_EQ(ran.load(), 8u);
    NF_CHECK_EQ(group.pending(), 0u);

    // Must return rather than spin.
    group.wait();

    JobSystem::instance().shutdown();
}
