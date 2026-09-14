// Tests/ECSTests/test_ecs.cpp — ECS core tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/ECS/ECS.hpp>

#include <string>

namespace {

using namespace nf;
using namespace nf::ecs;

struct Position { float x, y, z; };
struct Velocity { float vx, vy, vz; };
struct Health { int hp; };
struct TagA {};
struct TagB {};
struct TagC {};

} // namespace

NF_TEST(entity_generation_prevents_stale_handles) {
    World world;
    Entity e1 = world.create_entity();
    NF_CHECK(e1.valid());
    u32 id1 = e1.id;
    u32 gen1 = e1.generation;
    world.destroy_entity(e1);
    NF_CHECK(!world.is_alive(e1));
    Entity e2 = world.create_entity();
    // Should reuse id but with new generation
    NF_CHECK(e2.id == id1);
    NF_CHECK(e2.generation != gen1);
    NF_CHECK(world.is_alive(e2));
    NF_CHECK(!world.is_alive(e1)); // stale handle must be dead
    // Old handle should not be able to add components
    world.add<Health>(e2, Health{100});
    NF_CHECK(!world.has<Health>(e1));
    NF_CHECK(world.has<Health>(e2));
}

NF_TEST(component_add_remove) {
    World world;
    Entity e = world.create_entity();
    NF_CHECK(!world.has<Position>(e));
    world.add<Position>(e, Position{1,2,3});
    NF_CHECK(world.has<Position>(e));
    auto* p = world.get<Position>(e);
    NF_CHECK(p && p->x==1 && p->y==2 && p->z==3);
    p->x = 5;
    NF_CHECK(world.get<Position>(e)->x==5);
    world.remove<Position>(e);
    NF_CHECK(!world.has<Position>(e));
    NF_CHECK(world.get<Position>(e)==nullptr);
    // Re-add
    world.add<Position>(e, Position{7,8,9});
    NF_CHECK(world.get<Position>(e)->x==7);
}

NF_TEST(query_single_component) {
    World world;
    for (int i=0;i<5;++i){
        Entity e = world.create_entity();
        world.add<Position>(e, Position{float(i),0,0});
        if (i%2==0) world.add<Health>(e, Health{100});
    }
    auto with_pos = world.query<Position>();
    NF_CHECK(with_pos.size()==5);
    auto with_health = world.query<Health>();
    NF_CHECK(with_health.size()==3); // 0,2,4
}

NF_TEST(query_multi_component) {
    World world;
    for (int i=0;i<6;++i){
        Entity e = world.create_entity();
        world.add<Position>(e, Position{float(i),0,0});
        if (i<4) world.add<Velocity>(e, Velocity{1,0,0});
        if (i%2==0) world.add<Health>(e, Health{100});
    }
    auto pos_vel = world.query<Position, Velocity>();
    NF_CHECK(pos_vel.size()==4);
    auto pos_health = world.query<Position, Health>();
    NF_CHECK(pos_health.size()==3);
    auto all_three = world.query<Position, Velocity, Health>();
    NF_CHECK(all_three.size()==2); // i=0,2
}

NF_TEST(query_with_without) {
    World world;
    for (int i=0;i<4;++i){
        Entity e = world.create_entity();
        world.add<Position>(e, Position{float(i),0,0});
        if (i==0 || i==1) world.add<TagA>(e);
        if (i==1 || i==2) world.add<TagB>(e);
    }
    // With TagA
    auto with_a = world.query<TagA>();
    NF_CHECK(with_a.size()==2);
    // Without TagB: entities with Position but without TagB
    // Our minimal query doesn't have explicit Without, so we filter manually
    auto all_pos = world.query<Position>();
    size_t without_b = 0;
    for (Entity e : all_pos) if (!world.has<TagB>(e)) ++without_b;
    NF_CHECK(without_b==2); // i=0,3
    // With TagA without TagB
    size_t with_a_without_b=0;
    for (Entity e : world.query<TagA>()) if (!world.has<TagB>(e)) ++with_a_without_b;
    NF_CHECK(with_a_without_b==1); // i=0
}

NF_TEST(parallel_system_dependencies) {
    World world;
    // Create 10 entities with Position
    for (int i=0;i<10;++i){
        Entity e = world.create_entity();
        world.add<Position>(e, Position{float(i),0,0});
    }
    int counter_a=0, counter_b=0, counter_c=0, counter_d=0;
    SystemScheduler scheduler;
    scheduler.add_system({"A", {}, [&](World& w){ counter_a++; for (auto e : w.query<Position>()) w.get<Position>(e)->x += 1; }});
    scheduler.add_system({"B", {"A"}, [&](World& w){ counter_b++; for (auto e : w.query<Position>()) w.get<Position>(e)->y += 1; }});
    // C and D both depend on B, but not on each other — they could run in parallel
    scheduler.add_system({"C", {"B"}, [&](World& w){ (void)w; counter_c++; }});
    scheduler.add_system({"D", {"B"}, [&](World& w){ (void)w; counter_d++; }});
    scheduler.add_system({"E", {"C","D"}, [&](World& w){ (void)w; }});
    scheduler.execute(world);
    NF_CHECK(counter_a==1 && counter_b==1 && counter_c==1 && counter_d==1);
    // Verify that all entities were modified by A and B
    for (Entity e : world.query<Position>()) {
        auto* p = world.get<Position>(e);
        NF_CHECK(p->x >= 1 && p->y == 1);
    }
}
