// Tests/ECSTests/test_benchmarks.cpp — Performance benchmarks for ECS

#include <NF/Test/TestFramework.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>

#include <vector>

namespace {

using namespace nf;
using namespace nf::ecs;

struct Position { float x, y, z; };
struct Velocity { float vx, vy, vz; };

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

    NF_LOG_INFO(LogCategory::Core, "[Benchmark] {}: {} entities — create {:.2f}ms, query {:.2f}ms (sum {}), add/remove {:.2f}ms, destroy {:.2f}ms, memory ~{} KB",
                label, count, create_ms, query_ms, sum, add_remove_ms, destroy_ms,
                (count * (sizeof(Position)+sizeof(Entity))) / 1024);

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
