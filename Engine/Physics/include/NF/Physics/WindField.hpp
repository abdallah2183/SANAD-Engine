#pragma once

#include <NF/Core/Math.hpp>
#include <vector>
#include <algorithm>

namespace nf::physics {

enum class WindZoneType : uint8_t {
    Directional,  // Constant ambient or localized linear wind stream
    Spherical,    // Radial blast or suction pulse from a center point
    Vortex        // Rotational cylindrical vortex (tornado / rotor downwash)
};

/// A local or global source of atmospheric air velocity.
struct WindZone {
    WindZoneType type = WindZoneType::Directional;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{1.0f, 0.0f, 0.0f}; // Main wind or vortex axis
    float strength = 5.0f;            // Peak speed in meters per second
    float radius = 15.0f;             // Radius of influence (for Spherical / Vortex)
    float falloff_exponent = 1.0f;    // Distance attenuation sharpness
    float pulse_frequency = 0.5f;     // Periodic gust oscillation (Hz)
    float pulse_magnitude = 0.3f;     // Amplitude of gust variation [0, 1]
    bool enabled = true;

    /// Evaluates the wind velocity vector at `sample_pos` at a given timeline `time`.
    [[nodiscard]] Vec3 sample(const Vec3& sample_pos, float time) const;
};

/// Spatial wind manager aggregating global weather wind, local zones, and 3D fluid turbulence.
class WindFieldManager {
public:
    WindFieldManager() = default;

    size_t add_zone(const WindZone& zone);
    void remove_zone(size_t index);
    [[nodiscard]] size_t zone_count() const { return m_zones.size(); }
    [[nodiscard]] const WindZone* get_zone(size_t index) const;
    WindZone* get_zone(size_t index);

    void set_global_wind(const Vec3& direction, float speed);
    [[nodiscard]] const Vec3& global_wind_direction() const { return m_global_direction; }
    [[nodiscard]] float global_wind_speed() const { return m_global_speed; }

    void set_turbulence(float strength, float frequency);
    [[nodiscard]] float turbulence_strength() const { return m_turbulence_strength; }
    [[nodiscard]] float turbulence_frequency() const { return m_turbulence_frequency; }

    /// Samples total combined wind velocity vector (in m/s) at the specified world coordinate.
    [[nodiscard]] Vec3 sample_wind(const Vec3& world_pos, float time) const;

    /// Samples scalar wind speed magnitude at the specified world coordinate.
    [[nodiscard]] float sample_speed(const Vec3& world_pos, float time) const;

    void clear();

private:
    Vec3 m_global_direction{1.0f, 0.0f, 0.0f};
    float m_global_speed = 3.0f;
    float m_turbulence_strength = 0.5f;
    float m_turbulence_frequency = 0.8f;
    std::vector<WindZone> m_zones;
};

} // namespace nf::physics
