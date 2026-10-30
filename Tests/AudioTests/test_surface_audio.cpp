// Tests/AudioTests/test_surface_audio.cpp — Surface Material Audio Matrix tests (Phase 120)

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/SurfaceAudio.hpp>

using namespace nf;
using namespace nf::audio;

NF_TEST(surface_audio_default_lookup) {
    SurfaceAudioMatrix matrix;
    const auto& config = matrix.get_interaction_config(SurfaceType::Concrete, SurfaceInteraction::FootstepWalk);
    NF_CHECK(config.volume_multiplier > 0.0f);
    NF_CHECK(config.cue.variant_count() > 0);
}

NF_TEST(surface_audio_register_interaction) {
    SurfaceAudioMatrix matrix;

    SurfaceInteractionConfig cfg;
    cfg.cue.set_name("wood_walk");
    cfg.cue.add_variant("audio/footstep_wood_01.wav");
    cfg.cue.add_variant("audio/footstep_wood_02.wav");
    cfg.volume_multiplier = 0.85f;

    matrix.set_interaction_cue(SurfaceType::Wood, SurfaceInteraction::FootstepWalk, cfg);

    const auto& retrieved = matrix.get_interaction_config(SurfaceType::Wood, SurfaceInteraction::FootstepWalk);
    NF_CHECK_EQ(retrieved.cue.name(), "wood_walk");
    NF_CHECK_EQ(retrieved.cue.variant_count(), 2u);
    NF_CHECK_NEAR(retrieved.volume_multiplier, 0.85f, 1e-4f);
}

NF_TEST(surface_audio_footstep_speed_scaling) {
    SurfaceAudioMatrix matrix;

    SoundCueResult walk = matrix.evaluate_footstep(SurfaceType::Dirt, 1.5f);
    SoundCueResult run  = matrix.evaluate_footstep(SurfaceType::Dirt, 5.0f);

    NF_CHECK(walk.final_volume > 0.0f);
    NF_CHECK(run.final_volume > walk.final_volume);
    NF_CHECK(run.final_pitch >= walk.final_pitch);
}

NF_TEST(surface_audio_footstep_too_slow_silent) {
    SurfaceAudioMatrix matrix;
    SoundCueResult res = matrix.evaluate_footstep(SurfaceType::Concrete, 0.1f);
    NF_CHECK_NEAR(res.final_volume, 0.0f, 1e-4f);
}

NF_TEST(surface_audio_footstep_wetness_redirection) {
    SurfaceAudioMatrix matrix;

    SurfaceInteractionConfig dry_stone;
    dry_stone.cue.set_name("stone_dry");
    dry_stone.cue.add_variant("audio/stone_dry.wav");
    matrix.set_interaction_cue(SurfaceType::Concrete, SurfaceInteraction::FootstepWalk, dry_stone);

    SurfaceInteractionConfig splash_water;
    splash_water.cue.set_name("water_splash");
    splash_water.cue.add_variant("audio/water_splash.wav");
    matrix.set_interaction_cue(SurfaceType::Water, SurfaceInteraction::FootstepWalk, splash_water);

    // High wetness (0.9f) should pick up water footstep
    SoundCueResult wet_res = matrix.evaluate_footstep(SurfaceType::Concrete, 2.0f, 0.5f, 0.9f);
    NF_CHECK_EQ(wet_res.sound_name, "audio/water_splash.wav");

    // Dry footstep (0.0f wetness) picks up stone footstep
    SoundCueResult dry_res = matrix.evaluate_footstep(SurfaceType::Concrete, 2.0f, 0.5f, 0.0f);
    NF_CHECK_EQ(dry_res.sound_name, "audio/stone_dry.wav");
}

NF_TEST(surface_audio_collision_impact) {
    SurfaceAudioMatrix matrix;

    SurfaceInteractionConfig metal_on_stone;
    metal_on_stone.cue.set_name("metal_stone_hit");
    metal_on_stone.cue.add_variant("audio/impact_metal_stone.wav");
    metal_on_stone.min_velocity = 1.0f;
    metal_on_stone.max_velocity = 12.0f;

    matrix.set_collision_cue(SurfaceType::Metal, SurfaceType::Concrete, metal_on_stone);

    SoundCueResult res = matrix.evaluate_impact(SurfaceType::Metal, SurfaceType::Concrete, 6.0f);
    NF_CHECK_EQ(res.sound_name, "audio/impact_metal_stone.wav");
    NF_CHECK(res.final_volume > 0.0f);

    // Symmetry check: evaluate Concrete against Metal produces the same sound
    SoundCueResult res_sym = matrix.evaluate_impact(SurfaceType::Concrete, SurfaceType::Metal, 6.0f);
    NF_CHECK_EQ(res_sym.sound_name, "audio/impact_metal_stone.wav");
}

NF_TEST(surface_audio_impact_velocity_threshold) {
    SurfaceAudioMatrix matrix;

    SurfaceInteractionConfig cfg;
    cfg.min_velocity = 2.0f;
    matrix.set_interaction_cue(SurfaceType::Default, SurfaceInteraction::WeaponImpact, cfg);

    // Velocity 0.5 < min_velocity 2.0 -> inaudible
    SoundCueResult res = matrix.evaluate_impact(SurfaceType::Default, SurfaceType::Default, 0.5f);
    NF_CHECK_NEAR(res.final_volume, 0.0f, 1e-4f);
}

NF_TEST(surface_audio_impact_velocity_scaling) {
    SurfaceAudioMatrix matrix;

    SoundCueResult low_hit  = matrix.evaluate_impact(SurfaceType::Wood, SurfaceType::Wood, 2.0f);
    SoundCueResult high_hit = matrix.evaluate_impact(SurfaceType::Wood, SurfaceType::Wood, 8.0f);

    NF_CHECK(high_hit.final_volume > low_hit.final_volume);
    NF_CHECK(high_hit.final_pitch >= low_hit.final_pitch);
}

NF_TEST(surface_audio_surface_type_strings) {
    NF_CHECK_EQ(SurfaceAudioMatrix::surface_to_string(SurfaceType::Concrete), "Concrete");
    NF_CHECK_EQ(SurfaceAudioMatrix::surface_to_string(SurfaceType::Metal), "Metal");
    NF_CHECK_EQ(SurfaceAudioMatrix::surface_to_string(SurfaceType::Water), "Water");

    NF_CHECK_EQ(static_cast<u8>(SurfaceAudioMatrix::string_to_surface("Concrete")), static_cast<u8>(SurfaceType::Concrete));
    NF_CHECK_EQ(static_cast<u8>(SurfaceAudioMatrix::string_to_surface("Metal")), static_cast<u8>(SurfaceType::Metal));
    NF_CHECK_EQ(static_cast<u8>(SurfaceAudioMatrix::string_to_surface("Unknown")), static_cast<u8>(SurfaceType::Default));
}

NF_TEST(surface_audio_interaction_type_strings) {
    NF_CHECK_EQ(SurfaceAudioMatrix::interaction_to_string(SurfaceInteraction::FootstepWalk), "FootstepWalk");
    NF_CHECK_EQ(SurfaceAudioMatrix::interaction_to_string(SurfaceInteraction::BulletImpact), "BulletImpact");

    NF_CHECK_EQ(static_cast<u8>(SurfaceAudioMatrix::string_to_interaction("FootstepRun")), static_cast<u8>(SurfaceInteraction::FootstepRun));
    NF_CHECK_EQ(static_cast<u8>(SurfaceAudioMatrix::string_to_interaction("BulletImpact")), static_cast<u8>(SurfaceInteraction::BulletImpact));
}
