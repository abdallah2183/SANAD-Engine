// Tests/ECSTests/test_benchmarks.cpp — Performance benchmarks for ECS

#include <NF/Test/TestFramework.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace nf;
using namespace nf::ecs;

struct Position { float x, y, z; };
struct Velocity { float vx, vy, vz; };

// Baseline measured 2026-09-14 on the dev machine (MSVC 14.51.36231, build/verify
// — a Debug build with /Od, so these are Debug numbers and are slow by design).
// Values are nanoseconds per entity:
//
//   10K : create  547, query 555, add/remove 471, destroy 243
//   100K: create  528, query 552, add/remove 473, destroy 237
//   1M  : create  530, query 831, add/remove 767, destroy 418   (cache-bound)
//
// The ceiling is ~12x the worst of those. That is deliberate: this is a
// regression detector, not a performance target. It catches a query that quietly
// became O(n^2), or storage that started copying, and it does not fail because CI
// ran on a throttled shared runner. A tighter bound would be more sensitive and
// also flaky — and a flaky test gets muted, after which it detects nothing.
constexpr double kMaxNsPerEntity = 10'000.0;

void check_ns_per_entity(double elapsed_ms, size_t count, const char* phase,
                         size_t ops_per_entity = 1) {
    const double ns = elapsed_ms * 1'000'000.0 /
                      (static_cast<double>(count) * static_cast<double>(ops_per_entity));
    if (!(ns < kMaxNsPerEntity)) {
        throw std::runtime_error(std::string("benchmark regression: ") + phase + " took " +
                                 std::to_string(static_cast<long long>(ns)) +
                                 " ns/entity, ceiling is " +
                                 std::to_string(static_cast<long long>(kMaxNsPerEntity)));
    }
}

void benchmark_world(World& world, size_t count, const char* label) {
    Clock clock;
    // Create
    clock.reset();
    for (size_t i=0;i<count;++i){
        Entity e = world.create_entity();
        world.add<Position>(e, Position{float(i),0,0});
        if (i%2==0) world.add<Velocity>(e, Velocity{1,0,0});
    }
    double create_ms = clock.elapsed_ms();

    // Query iteration
    clock.reset();
    size_t sum = 0;
    for (int iter=0; iter<10; ++iter){
        auto with_pos = world.query<Position>();
        sum += with_pos.size();
        auto with_both = world.query<Position, Velocity>();
        sum += with_both.size();
    }
    double query_ms = clock.elapsed_ms();

    // Component add/remove
    clock.reset();
    auto entities = world.all_entities();
    for (Entity e : entities) {
        if (world.has<Velocity>(e)) world.remove<Velocity>(e);
        else world.add<Velocity>(e, Velocity{});
    }
    double add_remove_ms = clock.elapsed_ms();

    // Destroy
    clock.reset();
    auto all = world.all_entities();
    for (Entity e : all) world.destroy_entity(e);
    double destroy_ms = clock.elapsed_ms();

    // Assert before reporting, so a regression is a failure rather than a number
    // in a log that nobody reads. `query` covers 10 iterations, hence the
    // ops_per_entity multiplier.
    check_ns_per_entity(create_ms, count, "create");
    check_ns_per_entity(query_ms, count, "query", 10);
    check_ns_per_entity(add_remove_ms, count, "add/remove");
    check_ns_per_entity(destroy_ms, count, "destroy");

    // Metrics go to stdout unconditionally: the test runner defaults to
    // set_min_level(Warn), which would silently swallow an Info-level report
    // and leave a "green" run with no observable numbers (green-by-skip).
    std::printf("[Benchmark] %s: %zu entities — create %.2fms, query %.2fms (sum %zu), "
                "add/remove %.2fms, destroy %.2fms, memory ~%zu KB\n",
                label, count, create_ms, query_ms, sum, add_remove_ms, destroy_ms,
                (count * (sizeof(Position) + sizeof(Entity))) / 1024);
    std::fflush(stdout);

    // Ensure world is empty after
    NF_CHECK(world.alive_entity_count()==0);
}

} // namespace

NF_TEST(benchmark_10k_entities) {
    World world;
    benchmark_world(world, 10'000, "10K");
}

NF_TEST(benchmark_100k_entities) {
    World world;
    benchmark_world(world, 100'000, "100K");
}

// 1M is heavy for CI, so it is opt-in — but opt-in means SKIPPED, not passed.
NF_TEST(benchmark_1m_entities) {
    World world;
    bool has_env = false;
#ifdef _MSC_VER
    char* buf = nullptr; size_t sz = 0;
    if (_dupenv_s(&buf, &sz, "NF_BENCH_1M")==0 && buf) { has_env = true; std::free(buf); }
#else
    has_env = std::getenv("NF_BENCH_1M") != nullptr;
#endif
    if (!has_env) {
        NF_SKIP("set NF_BENCH_1M=1 to run the 1M entity benchmark");
    }
    benchmark_world(world, 1'000'000, "1M");
}
