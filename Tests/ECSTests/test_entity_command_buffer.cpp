#include <NF/Test/TestFramework.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/ECS/EntityCommandBuffer.hpp>

namespace {

using namespace nf;
using namespace nf::ecs;

struct ECBPosition { float x = 0.0f, y = 0.0f, z = 0.0f; };
struct ECBVelocity { float vx = 0.0f, vy = 0.0f, vz = 0.0f; };
struct ECBHealth { int hp = 100; };

} // namespace

NF_TEST(ecb_create_and_playback) {
    World world;
    EntityCommandBuffer ecb;

    NF_CHECK(ecb.empty());
    Entity p1 = ecb.create_entity();
    Entity p2 = ecb.create_entity();

    NF_CHECK(EntityCommandBuffer::is_placeholder(p1));
    NF_CHECK(EntityCommandBuffer::is_placeholder(p2));
    NF_CHECK(p1 != p2);
    NF_CHECK_EQ(ecb.command_count(), 0u); // creating placeholders doesn't queue commands
    NF_CHECK(!ecb.empty());

    ecb.add_component(p1, ECBPosition{10.0f, 20.0f, 30.0f});
    ecb.add_component(p2, ECBHealth{75});

    NF_CHECK_EQ(ecb.command_count(), 2u);

    // World should be untouched before playback
    NF_CHECK_EQ(world.alive_entity_count(), 0u);

    ecb.playback(world);

    // After playback, world should have 2 live entities and ecb should be empty
    NF_CHECK_EQ(world.alive_entity_count(), 2u);
    NF_CHECK(ecb.empty());

    auto pos_entities = world.query<ECBPosition>();
    NF_CHECK_EQ(pos_entities.size(), 1u);
    const auto* pos = world.get<ECBPosition>(pos_entities[0]);
    NF_CHECK(pos != nullptr);
    NF_CHECK_NEAR(pos->x, 10.0f, 1e-5f);
    NF_CHECK_NEAR(pos->y, 20.0f, 1e-5f);
    NF_CHECK_NEAR(pos->z, 30.0f, 1e-5f);

    auto hp_entities = world.query<ECBHealth>();
    NF_CHECK_EQ(hp_entities.size(), 1u);
    const auto* hp = world.get<ECBHealth>(hp_entities[0]);
    NF_CHECK(hp != nullptr);
    NF_CHECK_EQ(hp->hp, 75);
}

NF_TEST(ecb_deferred_destroy_and_modify) {
    World world;
    Entity e1 = world.create_entity();
    Entity e2 = world.create_entity();
    world.add<ECBPosition>(e1, ECBPosition{1.0f, 2.0f, 3.0f});
    world.add<ECBPosition>(e2, ECBPosition{4.0f, 5.0f, 6.0f});

    EntityCommandBuffer ecb;
    // Destroy e1 and modify e2 via ECB
    ecb.destroy_entity(e1);
    ecb.set_component(e2, ECBPosition{100.0f, 200.0f, 300.0f});

    NF_CHECK_EQ(world.alive_entity_count(), 2u);
    NF_CHECK(world.is_alive(e1));

    ecb.playback(world);

    NF_CHECK_EQ(world.alive_entity_count(), 1u);
    NF_CHECK(!world.is_alive(e1));
    NF_CHECK(world.is_alive(e2));

    const auto* p = world.get<ECBPosition>(e2);
    NF_CHECK(p != nullptr);
    NF_CHECK_NEAR(p->x, 100.0f, 1e-5f);
}

NF_TEST(ecb_mutate_during_query) {
    World world;
    // Spawn 10 entities
    for (int i = 0; i < 10; ++i) {
        Entity e = world.create_entity();
        world.add<ECBPosition>(e, ECBPosition{static_cast<float>(i), 0.0f, 0.0f});
    }

    EntityCommandBuffer ecb;

    // Iterate query safely and push creations/destructions
    auto entities = world.query<ECBPosition>();
    for (Entity e : entities) {
        const auto* p = world.get<ECBPosition>(e);
        if (p->x < 5.0f) {
            ecb.destroy_entity(e);
        } else {
            // Spawn a companion entity
            Entity companion = ecb.create_entity();
            ecb.add_component(companion, ECBVelocity{p->x * 2.0f, 0.0f, 0.0f});
        }
    }

    ecb.playback(world);

    // 5 destroyed (0,1,2,3,4), 5 kept (5,6,7,8,9), and 5 companion entities created
    NF_CHECK_EQ(world.alive_entity_count(), 10u);
    NF_CHECK_EQ(world.query<ECBPosition>().size(), 5u);
    NF_CHECK_EQ(world.query<ECBVelocity>().size(), 5u);
}

NF_TEST(ecb_custom_action) {
    World world;
    EntityCommandBuffer ecb;
    bool action_called = false;

    ecb.add_action([&action_called](World& w) {
        action_called = true;
        Entity e = w.create_entity();
        w.add<ECBHealth>(e, ECBHealth{500});
    });

    NF_CHECK(!action_called);
    ecb.playback(world);
    NF_CHECK(action_called);
    NF_CHECK_EQ(world.alive_entity_count(), 1u);
    NF_CHECK_EQ(world.query<ECBHealth>().size(), 1u);
}
