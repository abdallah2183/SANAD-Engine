#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace nf::rendering {

/// Discrete atmospheric weather types
enum class WeatherType : u8 {
    Clear = 0,
    Cloudy,
    Overcast,
    Rain,
    Thunderstorm,
    Foggy,
    Snow
};

/// Exponential height fog parameters
struct FogParams {
    f32 base_density = 0.015f;       ///< Fog density at reference altitude
    f32 base_height = 0.0f;          ///< Base altitude in world units
    f32 height_falloff = 0.08f;      ///< Exponential decay rate with altitude
    f32 start_distance = 2.0f;       ///< Distance from camera before fog begins
    f32 max_opacity = 1.0f;          ///< Upper ceiling for fog opacity [0, 1]
    Vec3 color{0.65f, 0.75f, 0.85f}; ///< Ambient fog color (RGB)
    f32 sun_inscattering = 0.3f;     ///< Intensity of forward scattering from sun
};

/// Environment configuration for a specific weather state
struct WeatherPreset {
    WeatherType type = WeatherType::Clear;
    FogParams fog;
    f32 sun_intensity = 1.0f;
    Vec3 sun_color{1.0f, 0.95f, 0.85f};
    f32 ambient_intensity = 0.25f;
    Vec3 ambient_color{0.4f, 0.5f, 0.65f};
    f32 precipitation_rate = 0.0f;   ///< [0, 1] rain/snow particle intensity
    f32 wind_strength = 2.0f;        ///< Wind speed in m/s
    Vec3 wind_direction{1.0f, 0.0f, 0.0f};
    f32 wetness_accumulation_rate = 0.0f; ///< Rate at which puddles/surfaces get wet
};

/// Weather and atmospheric fog manager with smooth transitions
class WeatherSystem {
public:
    WeatherSystem();

    /// Get factory preset for a standard weather type
    [[nodiscard]] static WeatherPreset get_default_preset(WeatherType type);

    /// Set active weather, either instantly (transition_duration <= 0) or blended over time
    void set_weather(WeatherType type, f32 transition_duration = 0.0f);

    /// Set custom weather preset directly
    void set_custom_weather(const WeatherPreset& preset, f32 transition_duration = 0.0f);

    /// Advance time, transition blending, ground wetness, and wind gusts
    void update(f32 dt);

    /// Compute exponential height fog factor [0 = clear, 1 = fully obscured] along a ray
    [[nodiscard]] f32 compute_fog_factor(const Vec3& ray_start, const Vec3& ray_end) const;

    /// Calculate combined fog color blending ambient fog with sun in-scattering
    [[nodiscard]] Vec3 compute_fog_color(const Vec3& ray_dir, const Vec3& sun_dir) const;

    /// Current interpolated weather properties
    [[nodiscard]] const WeatherPreset& current_preset() const noexcept { return m_current; }
    [[nodiscard]] WeatherType current_weather_type() const noexcept { return m_current.type; }
    [[nodiscard]] WeatherType target_weather_type() const noexcept { return m_target.type; }
    [[nodiscard]] bool is_transitioning() const noexcept { return m_transitioning; }
    [[nodiscard]] f32 transition_progress() const noexcept { return m_transition_progress; }

    /// Ground wetness level [0 = bone dry, 1 = flooded puddles]
    [[nodiscard]] f32 ground_wetness() const noexcept { return m_ground_wetness; }
    void set_ground_wetness(f32 wetness) noexcept { m_ground_wetness = std::clamp(wetness, 0.0f, 1.0f); }

    /// Drying rate of ground wetness in units per second when not raining
    void set_drying_speed(f32 speed) noexcept { m_drying_speed = std::max(0.0f, speed); }

    /// Current instantaneous wind vector including procedural gusts
    [[nodiscard]] Vec3 current_wind_vector() const noexcept { return m_wind_vector; }

private:
    void blend_presets(f32 alpha);

    WeatherPreset m_source;
    WeatherPreset m_target;
    WeatherPreset m_current;

    bool m_transitioning = false;
    f32 m_transition_time = 0.0f;
    f32 m_transition_duration = 0.0f;
    f32 m_transition_progress = 1.0f;

    f32 m_ground_wetness = 0.0f;
    f32 m_drying_speed = 0.05f;

    f32 m_time_accumulator = 0.0f;
    Vec3 m_wind_vector{1.0f, 0.0f, 0.0f};
};

} // namespace nf::rendering
