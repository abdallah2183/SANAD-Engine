// Tests/RuntimeTests/test_impact_events.cpp — Phase 49: frame-local impacts.
//
// `last_hit` keeps one value; a hit marker, a spark and a sound need every
// impact of the frame. These cases pin the two-slot contract and the hard cap
// without a device: current fills, begin_frame moves it to previous, and the
// oldest events are the ones a flood drops.

#include <NF/Test/TestFramework.hpp>
#include <NF/Runtime/Impact.hpp>

using namespace nf;
using namespace nf::runtime;

namespace {

ImpactEvent impact(u32 id, f32 x = 0.0f) {
    ImpactEvent event;
    event.shooter = ecs::Entity{1u, 0u};
    event.victim = ecs::Entity{id, 0u};
    event.victim_name = "Target";
    event.position = Vec3{x, 1.0f, 2.0f};
    event.normal = Vec3{0.0f, 1.0f, 0.0f};
    event.distance = 10.0f;
    event.damage = 12.0f;
    event.applied = 12.0f;
    return event;
}

} // namespace

NF_TEST(impact_buffer_keeps_frame_order_and_moves_on_begin_frame) {
    ImpactBuffer buffer;
    buffer.push(impact(2u, 1.0f));
    buffer.push(impact(3u, 2.0f));
    NF_CHECK_EQ(buffer.current().size(), 2u);
    NF_CHECK(buffer.previous().empty());
    NF_CHECK_EQ(buffer.current()[0].victim.id, 2u);
    NF_CHECK_EQ(buffer.current()[1].victim.id, 3u);

    buffer.begin_frame();
    NF_CHECK(buffer.current().empty());
    NF_CHECK_EQ(buffer.previous().size(), 2u);
    NF_CHECK_EQ(buffer.previous()[0].victim.id, 2u);
    NF_CHECK_EQ(buffer.previous()[1].victim.id, 3u);

    // The next frame replaces previous with the new completed frame; it does
    // not append to it forever.
    buffer.push(impact(4u, 3.0f));
    buffer.begin_frame();
    NF_CHECK(buffer.current().empty());
    NF_CHECK_EQ(buffer.previous().size(), 1u);
    NF_CHECK_EQ(buffer.previous()[0].victim.id, 4u);
}

NF_TEST(impact_buffer_drops_the_oldest_events_at_the_cap) {
    ImpactBuffer buffer;
    const u32 extra = 20u;
    for (u32 i = 0; i < ImpactBuffer::kCapacity + extra; ++i) {
        buffer.push(impact(i));
    }
    NF_CHECK_EQ(buffer.current().size(), ImpactBuffer::kCapacity);
    NF_CHECK_EQ(buffer.current().front().victim.id, extra);
    NF_CHECK_EQ(buffer.current().back().victim.id, ImpactBuffer::kCapacity + extra - 1u);
}

NF_TEST(impact_buffer_clear_drops_both_slots) {
    ImpactBuffer buffer;
    buffer.push(impact(1u));
    buffer.begin_frame();
    buffer.push(impact(2u));
    buffer.clear();
    NF_CHECK(buffer.current().empty());
    NF_CHECK(buffer.previous().empty());
}

NF_TEST(impact_event_carries_the_damage_and_team_verdict) {
    ImpactEvent blocked = impact(9u);
    blocked.applied = 0.0f;
    blocked.blocked_by_faction = true;
    blocked.killed = false;

    ImpactBuffer buffer;
    buffer.push(blocked);
    const ImpactEvent& stored = buffer.current().front();
    NF_CHECK(stored.blocked_by_faction);
    NF_CHECK_NEAR(stored.damage, 12.0f, 1e-5f);
    NF_CHECK_NEAR(stored.applied, 0.0f, 1e-5f);
    NF_CHECK(!stored.killed);
}
