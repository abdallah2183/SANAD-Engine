// Tests/RuntimeTests/test_shot_events.cpp — Phase 53: the shot itself.
//
// Impacts answer "what did the round hit". A muzzle flash, a tracer and a
// casing sound need the other half: what left the barrel, in which direction,
// and which pellet of the volley it was. These cases pin the frame-local
// contract without a device: current fills, begin_frame moves it to previous,
// and a flood drops the oldest shots first.

#include <NF/Test/TestFramework.hpp>
#include <NF/Runtime/Shot.hpp>

using namespace nf;
using namespace nf::runtime;

namespace {

ShotEvent shot(u32 id, f32 x = 0.0f) {
    ShotEvent event;
    event.shooter = ecs::Entity{id, 0u};
    event.origin = Vec3{x, 1.0f, 2.0f};
    event.direction = Vec3{0.0f, 0.0f, -1.0f};
    event.damage = 12.0f;
    event.pellet = 1u;
    event.pellets = 1u;
    return event;
}

} // namespace

NF_TEST(shot_buffer_keeps_order_and_moves_on_begin_frame) {
    ShotBuffer buffer;
    buffer.push(shot(2u, 1.0f));
    buffer.push(shot(3u, 2.0f));
    NF_CHECK_EQ(buffer.current().size(), 2u);
    NF_CHECK(buffer.previous().empty());
    NF_CHECK_EQ(buffer.current()[0].shooter.id, 2u);
    NF_CHECK_EQ(buffer.current()[1].shooter.id, 3u);

    buffer.begin_frame();
    NF_CHECK(buffer.current().empty());
    NF_CHECK_EQ(buffer.previous().size(), 2u);
    NF_CHECK_EQ(buffer.previous()[0].shooter.id, 2u);
    NF_CHECK_EQ(buffer.previous()[1].shooter.id, 3u);

    buffer.push(shot(4u));
    buffer.begin_frame();
    NF_CHECK_EQ(buffer.previous().size(), 1u);
    NF_CHECK_EQ(buffer.previous()[0].shooter.id, 4u);
}

NF_TEST(shot_buffer_drops_the_oldest_at_the_cap) {
    ShotBuffer buffer;
    const u32 extra = 20u;
    for (u32 i = 0; i < ShotBuffer::kCapacity + extra; ++i) {
        buffer.push(shot(i));
    }
    NF_CHECK_EQ(buffer.current().size(), ShotBuffer::kCapacity);
    NF_CHECK_EQ(buffer.current().front().shooter.id, extra);
    NF_CHECK_EQ(buffer.current().back().shooter.id, ShotBuffer::kCapacity + extra - 1u);
}

NF_TEST(shot_buffer_clear_drops_both_slots) {
    ShotBuffer buffer;
    buffer.push(shot(1u));
    buffer.begin_frame();
    buffer.push(shot(2u));
    buffer.clear();
    NF_CHECK(buffer.current().empty());
    NF_CHECK(buffer.previous().empty());
}

NF_TEST(a_shot_event_carries_the_pellet_and_the_spread_direction) {
    ShotEvent pellet;
    pellet.shooter = ecs::Entity{7u, 3u};
    pellet.origin = Vec3{1.0f, 2.0f, 3.0f};
    pellet.direction = Vec3{0.1f, 0.2f, -0.97f};
    pellet.damage = 8.5f;
    pellet.pellet = 3u;
    pellet.pellets = 8u;
    pellet.projectile = true;
    pellet.seed = 99u;

    ShotBuffer buffer;
    buffer.push(pellet);
    const ShotEvent& stored = buffer.current().front();
    NF_CHECK_EQ(stored.shooter.id, 7u);
    NF_CHECK_EQ(stored.pellet, 3u);
    NF_CHECK_EQ(stored.pellets, 8u);
    NF_CHECK(stored.projectile);
    NF_CHECK_EQ(stored.seed, 99u);
    NF_CHECK_NEAR(stored.direction.z, -0.97f, 1e-6f);
}
