#include <NF/Audio/SurfaceAudio.hpp>

#include <algorithm>
#include <cmath>

namespace nf::audio {

SurfaceAudioMatrix::SurfaceAudioMatrix() {
    m_default_config.cue.set_name("default_surface_cue");
    m_default_config.cue.add_variant("audio/impact_default.wav", 1.0f, 1.0f, 1.0f);
    m_default_config.min_velocity = 0.5f;
    m_default_config.max_velocity = 8.0f;
    m_default_config.volume_multiplier = 1.0f;
}

void SurfaceAudioMatrix::set_interaction_cue(
    SurfaceType surface,
    SurfaceInteraction interaction,
    const SurfaceInteractionConfig& config
) {
    u32 key = make_interaction_key(surface, interaction);
    m_interactions[key] = config;
}

const SurfaceInteractionConfig& SurfaceAudioMatrix::get_interaction_config(
    SurfaceType surface,
    SurfaceInteraction interaction
) const {
    u32 key = make_interaction_key(surface, interaction);
    auto it = m_interactions.find(key);
    if (it != m_interactions.end()) {
        return it->second;
    }

    // Fallback to Default surface with requested interaction
    u32 fallback_key = make_interaction_key(SurfaceType::Default, interaction);
    auto fb_it = m_interactions.find(fallback_key);
    if (fb_it != m_interactions.end()) {
        return fb_it->second;
    }

    return m_default_config;
}

void SurfaceAudioMatrix::set_collision_cue(
    SurfaceType surface_a,
    SurfaceType surface_b,
    const SurfaceInteractionConfig& config
) {
    u32 key = make_collision_key(surface_a, surface_b);
    m_collisions[key] = config;
}

SoundCueResult SurfaceAudioMatrix::evaluate_footstep(
    SurfaceType surface,
    f32 character_speed,
    f32 rand_val,
    f32 surface_wetness
) {
    if (character_speed < 0.2f) {
        SoundCueResult silent;
        silent.final_volume = 0.0f;
        return silent;
    }

    SurfaceInteraction interaction = SurfaceInteraction::FootstepWalk;
    if (character_speed >= 6.0f) {
        interaction = SurfaceInteraction::FootstepSprint;
    } else if (character_speed >= 3.0f) {
        interaction = SurfaceInteraction::FootstepRun;
    }

    // High wetness redirects to Water footsteps
    SurfaceType actual_surface = (surface_wetness >= 0.75f) ? SurfaceType::Water : surface;
    const auto& config = get_interaction_config(actual_surface, interaction);

    SoundCueResult res;
    if (config.cue.variant_count() > 0) {
        res = const_cast<SoundCue&>(config.cue).evaluate(rand_val);
    } else {
        res.sound_name = "audio/footstep_default.wav";
        res.final_volume = 1.0f;
        res.final_pitch = 1.0f;
    }

    // Scale volume with speed
    f32 speed_norm = std::clamp(
        (character_speed - config.min_velocity) / (config.max_velocity - config.min_velocity),
        0.25f,
        1.25f
    );
    res.final_volume *= speed_norm * config.volume_multiplier;

    // Subtle pitch rise with sprinting
    f32 pitch_boost = (character_speed - 2.0f) * config.pitch_velocity_scaling;
    res.final_pitch = std::clamp(res.final_pitch + pitch_boost, 0.5f, 2.0f);

    return res;
}

SoundCueResult SurfaceAudioMatrix::evaluate_impact(
    SurfaceType surface_a,
    SurfaceType surface_b,
    f32 impact_velocity,
    f32 rand_val
) {
    if (impact_velocity <= 0.1f) {
        SoundCueResult silent;
        silent.final_volume = 0.0f;
        return silent;
    }

    u32 key = make_collision_key(surface_a, surface_b);
    auto it = m_collisions.find(key);

    const SurfaceInteractionConfig* config_ptr = nullptr;
    if (it != m_collisions.end()) {
        config_ptr = &it->second;
    } else {
        config_ptr = &get_interaction_config(surface_a, SurfaceInteraction::WeaponImpact);
    }

    const auto& config = *config_ptr;
    if (impact_velocity < config.min_velocity) {
        SoundCueResult silent;
        silent.final_volume = 0.0f;
        return silent;
    }

    SoundCueResult res;
    if (config.cue.variant_count() > 0) {
        res = const_cast<SoundCue&>(config.cue).evaluate(rand_val);
    } else {
        res.sound_name = "audio/impact_default.wav";
        res.final_volume = 1.0f;
        res.final_pitch = 1.0f;
    }

    f32 vel_norm = std::clamp(
        (impact_velocity - config.min_velocity) / (config.max_velocity - config.min_velocity),
        0.1f,
        1.5f
    );
    res.final_volume *= vel_norm * config.volume_multiplier;

    f32 pitch_shift = (impact_velocity - config.min_velocity) * config.pitch_velocity_scaling;
    res.final_pitch = std::clamp(res.final_pitch + pitch_shift, 0.5f, 2.5f);

    return res;
}

std::string SurfaceAudioMatrix::surface_to_string(SurfaceType surface) {
    switch (surface) {
        case SurfaceType::Concrete: return "Concrete";
        case SurfaceType::Wood:     return "Wood";
        case SurfaceType::Dirt:     return "Dirt";
        case SurfaceType::Grass:    return "Grass";
        case SurfaceType::Metal:    return "Metal";
        case SurfaceType::Water:    return "Water";
        case SurfaceType::Glass:    return "Glass";
        case SurfaceType::Sand:     return "Sand";
        case SurfaceType::Snow:     return "Snow";
        default:                    return "Default";
    }
}

SurfaceType SurfaceAudioMatrix::string_to_surface(const std::string& name) {
    if (name == "Concrete") return SurfaceType::Concrete;
    if (name == "Wood")     return SurfaceType::Wood;
    if (name == "Dirt")     return SurfaceType::Dirt;
    if (name == "Grass")    return SurfaceType::Grass;
    if (name == "Metal")    return SurfaceType::Metal;
    if (name == "Water")    return SurfaceType::Water;
    if (name == "Glass")    return SurfaceType::Glass;
    if (name == "Sand")     return SurfaceType::Sand;
    if (name == "Snow")     return SurfaceType::Snow;
    return SurfaceType::Default;
}

std::string SurfaceAudioMatrix::interaction_to_string(SurfaceInteraction interaction) {
    switch (interaction) {
        case SurfaceInteraction::FootstepWalk:   return "FootstepWalk";
        case SurfaceInteraction::FootstepRun:    return "FootstepRun";
        case SurfaceInteraction::FootstepSprint: return "FootstepSprint";
        case SurfaceInteraction::JumpLand:       return "JumpLand";
        case SurfaceInteraction::BodyFall:       return "BodyFall";
        case SurfaceInteraction::WeaponImpact:   return "WeaponImpact";
        case SurfaceInteraction::BulletImpact:   return "BulletImpact";
        case SurfaceInteraction::FrictionScrape: return "FrictionScrape";
        default:                                 return "FootstepWalk";
    }
}

SurfaceInteraction SurfaceAudioMatrix::string_to_interaction(const std::string& name) {
    if (name == "FootstepRun")    return SurfaceInteraction::FootstepRun;
    if (name == "FootstepSprint") return SurfaceInteraction::FootstepSprint;
    if (name == "JumpLand")       return SurfaceInteraction::JumpLand;
    if (name == "BodyFall")       return SurfaceInteraction::BodyFall;
    if (name == "WeaponImpact")   return SurfaceInteraction::WeaponImpact;
    if (name == "BulletImpact")   return SurfaceInteraction::BulletImpact;
    if (name == "FrictionScrape") return SurfaceInteraction::FrictionScrape;
    return SurfaceInteraction::FootstepWalk;
}

} // namespace nf::audio
