#pragma once

// NF/Rendering/CameraShake.hpp — trauma-based procedural camera shake.
//
// Trauma (0..1) rises with impacts/explosions and decays over time; the
// visible offset scales with trauma^2 (small hits barely register, big hits
// dominate — the standard game-feel curve). The noise is a sum of
// incommensurate sines: deterministic, smooth, no RNG state, same input
// time always yields the same offset (design doc Section 114).
//
// Usage: shake.add_trauma(0.5) on an explosion; every frame shake.update(dt)
// then camera.position += shake.position_offset(t) and rotate by
// shake.rotation_offset_deg(t) before update_camera().

#include <NF/Core/Math.hpp>

namespace nf::rendering {

class CameraShake {
public:
    /// Adds trauma, clamped to [0, 1].
    void add_trauma(float amount);
    /// Decays trauma (default 1.4/s) and advances the clock.
    void update(float dt_seconds);
    float trauma() const { return m_trauma; }

    void set_decay_per_second(float decay) { m_decay = decay < 0.0f ? 0.0f : decay; }
    void set_position_strength(float strength) { m_position_strength = strength; }
    void set_rotation_strength_deg(float strength) { m_rotation_strength = strength; }

    /// World-space positional offset at the current clock.
    Vec3 position_offset() const;
    /// Euler offset in degrees (pitch, yaw, roll) at the current clock.
    Vec3 rotation_offset_deg() const;

private:
    // Deterministic smooth noise in [-1, 1] per channel.
    float noise_x(float t) const;
    float noise_y(float t) const;
    float noise_z(float t) const;

    float m_trauma = 0.0f;
    float m_time = 0.0f;
    float m_decay = 1.4f;
    float m_position_strength = 0.35f;
    float m_rotation_strength = 6.0f; // degrees at full trauma
};

} // namespace nf::rendering
