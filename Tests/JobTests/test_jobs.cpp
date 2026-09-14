// JobTests/test_jobs.cpp

#include <NF/Test/TestFramework.hpp>
#include <NF/Jobs/JobSystem.hpp>

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
