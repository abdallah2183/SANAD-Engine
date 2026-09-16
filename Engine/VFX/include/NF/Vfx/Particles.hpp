#pragma once

// NF/Vfx/Particles.hpp — CPU particle simulation (design doc Section 57).
//
// Emission, ballistic integration (gravity + drag), lifetime kill, and
// age-graded size/color. Deterministic: spread comes from a hash of the
// emission counter (no RNG state), so identical update sequences yield
// identical particles on every platform (design doc Section 114).
//
// Rendering is a later phase: this module hands the renderer a packed,
// read-only particle array (position/size/color/life fraction).

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <cstdint>
#include <vector>

namespace nf::vfx {

struct Particle {
    Vec3 position{0, 0, 0};
    Vec3 velocity{0, 0, 0};
    float life = 0.0f; // remaining seconds
    float max_life = 1.0f;
    float size = 0.2f;
    Vec3 color{1, 1, 1};

    float age_fraction() const { return max_life > 0.0f ? 1.0f - life / max_life : 1.0f; }
};

struct EmitterConfig {
    float rate = 30.0f; // particles per second (0 = burst-only)
    float lifetime = 1.5f;
    float lifetime_spread = 0.5f; // +/- fraction of lifetime
    Vec3 velocity{0.0f, 3.0f, 0.0f};
    Vec3 velocity_spread{1.0f, 1.0f, 1.0f}; // +/- per axis
    Vec3 gravity{0.0f, -9.8f, 0.0f};
    float drag = 0.0f; // velocity retention loss per second (0 = none)
    float start_size = 0.25f;
    float end_size = 0.0f;
    Vec3 start_color{1.0f, 0.9f, 0.7f};
    Vec3 end_color{1.0f, 0.3f, 0.1f};
    u32 max_particles = 1024;
    Vec3 origin{0, 0, 0};
};

class ParticleSystem {
public:
    explicit ParticleSystem(EmitterConfig config = EmitterConfig{});

    /// Immediate spawn (explosions), bypassing the rate accumulator.
    void burst(u32 count);
    /// Emits by rate, integrates, grades, kills. dt <= 0 is a no-op.
    void update(float dt);
    void clear();

    usize alive() const { return m_particles.size(); }
    const std::vector<Particle>& particles() const { return m_particles; }
    const EmitterConfig& config() const { return m_config; }
    void set_config(EmitterConfig config) { m_config = config; }

private:
    // Deterministic [0,1) stream from the emission counter (integer hash).
    float rand01();

    EmitterConfig m_config;
    std::vector<Particle> m_particles;
    float m_emit_accumulator = 0.0f;
    u64 m_emit_counter = 0;
};

} // namespace nf::vfx
