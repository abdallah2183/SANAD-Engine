#pragma once

// NF/Rendering/TimeOfDay.hpp — day/night cycle driver (design doc Section 64).
//
// A single clock (0-24h) derives the sun travel-direction, its color and
// intensity, and a matching SkyParams palette. Games and the editor feed the
// results straight into Renderer3D (set_directional_light + set_sky); the
// math is pure and pinned by tests, no device needed.
//
// Model (documented, not accidental):
//   - The sun rises at 06:00 (+X, east), culminates at 12:00 (+Y), sets at
//     18:00 (-X, west), with a slight +Z tilt so shadows always have depth.
//   - Elevation e = sin(pi * (t - 6) / 12): +1 at noon, -1 at midnight.
//   - Day light warms toward the horizon (orange) and cools to white at
//     noon; night is dim steel-blue moonlight from a fixed high angle.
//   - Sky palettes blend day -> sunset -> night by elevation bands.

#include <NF/Core/Math.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/Sky.hpp>

namespace nf::rendering {

class TimeOfDay {
public:
    /// Advances the clock by dt_seconds of real time. day_length_seconds <= 0
    /// freezes time (advance is a no-op but the state stays queryable).
    void advance(float dt_seconds);

    void set_time_hours(float hours); // wraps into [0, 24)
    float time_hours() const { return m_time_hours; }
    void set_day_length_seconds(float seconds) { m_day_length = seconds; }
    float day_length_seconds() const { return m_day_length; }

    /// Sine of the sun elevation: +1 noon, 0 horizon, -1 midnight.
    float sun_elevation() const;
    bool is_day() const { return sun_elevation() > 0.0f; }

    /// Normalized travel direction of sunlight (points down-ish during day).
    Vec3 sun_direction() const;
    /// Direction TOWARD the sun (normalized), for skies and shaders.
    Vec3 sun_position_dir() const { return -sun_direction(); }

    DirectionalLight make_light() const;
    SkyParams make_sky() const;

private:
    float m_time_hours = 12.0f;
    float m_day_length = 240.0f; // seconds per full cycle; <= 0 freezes
};

} // namespace nf::rendering
