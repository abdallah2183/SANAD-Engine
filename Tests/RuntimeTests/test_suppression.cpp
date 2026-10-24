// Tests/RuntimeTests/test_suppression.cpp — Phase 55: near misses matter.
//
// Suppression is a small deterministic state: shots add, time removes, and the
// level slows the turn, narrows the fire cone and eventually holds the trigger.
// These cases pin the arithmetic without a device; the runtime integration is
// covered where a real agent can be placed in a scene.

#include <NF/Test/TestFramework.hpp>
#include <NF/Runtime/CombatAI.hpp>
#include <NF/Runtime/Suppression.hpp>

#include <cmath>
#include <limits>

using namespace nf;
using namespace nf::runtime;

namespace {

constexpr f32 kEps = 1e-4f;

CombatAimConfig quick_config() {
    CombatAimConfig c;
    c.reaction_seconds = 0.0f;
    c.aim_speed_deg_per_s = 100.0f;
    c.fire_cone_deg = 3.0f;
    c.max_range = 100.0f;
    c.burst_on_seconds = 1.0f;
    c.burst_off_seconds = 0.5f;
    return c;
}

CombatAimInput target_left() {
    CombatAimInput in;
    in.has_target = true;
    in.eye = Vec3{0.0f, 0.0f, 0.0f};
    in.target = Vec3{-10.0f, 0.0f, 0.0f}; // yaw +90
    in.visible = true;
    in.dt = 0.5f;
    return in;
}

} // namespace

NF_TEST(suppression_adds_caps_and_decays) {
    SuppressionState state;
    suppression_add(state, 0.5f);
    NF_CHECK_NEAR(state.level, 0.5f, kEps);
    suppression_add(state, 1.0f);
    NF_CHECK_NEAR(state.level, 1.0f, kEps);
    suppression_add(state, -1.0f);
    NF_CHECK_NEAR(state.level, 1.0f, kEps);

    suppression_step(state, 1.0f);
    NF_CHECK_NEAR(state.level, 1.0f - kSuppressionDecayPerSecond, kEps);
    suppression_step(state, 100.0f);
    NF_CHECK_NEAR(state.level, 0.0f, kEps);
    suppression_step(state, -1.0f);
    NF_CHECK_NEAR(state.level, 0.0f, kEps);
}

NF_TEST(distance_to_a_ray_handles_ahead_behind_and_beyond) {
    const Vec3 origin{0.0f, 0.0f, 0.0f};
    const Vec3 forward{0.0f, 0.0f, -1.0f};
    NF_CHECK_NEAR(distance_point_to_ray(Vec3{2.0f, 0.0f, -5.0f}, origin, forward, 100.0f),
                  2.0f, kEps);
    // Behind the origin: the closest point is the origin itself.
    NF_CHECK_NEAR(distance_point_to_ray(Vec3{2.0f, 0.0f, 5.0f}, origin, forward, 100.0f),
                  std::sqrt(4.0f + 25.0f), kEps);
    // The ray ends before the point.
    NF_CHECK_NEAR(distance_point_to_ray(Vec3{0.0f, 0.0f, -10.0f}, origin, forward, 5.0f),
                  5.0f, kEps);
    // A zero direction is a point.
    NF_CHECK_NEAR(distance_point_to_ray(Vec3{3.0f, 4.0f, 0.0f}, origin, Vec3{}, 100.0f),
                  5.0f, kEps);
    NF_CHECK(std::isinf(distance_point_to_ray(Vec3{std::nanf(""), 0.0f, 0.0f}, origin,
                                              forward, 100.0f)));
}

NF_TEST(suppression_slows_the_turn) {
    CombatAimConfig config = quick_config();
    CombatAimState calm;
    CombatAimInput in = target_left();
    const CombatAimOutput normal = combat_aim_step(calm, config, in);
    NF_CHECK_NEAR(normal.yaw_deg, 50.0f, 1e-2f); // 100 deg/s * 0.5 s

    CombatAimState pinned;
    in.suppression = 1.0f;
    const CombatAimOutput suppressed = combat_aim_step(pinned, config, in);
    NF_CHECK_NEAR(suppressed.yaw_deg, 25.0f, 1e-2f); // half speed
    NF_CHECK(suppressed.error_deg > normal.error_deg);
}

NF_TEST(high_suppression_holds_the_trigger_even_when_aimed) {
    CombatAimConfig config = quick_config();
    CombatAimState state;
    CombatAimInput in;
    in.has_target = true;
    in.eye = Vec3{0.0f, 0.0f, 0.0f};
    in.target = Vec3{0.0f, 0.0f, -10.0f}; // already ahead
    in.visible = true;
    in.dt = 0.1f;

    in.suppression = 0.0f;
    NF_CHECK(combat_aim_step(state, config, in).trigger);

    state = CombatAimState{};
    in.suppression = kSuppressionFireBlock;
    const CombatAimOutput blocked = combat_aim_step(state, config, in);
    NF_CHECK(blocked.aimed);
    NF_CHECK(!blocked.trigger);

    // Below the block threshold the agent may still fire if aimed.
    state = CombatAimState{};
    in.suppression = 0.5f;
    NF_CHECK(combat_aim_step(state, config, in).trigger);
}
