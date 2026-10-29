#include <NF/Test/TestFramework.hpp>
#include <NF/Animation/AnimNotify.hpp>
#include <NF/Animation/AnimMontage.hpp>

using namespace nf;
using namespace nf::animation;

NF_TEST(timeline_instant_notify_detection) {
    AnimTimeline timeline;
    timeline.add_instant_notify("Footstep_R", 0.5f, "dirt", 101);

    // 1. Advance before trigger -> no event
    auto evts1 = timeline.advance(0.0f, 0.4f, 1.0f, false);
    NF_CHECK(evts1.empty());

    // 2. Advance crossing trigger -> 1 event
    auto evts2 = timeline.advance(0.4f, 0.6f, 1.0f, false);
    NF_CHECK_EQ(evts2.size(), 1u);
    NF_CHECK_EQ(evts2[0].name, "Footstep_R");
    NF_CHECK_NEAR(evts2[0].time, 0.5f, 1e-4f);
    NF_CHECK_EQ(evts2[0].payload, "dirt");
    NF_CHECK_EQ(evts2[0].user_data, 101);

    // 3. Advance after trigger -> no event
    auto evts3 = timeline.advance(0.6f, 1.0f, 1.0f, false);
    NF_CHECK(evts3.empty());
}

NF_TEST(timeline_state_notify_begin_and_end) {
    AnimTimeline timeline;
    // Window from 0.3s to 0.7s
    timeline.add_state_notify("MeleeHitbox", 0.3f, 0.4f, "sword_blade");

    // 1. Advance across start
    auto evts1 = timeline.advance(0.0f, 0.4f, 1.0f, false);
    NF_CHECK_EQ(evts1.size(), 1u);
    NF_CHECK(evts1[0].is_state_begin);
    NF_CHECK(!evts1[0].is_state_end);
    NF_CHECK_NEAR(evts1[0].time, 0.3f, 1e-4f);

    // 2. Advance inside window
    auto evts2 = timeline.advance(0.4f, 0.6f, 1.0f, false);
    NF_CHECK(evts2.empty());

    // 3. Advance across end
    auto evts3 = timeline.advance(0.6f, 0.8f, 1.0f, false);
    NF_CHECK_EQ(evts3.size(), 1u);
    NF_CHECK(!evts3[0].is_state_begin);
    NF_CHECK(evts3[0].is_state_end);
    NF_CHECK_NEAR(evts3[0].time, 0.7f, 1e-4f);
}

NF_TEST(timeline_loop_wrap_fires_notifies_across_boundary) {
    AnimTimeline timeline;
    timeline.add_instant_notify("TailEndEvent", 0.95f);
    timeline.add_instant_notify("EarlyEvent", 0.05f);

    // Advancing from 0.9s to 0.1s in a 1.0s loop wraps around
    auto evts = timeline.advance(0.9f, 0.1f, 1.0f, true);
    NF_CHECK_EQ(evts.size(), 2u);
    NF_CHECK_EQ(evts[0].name, "TailEndEvent");
    NF_CHECK_EQ(evts[1].name, "EarlyEvent");
}

NF_TEST(montage_section_playback_and_auto_transition) {
    AnimMontage montage("AttackCombo", 2.0f);
    montage.add_section("Slash1", 0.0f, 1.0f, "Slash2");
    montage.add_section("Slash2", 1.0f, 1.0f, ""); // Ends combo

    montage.play("Slash1");
    NF_CHECK(montage.is_playing());
    NF_CHECK_EQ(montage.current_section(), "Slash1");
    NF_CHECK_NEAR(montage.current_time(), 0.0f, 1e-4f);

    // Step 0.5s -> inside Slash1
    montage.advance(0.5f);
    NF_CHECK(montage.is_playing());
    NF_CHECK_EQ(montage.current_section(), "Slash1");
    NF_CHECK_NEAR(montage.current_time(), 0.5f, 1e-4f);

    // Step 0.7s (total 1.2s) -> seamlessly crosses into Slash2!
    montage.advance(0.7f);
    NF_CHECK(montage.is_playing());
    NF_CHECK_EQ(montage.current_section(), "Slash2");
    NF_CHECK_NEAR(montage.current_time(), 1.2f, 1e-4f);

    // Step 1.0s (past 2.0s) -> finishes Slash2 and stops
    montage.advance(1.0f);
    NF_CHECK(!montage.is_playing());
}

NF_TEST(montage_branching_and_interactive_jump) {
    AnimMontage montage("HeroLocomotion", 5.0f);
    montage.add_section("Idle", 0.0f, 1.0f, "Idle");
    montage.add_section("SpinAttack", 3.0f, 1.5f, "");

    montage.play("Idle");
    montage.advance(0.5f);
    NF_CHECK_EQ(montage.current_section(), "Idle");

    // Interrupt with SpinAttack
    bool jumped = montage.jump_to_section("SpinAttack");
    NF_CHECK(jumped);
    NF_CHECK_EQ(montage.current_section(), "SpinAttack");
    NF_CHECK_NEAR(montage.current_time(), 3.0f, 1e-4f);

    // Try jump to nonexistent section fails
    bool bad_jump = montage.jump_to_section("GhostMove");
    NF_CHECK(!bad_jump);
    NF_CHECK_EQ(montage.current_section(), "SpinAttack");
}
