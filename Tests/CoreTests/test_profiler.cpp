// CoreTests — hierarchical CPU profiler: nesting, aggregates, trace export.
//
// All timing assertions are relational (parent >= child, exclusive <=
// inclusive) — never absolute — so loaded CI machines cannot flake them.

#include <NF/Core/Profiler.hpp>
#include <NF/Test/TestFramework.hpp>

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace nf;

namespace {

bool has_zone(const std::vector<ProfileAggregate>& aggs, const std::string& name) {
    for (const auto& a : aggs) {
        if (a.name == name) return true;
    }
    return false;
}

const ProfileAggregate* find_zone(const std::vector<ProfileAggregate>& aggs,
                                  const std::string& name) {
    for (const auto& a : aggs) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

void burn(volatile int& sink, int n) {
    for (int i = 0; i < n; ++i) sink += i;
}

} // namespace

NF_TEST(profiler_nesting_and_exclusive_math) {
    Profiler::instance().reset_for_tests();
    volatile int sink = 0;
    {
        ProfileZone outer("outer");
        burn(sink, 1000);
        {
            ProfileZone inner("inner");
            burn(sink, 1000);
        }
        {
            ProfileZone inner("inner"); // second call: aggregation x2
            burn(sink, 1000);
        }
    }
    NF_CHECK(Profiler::instance().end_frame() == 3);

    const auto& aggs = Profiler::instance().last_aggregates();
    const auto* outer = find_zone(aggs, "outer");
    const auto* inner = find_zone(aggs, "inner");
    NF_CHECK(outer != nullptr);
    NF_CHECK(inner != nullptr);
    NF_CHECK(outer->calls == 1);
    NF_CHECK(inner->calls == 2);
    // Inclusive contains the children; exclusive excludes them.
    NF_CHECK(outer->inclusive_us >= inner->inclusive_us);
    NF_CHECK(outer->exclusive_us <= outer->inclusive_us);
    NF_CHECK(inner->exclusive_us == inner->inclusive_us); // leaf: equal
    // Aggregates come out sorted by name (deterministic).
    for (usize i = 1; i < aggs.size(); ++i) {
        NF_CHECK(aggs[i - 1].name <= aggs[i].name);
    }
    // Events are sorted by start time.
    const auto& events = Profiler::instance().last_events();
    NF_CHECK(events.size() == 3);
    for (usize i = 1; i < events.size(); ++i) {
        NF_CHECK(events[i - 1].start_us <= events[i].start_us);
    }
}

NF_TEST(profiler_frame_boundary_resets) {
    Profiler::instance().reset_for_tests();
    {
        ProfileZone a("frame_a");
    }
    NF_CHECK(Profiler::instance().end_frame() == 1);
    NF_CHECK(Profiler::instance().frame_index() == 1);
    // Next frame starts empty; the old events are replaced, not appended.
    NF_CHECK(Profiler::instance().end_frame() == 0);
    NF_CHECK(Profiler::instance().last_events().empty());
    NF_CHECK(Profiler::instance().frame_index() == 2);
}

NF_TEST(profiler_macros_record_function_zones) {
    Profiler::instance().reset_for_tests();
    {
        NF_PROFILE_SCOPE("macro_scope");
        volatile int sink = 0;
        burn(sink, 100);
    }
    NF_CHECK(Profiler::instance().end_frame() == 1);
    NF_CHECK(has_zone(Profiler::instance().last_aggregates(), "macro_scope"));
}

NF_TEST(profiler_merges_worker_threads) {
    Profiler::instance().reset_for_tests();
    std::thread worker([] {
        ProfileZone z("worker_zone");
        volatile int sink = 0;
        burn(sink, 500);
    });
    worker.join();
    {
        ProfileZone z("main_zone");
    }
    NF_CHECK(Profiler::instance().end_frame() == 2);
    NF_CHECK(has_zone(Profiler::instance().last_aggregates(), "worker_zone"));
    NF_CHECK(has_zone(Profiler::instance().last_aggregates(), "main_zone"));
}

NF_TEST(profiler_chrome_trace_export) {
    Profiler::instance().reset_for_tests();
    {
        ProfileZone outer("trace\"outer");
        ProfileZone inner("trace_inner");
    }
    Profiler::instance().end_frame();
    const auto tmp = std::filesystem::temp_directory_path() / "nf_test_trace.json";
    std::string err;
    NF_CHECK(Profiler::instance().save_chrome_trace(tmp.string(), err));
    NF_CHECK(err.empty());

    // The file is JSON with complete ("X") events and escaped names.
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    fopen_s(&f, tmp.string().c_str(), "rb");
#else
    f = std::fopen(tmp.string().c_str(), "rb");
#endif
    NF_CHECK(f != nullptr);
    std::string content;
    if (f) {
        char buf[1024];
        usize n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
        std::fclose(f);
    }
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    NF_CHECK(content.find("\"traceEvents\"") != std::string::npos);
    NF_CHECK(content.find("\"ph\":\"X\"") != std::string::npos);
    NF_CHECK(content.find("trace\\\"outer") != std::string::npos); // escaped quote
    NF_CHECK(content.find("\"ts\"") != std::string::npos);

    std::string err2;
    NF_CHECK(!Profiler::instance().save_chrome_trace("", err2));
    NF_CHECK(!err2.empty());
}
