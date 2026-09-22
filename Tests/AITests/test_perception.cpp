// AITests/perception — sight, hearing, touch and damage sensing.
//
// Every case is pure CPU: no Vulkan, no physics, no clock. Where occlusion
// matters the test supplies its own blocker, which is exactly the seam the
// module exposes so NavGrid or a physics raycast can be plugged in later
// without a recompile of this logic.

#include <NF/AI/Perception.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::ai;

namespace {

PerceptionTarget make_target(const Vec3& position, float noise = 0.0f,
                             float visibility = 1.0f, float radius = 0.5f) {
    PerceptionTarget t;
    t.position = position;
    t.noise = noise;
    t.visibility = visibility;
    t.radius = radius;
    return t;
}

/// A sensor at the origin looking down +X with the module's default senses.
Perception default_sensor() {
    Perception perc;
    perc.set_position(Vec3{});
    perc.set_forward(Vec3{1.0f, 0.0f, 0.0f});
    return perc;
}

} // namespace

NF_TEST(perception_scene_ids_are_never_recycled) {
    PerceptionScene scene;
    const u32 a = scene.add_target(make_target({1.0f, 0.0f, 0.0f}));
    const u32 b = scene.add_target(make_target({2.0f, 0.0f, 0.0f}));
    scene.remove_target(a);
    const u32 c = scene.add_target(make_target({3.0f, 0.0f, 0.0f}));
    NF_CHECK(a != b);
    NF_CHECK(b != c);
    NF_CHECK(c != a); // a's id is not handed to the new target
    NF_CHECK(c > b);  // ids only ever increase
    NF_CHECK(scene.targets().size() == 2);
}

NF_TEST(perception_scene_age_drops_expired_events_in_order) {
    PerceptionScene scene;
    scene.emit(Stimulus{StimulusKind::Sound, {1, 0, 0}, 1.0f, 1u, 2.0f});
    scene.emit(Stimulus{StimulusKind::Sound, {2, 0, 0}, 1.0f, 2u, 5.0f});
    scene.emit(Stimulus{StimulusKind::Sound, {3, 0, 0}, 1.0f, 3u, 1.0f});

    scene.age(2.5f);
    NF_CHECK(scene.stimuli().size() == 1);
    NF_CHECK(scene.stimuli().front().source_id == 2u); // emission order preserved
    NF_CHECK_NEAR(scene.stimuli().front().remaining, 2.5f, 1e-4f);
}

NF_TEST(perception_scene_clear_keeps_id_counter_advancing) {
    PerceptionScene scene;
    const u32 first = scene.add_target(make_target({}));
    scene.clear();
    const u32 after = scene.add_target(make_target({}));
    NF_CHECK(after != first);
    NF_CHECK(after > first);
    NF_CHECK(scene.targets().size() == 1);
    NF_CHECK(scene.stimuli().empty());
}

NF_TEST(perception_detects_target_in_the_sight_cone) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 2.0f); // falloff at 10 m of a 21 m reach ~ 0.52/s
    NF_CHECK(perc.is_sensing(enemy));
    NF_CHECK(perc.is_aware_of(enemy));

    const PerceivedTarget* pt = perc.find(enemy);
    NF_CHECK(pt != nullptr);
    NF_CHECK(pt->last_sense == StimulusKind::Sight);
    NF_CHECK_NEAR(pt->last_known_position.x, 10.0f, 1e-4f);
}

NF_TEST(perception_target_behind_is_invisible) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({-10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 10.0f); // plenty of time, still nothing
    NF_CHECK(!perc.is_aware_of(enemy));
    NF_CHECK(perc.perceived().empty()); // pruned: never any stimulus
}

NF_TEST(perception_target_beyond_range_is_invisible) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({100.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 10.0f);
    NF_CHECK(!perc.is_aware_of(enemy));
}

NF_TEST(perception_target_outside_fov_is_invisible) {
    PerceptionScene scene;
    const u32 inside = scene.add_target(make_target({10.0f, 10.0f, 0.0f}));
    const u32 outside = scene.add_target(make_target({10.0f, 20.0f, 0.0f}));

    Perception perc = default_sensor();
    // 45 degrees off-axis is inside the 120-degree cone but at 14 m the falloff
    // leaves only ~0.33/s, so it needs more than 3 s to fill.
    perc.update(scene, 5.0f);
    NF_CHECK(perc.is_aware_of(inside));
    NF_CHECK(!perc.is_aware_of(outside)); // 63 degrees, outside the cone
}

NF_TEST(perception_sight_blocker_occludes_a_visible_target) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.set_sight_blocker([](Vec3, Vec3) { return true; }); // a solid wall
    perc.update(scene, 10.0f);
    NF_CHECK(!perc.is_aware_of(enemy));
}

NF_TEST(perception_sight_blocker_can_be_selective) {
    PerceptionScene scene;
    const u32 near_id = scene.add_target(make_target({5.0f, 0.0f, 0.0f}));
    const u32 far_id = scene.add_target(make_target({15.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    // A wall at x = 10: blocks anything that has to cross it.
    perc.set_sight_blocker([](Vec3 a, Vec3 b) {
        const bool crosses = (a.x < 10.0f) != (b.x < 10.0f);
        return crosses;
    });
    perc.update(scene, 5.0f);
    NF_CHECK(perc.is_aware_of(near_id));
    NF_CHECK(!perc.is_aware_of(far_id));
}

NF_TEST(perception_low_visibility_delays_but_does_not_prevent_detection) {
    PerceptionScene scene;
    const u32 plain = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));
    const u32 hidden = scene.add_target(make_target({10.0f, 5.0f, 0.0f}, 0.0f, 0.2f));

    Perception perc = default_sensor();
    perc.update(scene, 3.0f);
    NF_CHECK(perc.is_aware_of(plain));
    NF_CHECK(!perc.is_aware_of(hidden)); // a quarter of the stimulus rate

    perc.update(scene, 8.0f);
    NF_CHECK(perc.is_aware_of(hidden)); // but it does get there
}

NF_TEST(perception_continuous_noise_detects_behind_the_sensor) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({-10.0f, 0.0f, 0.0f}, 2.0f));

    Perception perc = default_sensor();
    perc.update(scene, 3.0f);
    NF_CHECK(perc.is_aware_of(enemy));

    const PerceivedTarget* pt = perc.find(enemy);
    NF_CHECK(pt->last_sense == StimulusKind::Sound);
    // Heard, never seen: the module deliberately does NOT guess a position
    // from a noise source — nothing is known to pin.
    NF_CHECK(pt->last_known_position.length_sq() == 0.0f);
}

NF_TEST(perception_discrete_sound_pins_a_position) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({-10.0f, 0.0f, 0.0f}, 0.0f));
    scene.emit(Stimulus{StimulusKind::Sound, {4.0f, 0.0f, 0.0f}, 2.0f, enemy, 1.0f});

    Perception perc = default_sensor();
    perc.update(scene, 1.5f);
    NF_CHECK(perc.is_aware_of(enemy));

    const PerceivedTarget* pt = perc.find(enemy);
    NF_CHECK(pt->last_sense == StimulusKind::Sound);
    NF_CHECK_NEAR(pt->last_known_position.x, 4.0f, 1e-4f);
}

NF_TEST(perception_unattributed_stimulus_is_ignored) {
    PerceptionScene scene;
    scene.emit(Stimulus{StimulusKind::Sound, {1.0f, 0.0f, 0.0f}, 10.0f, 0u, 5.0f});

    Perception perc = default_sensor();
    perc.update(scene, 10.0f);
    NF_CHECK(perc.perceived().empty()); // source_id 0 = nobody to blame
}

NF_TEST(perception_out_of_range_sound_stimulus_is_ignored) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({100.0f, 0.0f, 0.0f}));
    scene.emit(Stimulus{StimulusKind::Sound, {100.0f, 0.0f, 0.0f}, 1.0f, enemy, 1.0f});

    Perception perc = default_sensor();
    perc.update(scene, 10.0f);
    NF_CHECK(!perc.is_aware_of(enemy));
}

NF_TEST(perception_touch_detects_contact) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({1.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 1.0f);
    NF_CHECK(perc.is_aware_of(enemy));

    const PerceivedTarget* pt = perc.find(enemy);
    NF_CHECK(pt->last_sense == StimulusKind::Touch);
}

NF_TEST(perception_touch_has_no_falloff) {
    PerceptionScene scene;
    const u32 near = scene.add_target(make_target({0.5f, 0.0f, 0.0f}));
    const u32 far = scene.add_target(make_target({3.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.sense(StimulusKind::Sight).range = 0.0f; // blind, so only touch can fire
    perc.update(scene, 0.1f);
    NF_CHECK(perc.is_sensing(near));
    NF_CHECK(!perc.is_sensing(far)); // outside the 2.5 m touch reach
}

NF_TEST(perception_damage_is_immediate_and_ignores_range) {
    PerceptionScene scene;
    const u32 sniper = scene.add_target(make_target({1000.0f, 0.0f, 0.0f}));
    scene.emit(Stimulus{StimulusKind::Damage, {0.0f, 0.0f, 0.0f}, 1.0f, sniper, 1.0f});

    Perception perc = default_sensor();
    perc.update(scene, 0.01f); // a single frame
    NF_CHECK(perc.is_aware_of(sniper));

    const PerceivedTarget* pt = perc.find(sniper);
    NF_CHECK(pt->last_sense == StimulusKind::Damage);
    // The hit landed, so a stimulus did arrive — but the sniper is 1 km away and
    // behind the sensor, so the sight meter never moved. That separation, not a
    // `sensed` flag, is what tells a BT "I was hurt, not seen".
    NF_CHECK(perc.is_sensing(sniper));
    NF_CHECK(pt->detection_of(StimulusKind::Sight) == 0.0f);
    NF_CHECK(pt->detection_of(StimulusKind::Damage) == 1.0f);
}

NF_TEST(perception_partial_detection_is_not_aware) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 1.0f);
    NF_CHECK(!perc.is_aware_of(enemy));
    NF_CHECK(perc.is_sensing(enemy));
    NF_CHECK(perc.perceived().size() == 1);

    const PerceivedTarget* pt = perc.find(enemy);
    NF_CHECK(pt->detection_of(StimulusKind::Sight) > 0.0f);
    NF_CHECK(pt->detection_of(StimulusKind::Sight) < 1.0f);
}

NF_TEST(perception_detection_decays_without_stimulus) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 2.0f);
    NF_CHECK(perc.is_aware_of(enemy));

    scene.remove_target(enemy);
    perc.update(scene, 1.0f); // loss is 0.5/s

    const PerceivedTarget* pt = perc.find(enemy);
    NF_CHECK(pt != nullptr);
    NF_CHECK_NEAR(pt->detection_of(StimulusKind::Sight), 0.5f, 1e-3f);
    NF_CHECK(pt->aware); // certainty drains, knowledge lingers
}

NF_TEST(perception_awareness_lingers_then_the_target_is_forgotten) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 2.0f);
    NF_CHECK(perc.is_aware_of(enemy));

    scene.remove_target(enemy);
    perc.update(scene, 2.9f); // inside the linger window
    NF_CHECK(perc.is_aware_of(enemy));
    perc.update(scene, 0.2f); // past it
    NF_CHECK(!perc.is_aware_of(enemy));
    NF_CHECK(perc.perceived().empty());
}

NF_TEST(perception_linger_is_the_most_patient_sense_that_ever_noticed) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 2.0f);
    NF_CHECK(perc.is_aware_of(enemy));

    // A target only ever *seen* expires on sight's 3 s, not the damage sense's
    // 5 s — the search window belongs to the senses that actually noticed.
    scene.remove_target(enemy);
    perc.update(scene, 3.2f);
    NF_CHECK(!perc.is_aware_of(enemy));
    NF_CHECK(perc.perceived().empty());
}

NF_TEST(perception_linger_widens_when_a_more_patient_sense_fires) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 2.0f); // seen
    scene.emit(Stimulus{StimulusKind::Damage, {0.0f, 0.0f, 0.0f}, 1.0f, enemy, 1.0f});
    scene.age(0.01f); // the scene, not the sensor, owns event lifetimes
    perc.update(scene, 0.01f); // and now hurt
    NF_CHECK(perc.is_aware_of(enemy));

    // Sight's window would have closed at 3 s; the hit extends it to damage's 5.
    scene.remove_target(enemy);
    scene.age(3.5f);
    perc.update(scene, 3.5f);
    NF_CHECK(perc.is_aware_of(enemy));
    scene.age(1.6f);
    perc.update(scene, 1.6f);
    NF_CHECK(!perc.is_aware_of(enemy));
}

NF_TEST(perception_sight_overwrites_a_stale_noise_position) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));
    // A noise from where the target *was*...
    scene.emit(Stimulus{StimulusKind::Sound, {-5.0f, 0.0f, 0.0f}, 2.0f, enemy, 1.0f});

    Perception perc = default_sensor();
    // ...while a wall hides it, so only the rumour is recorded this tick.
    perc.set_sight_blocker([](Vec3 a, Vec3 b) { return (a.x < 10.0f) != (b.x < 10.0f); });
    perc.update(scene, 1.0f);
    NF_CHECK(perc.find(enemy)->last_known_position.x < 0.0f);

    // ...then the wall comes down and the target is in view.
    perc.set_sight_blocker(nullptr);
    perc.update(scene, 1.5f);
    NF_CHECK_NEAR(perc.find(enemy)->last_known_position.x, 10.0f, 1e-4f);
}

NF_TEST(perception_most_threatening_breaks_ties_by_first_seen) {
    PerceptionScene scene;
    const u32 first = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));
    const u32 second = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, 3.0f);
    NF_CHECK(perc.is_aware_of(first));
    NF_CHECK(perc.is_aware_of(second));

    const PerceivedTarget* threat = perc.most_threatening();
    NF_CHECK(threat != nullptr);
    NF_CHECK(threat->target_id == first); // identical readings, earlier wins

    // An unconfirmed rumour never outranks a confirmed sighting.
    const u32 noise_only = scene.add_target(make_target({-2.0f, 0.0f, 0.0f}, 4.0f));
    (void)noise_only;
    perc.update(scene, 0.5f);
    NF_CHECK(perc.most_threatening()->target_id == first);
}

NF_TEST(perception_replays_bit_identically) {
    auto run = [] {
        PerceptionScene scene;
        scene.add_target(make_target({10.0f, 0.0f, 0.0f})); // seen, never emits
        const u32 b = scene.add_target(make_target({-8.0f, 0.0f, 0.0f}, 1.5f));
        scene.emit(Stimulus{StimulusKind::Sound, {3.0f, 0.0f, 0.0f}, 2.0f, b, 1.0f});

        Perception perc = default_sensor();
        perc.set_sight_blocker([](Vec3, Vec3) { return false; });
        for (int i = 0; i < 10; ++i) {
            perc.update(scene, 0.5f);
            scene.age(0.5f);
        }
        return perc.perceived(); // value copy
    };

    const std::vector<PerceivedTarget> first = run();
    const std::vector<PerceivedTarget> second = run();
    NF_CHECK(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        NF_CHECK(first[i].target_id == second[i].target_id);
        NF_CHECK(first[i].aware == second[i].aware);
        NF_CHECK(first[i].sensed == second[i].sensed);
        NF_CHECK(first[i].last_sense == second[i].last_sense);
        for (u32 k = 0; k < kStimulusKindCount; ++k) {
            NF_CHECK(first[i].detection[k] == second[i].detection[k]); // exact equality
        }
    }
}

NF_TEST(perception_negative_dt_does_not_run_time_backwards) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));

    Perception perc = default_sensor();
    perc.update(scene, -100.0f); // a caller bug, not a crash
    NF_CHECK(!perc.is_aware_of(enemy));
    NF_CHECK(perc.is_sensing(enemy)); // the stimulus is real, only the clock is wrong
    NF_CHECK(perc.find(enemy)->silence == 0.0f);
}

NF_TEST(perception_zero_forward_blinds_a_sensor_looking_the_wrong_way) {
    PerceptionScene scene;
    const u32 behind = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));
    const u32 ahead = scene.add_target(make_target({0.0f, 0.0f, 10.0f}));

    Perception perc;
    perc.set_position(Vec3{});
    perc.set_forward(Vec3{}); // degenerate aim
    perc.update(scene, 10.0f);
    NF_CHECK(!perc.is_aware_of(behind)); // +Z default, not "see everything"
    NF_CHECK(perc.is_aware_of(ahead));
}

NF_TEST(perception_disabled_sense_is_dead) {
    PerceptionScene scene;
    const u32 enemy = scene.add_target(make_target({-3.0f, 0.0f, 0.0f}, 4.0f));

    Perception perc = default_sensor();
    perc.sense(StimulusKind::Sound).range = 0.0f; // deaf
    perc.update(scene, 10.0f);
    NF_CHECK(!perc.is_aware_of(enemy));
    NF_CHECK(perc.perceived().empty());
}

NF_TEST(perception_forget_drops_one_target_reset_drops_all) {
    PerceptionScene scene;
    const u32 a = scene.add_target(make_target({10.0f, 0.0f, 0.0f}));
    // Broadside to the sight cone, so this one is only ever *heard* — the point
    // of the test is memory management, not geometry.
    const u32 b = scene.add_target(make_target({0.0f, 0.0f, 10.0f}, 2.0f));

    Perception perc = default_sensor();
    perc.update(scene, 3.0f);
    NF_CHECK(perc.perceived().size() == 2);

    NF_CHECK(perc.forget(a));
    NF_CHECK(!perc.forget(a)); // already gone
    NF_CHECK(perc.perceived().size() == 1);
    NF_CHECK(perc.perceived().front().target_id == b); // order preserved

    perc.reset();
    NF_CHECK(perc.perceived().empty());
    NF_CHECK(!perc.is_aware_of(b));

    // Sense configs survive a reset.
    perc.update(scene, 3.0f);
    NF_CHECK(perc.is_aware_of(b));
}

NF_TEST(perception_target_sizes_extend_the_sense_reach) {
    PerceptionScene scene;
    // Would be out of touch reach (2.5 m) for a point target.
    const u32 big = scene.add_target(make_target({2.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 5.0f));

    Perception perc = default_sensor();
    perc.update(scene, 1.0f);
    NF_CHECK(perc.is_sensing(big));
    NF_CHECK(perc.find(big)->last_sense == StimulusKind::Touch);
}
