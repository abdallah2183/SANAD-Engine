#pragma once

#include <NF/Audio/SoundCue.hpp>
#include <NF/Core/Types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace nf::audio {

/// Physical material surface category
enum class SurfaceType : u8 {
    Default = 0,
    Concrete,
    Wood,
    Dirt,
    Grass,
    Metal,
    Water,
    Glass,
    Sand,
    Snow,
    Count
};

/// Interaction type creating sound on a surface
enum class SurfaceInteraction : u8 {
    FootstepWalk = 0,
    FootstepRun,
    FootstepSprint,
    JumpLand,
    BodyFall,
    WeaponImpact,
    BulletImpact,
    FrictionScrape,
    Count
};

/// Sound playback configuration for a specific surface and interaction
struct SurfaceInteractionConfig {
    SoundCue cue;
    f32 min_velocity = 0.5f;             ///< Threshold below which sound is inaudible
    f32 max_velocity = 10.0f;            ///< Velocity where volume reaches ceiling
    f32 volume_multiplier = 1.0f;
    f32 pitch_velocity_scaling = 0.03f;  ///< High impact speeds slightly raise pitch
    bool trigger_particles = true;       ///< Whether visual dust/sparks should accompany sound
};

/// Matrix mapping surface materials and physics collisions to dynamic sound cues
class SurfaceAudioMatrix {
public:
    SurfaceAudioMatrix();

    /// Register or overwrite interaction configuration for a single surface
    void set_interaction_cue(
        SurfaceType surface,
        SurfaceInteraction interaction,
        const SurfaceInteractionConfig& config
    );

    /// Check if interaction has authored cue (returns default if not)
    [[nodiscard]] const SurfaceInteractionConfig& get_interaction_config(
        SurfaceType surface,
        SurfaceInteraction interaction
    ) const;

    /// Register two-body collision impact cue (e.g. Metal impacting Stone)
    void set_collision_cue(
        SurfaceType surface_a,
        SurfaceType surface_b,
        const SurfaceInteractionConfig& config
    );

    /// Evaluate footstep sound based on surface, character locomotion speed, and optional wetness
    [[nodiscard]] SoundCueResult evaluate_footstep(
        SurfaceType surface,
        f32 character_speed,
        f32 rand_val = 0.5f,
        f32 surface_wetness = 0.0f
    );

    /// Evaluate physical collision impact between two surfaces based on impact velocity
    [[nodiscard]] SoundCueResult evaluate_impact(
        SurfaceType surface_a,
        SurfaceType surface_b,
        f32 impact_velocity,
        f32 rand_val = 0.5f
    );

    /// Convert between string names and SurfaceType enum
    [[nodiscard]] static std::string surface_to_string(SurfaceType surface);
    [[nodiscard]] static SurfaceType string_to_surface(const std::string& name);

    /// Convert between string names and SurfaceInteraction enum
    [[nodiscard]] static std::string interaction_to_string(SurfaceInteraction interaction);
    [[nodiscard]] static SurfaceInteraction string_to_interaction(const std::string& name);

private:
    [[nodiscard]] static u32 make_interaction_key(SurfaceType s, SurfaceInteraction i) noexcept {
        return (static_cast<u32>(s) << 8) | static_cast<u32>(i);
    }

    [[nodiscard]] static u32 make_collision_key(SurfaceType a, SurfaceType b) noexcept {
        u32 min_s = std::min(static_cast<u32>(a), static_cast<u32>(b));
        u32 max_s = std::max(static_cast<u32>(a), static_cast<u32>(b));
        return (min_s << 8) | max_s;
    }

    std::unordered_map<u32, SurfaceInteractionConfig> m_interactions;
    std::unordered_map<u32, SurfaceInteractionConfig> m_collisions;
    SurfaceInteractionConfig m_default_config;
};

} // namespace nf::audio
