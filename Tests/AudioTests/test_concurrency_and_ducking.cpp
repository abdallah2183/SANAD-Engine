// Tests/AudioTests/test_concurrency_and_ducking.cpp — Audio Concurrency & Bus Ducking Unit Tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioConcurrency.hpp>
#include <NF/Audio/AudioDucking.hpp>

#include <cmath>

using namespace nf;
using namespace nf::audio;

NF_TEST(audio_concurrency_basic_limiting_and_prevent_new) {
    AudioConcurrencyManager mgr;

    ConcurrencyGroup grp;
    grp.name = "Footsteps";
    grp.max_instances = 2;
    grp.steal_policy = VoiceStealPolicy::PreventNew;
    u32 grp_id = mgr.register_group(grp);

    u64 stolen_id = 0;
    // Voice 1
    bool ok1 = mgr.request_voice(grp_id, 101, "step_grass_1", 1.0f, 0.0f, std::nullopt, stolen_id);
    NF_CHECK(ok1);
    NF_CHECK_EQ(stolen_id, 0u);
    NF_CHECK_EQ(mgr.active_group_voices(grp_id), 1u);

    // Voice 2
    bool ok2 = mgr.request_voice(grp_id, 102, "step_grass_2", 1.0f, 0.0f, std::nullopt, stolen_id);
    NF_CHECK(ok2);
    NF_CHECK_EQ(stolen_id, 0u);
    NF_CHECK_EQ(mgr.active_group_voices(grp_id), 2u);

    // Voice 3: should be rejected
    bool ok3 = mgr.request_voice(grp_id, 103, "step_grass_3", 1.0f, 0.0f, std::nullopt, stolen_id);
    NF_CHECK(!ok3);
    NF_CHECK_EQ(mgr.active_group_voices(grp_id), 2u);

    // Release Voice 1
    mgr.release_voice(101);
    NF_CHECK_EQ(mgr.active_group_voices(grp_id), 1u);

    // Now Voice 3 is accepted
    bool ok3_retry = mgr.request_voice(grp_id, 103, "step_grass_3", 1.0f, 0.0f, std::nullopt, stolen_id);
    NF_CHECK(ok3_retry);
    NF_CHECK_EQ(mgr.active_group_voices(grp_id), 2u);
}

NF_TEST(audio_concurrency_stop_oldest_stealing) {
    AudioConcurrencyManager mgr;

    ConcurrencyGroup grp;
    grp.name = "Guns";
    grp.max_instances = 2;
    grp.steal_policy = VoiceStealPolicy::StopOldest;
    grp.voice_steal_fade_time = 0.05f; // 50ms fade
    u32 grp_id = mgr.register_group(grp);

    u64 stolen_id = 0;
    mgr.request_voice(grp_id, 1, "shot_1", 1.0f, 0.0f, std::nullopt, stolen_id);
    mgr.update(0.5f); // Voice 1 age = 0.5s

    mgr.request_voice(grp_id, 2, "shot_2", 1.0f, 0.0f, std::nullopt, stolen_id);
    mgr.update(0.2f); // Voice 1 age = 0.7s, Voice 2 age = 0.2s

    // Voice 3 steals oldest (Voice 1)
    bool ok3 = mgr.request_voice(grp_id, 3, "shot_3", 1.0f, 0.0f, std::nullopt, stolen_id);
    NF_CHECK(ok3);
    NF_CHECK_EQ(stolen_id, 1u);

    // Voice 1 should now be fading out
    NF_CHECK(mgr.is_voice_fading_out(1));
    NF_CHECK(!mgr.is_voice_fading_out(2));
    NF_CHECK(!mgr.is_voice_fading_out(3));

    // Mid-fade gain check
    mgr.update(0.025f);
    NF_CHECK(mgr.is_voice_active(1));
    NF_CHECK_NEAR(mgr.get_voice_fade_gain(1), 0.5f, 0.05f);

    // Complete fade out
    mgr.update(0.03f);
    NF_CHECK(!mgr.is_voice_active(1)); // Evicted!
    NF_CHECK(mgr.is_voice_active(2));
    NF_CHECK(mgr.is_voice_active(3));
}

NF_TEST(audio_concurrency_stop_farthest_and_quietest) {
    AudioConcurrencyManager mgr;

    // 1. Stop Farthest
    ConcurrencyGroup grp_farthest;
    grp_farthest.name = "Explosions";
    grp_farthest.max_instances = 2;
    grp_farthest.steal_policy = VoiceStealPolicy::StopFarthest;
    u32 g_far = mgr.register_group(grp_farthest);

    u64 stolen = 0;
    mgr.request_voice(g_far, 10, "exp_near", 1.0f, 5.0f, std::nullopt, stolen);
    mgr.request_voice(g_far, 20, "exp_far",  1.0f, 50.0f, std::nullopt, stolen);

    // Candidate at 15.0m: steals voice 20 (at 50m)
    bool ok = mgr.request_voice(g_far, 30, "exp_mid", 1.0f, 15.0f, std::nullopt, stolen);
    NF_CHECK(ok);
    NF_CHECK_EQ(stolen, 20u);

    // 2. Stop Quietest
    ConcurrencyGroup grp_quiet;
    grp_quiet.name = "Ambience";
    grp_quiet.max_instances = 2;
    grp_quiet.steal_policy = VoiceStealPolicy::StopQuietest;
    u32 g_quiet = mgr.register_group(grp_quiet);

    mgr.request_voice(g_quiet, 100, "wind_loud", 0.9f, 0.0f, std::nullopt, stolen);
    mgr.request_voice(g_quiet, 200, "drip_soft", 0.1f, 0.0f, std::nullopt, stolen);

    // Candidate at 0.5f: steals drip_soft (0.1f)
    ok = mgr.request_voice(g_quiet, 300, "cricket", 0.5f, 0.0f, std::nullopt, stolen);
    NF_CHECK(ok);
    NF_CHECK_EQ(stolen, 200u);
}

NF_TEST(audio_concurrency_priority_stealing) {
    AudioConcurrencyManager mgr;

    ConcurrencyGroup grp;
    grp.name = "Voices";
    grp.max_instances = 2;
    grp.steal_policy = VoiceStealPolicy::StopLowestPriority;
    u32 g_prio = mgr.register_group(grp);

    u64 stolen = 0;
    mgr.request_voice(g_prio, 1, "npc_bark", 1.0f, 0.0f, 100, stolen);
    mgr.request_voice(g_prio, 2, "story_dialogue", 1.0f, 0.0f, 200, stolen);

    // Candidate with priority 50: lower than all active (100 and 200) -> rejected
    bool ok_low = mgr.request_voice(g_prio, 3, "low_grunt", 1.0f, 0.0f, 50, stolen);
    NF_CHECK(!ok_low);
    NF_CHECK_EQ(stolen, 0u);

    // Candidate with priority 150: steals priority 100 (voice 1)
    bool ok_high = mgr.request_voice(g_prio, 4, "player_callout", 1.0f, 0.0f, 150, stolen);
    NF_CHECK(ok_high);
    NF_CHECK_EQ(stolen, 1u);
}

NF_TEST(audio_ducking_attack_hold_release_lifecycle) {
    AudioDuckingManager ducking;

    DuckingRule rule;
    rule.initiator_bus = BusId::Voice;
    rule.target_bus = BusId::Music;
    rule.duck_gain = 0.2f;        // Duck to 20%
    rule.attack_time = 0.1f;     // 100ms attack
    rule.hold_time = 0.2f;       // 200ms hold
    rule.release_time = 0.4f;    // 400ms release
    rule.threshold = 0.05f;
    ducking.add_rule(rule);

    f32 levels[kBusCount] = {0.0f};

    // 1. Idle initially
    ducking.update(0.016f, levels);
    NF_CHECK_NEAR(ducking.get_duck_multiplier(BusId::Music), 1.0f, 1e-4f);
    NF_CHECK(ducking.get_rule_state(0) == DuckingState::Idle);

    // 2. Voice starts speaking (level = 0.8f)
    levels[static_cast<u32>(BusId::Voice)] = 0.8f;

    // Step 50ms (half of attack): gain should drop to ~0.6f
    ducking.update(0.05f, levels);
    NF_CHECK(ducking.get_rule_state(0) == DuckingState::Attacking);
    NF_CHECK_NEAR(ducking.get_duck_multiplier(BusId::Music), 0.6f, 0.05f);

    // Step another 50ms: reaches target duck_gain (0.2f)
    ducking.update(0.05f, levels);
    NF_CHECK_NEAR(ducking.get_duck_multiplier(BusId::Music), 0.2f, 1e-3f);
    NF_CHECK(ducking.get_rule_state(0) == DuckingState::Holding);

    // 3. Voice finishes speaking (level = 0.0f)
    levels[static_cast<u32>(BusId::Voice)] = 0.0f;

    // Step 100ms (within 200ms hold): remains at 0.2f, state = Holding
    ducking.update(0.1f, levels);
    NF_CHECK(ducking.get_rule_state(0) == DuckingState::Holding);
    NF_CHECK_NEAR(ducking.get_duck_multiplier(BusId::Music), 0.2f, 1e-3f);

    // Step 150ms (hold timer expires, enters release)
    ducking.update(0.15f, levels);
    NF_CHECK(ducking.get_rule_state(0) == DuckingState::Releasing);
    NF_CHECK(ducking.get_duck_multiplier(BusId::Music) > 0.2f);

    // Advance 400ms (full release time elapsed)
    ducking.update(0.4f, levels);
    NF_CHECK_NEAR(ducking.get_duck_multiplier(BusId::Music), 1.0f, 1e-3f);
    NF_CHECK(ducking.get_rule_state(0) == DuckingState::Idle);
}

NF_TEST(audio_ducking_multiple_rules_and_reset) {
    AudioDuckingManager ducking;

    // Rule 1: Voice ducks Music to 0.4f
    DuckingRule r1;
    r1.initiator_bus = BusId::Voice;
    r1.target_bus = BusId::Music;
    r1.duck_gain = 0.4f;
    r1.attack_time = 0.01f;
    r1.hold_time = 0.1f;
    r1.release_time = 0.1f;
    ducking.add_rule(r1);

    // Rule 2: SFX (e.g. Explosion/Tinnitus) ducks Music to 0.1f
    DuckingRule r2;
    r2.initiator_bus = BusId::Sfx;
    r2.target_bus = BusId::Music;
    r2.duck_gain = 0.1f;
    r2.attack_time = 0.01f;
    r2.hold_time = 0.1f;
    r2.release_time = 0.1f;
    ducking.add_rule(r2);

    f32 levels[kBusCount] = {0.0f};

    // Both Voice and SFX are active
    levels[static_cast<u32>(BusId::Voice)] = 0.5f;
    levels[static_cast<u32>(BusId::Sfx)] = 0.9f;

    ducking.update(0.02f, levels);

    // Lowest gain (0.1f) must win for Music
    NF_CHECK_NEAR(ducking.get_duck_multiplier(BusId::Music), 0.1f, 1e-3f);

    // Reset restores everything
    ducking.reset();
    NF_CHECK_NEAR(ducking.get_duck_multiplier(BusId::Music), 1.0f, 1e-4f);
    NF_CHECK(ducking.get_rule_state(0) == DuckingState::Idle);
    NF_CHECK(ducking.get_rule_state(1) == DuckingState::Idle);
}
