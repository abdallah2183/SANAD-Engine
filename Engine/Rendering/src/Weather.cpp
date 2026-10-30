#include <NF/Rendering/Weather.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {

namespace {

constexpr f32 k_pi = 3.14159265358979323846f;

Vec3 lerp_vec3(const Vec3& a, const Vec3& b, f32 t) {
    return Vec3{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

f32 lerp_f32(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

} // namespace

WeatherSystem::WeatherSystem() {
    m_current = get_default_preset(WeatherType::Clear);
    m_source = m_current;
    m_target = m_current;
    m_wind_vector = m_current.wind_direction.normalized() * m_current.wind_strength;
}

WeatherPreset WeatherSystem::get_default_preset(WeatherType type) {
    WeatherPreset p;
    p.type = type;

    switch (type) {
        case WeatherType::Clear:
            p.fog.base_density = 0.005f;
            p.fog.height_falloff = 0.05f;
            p.fog.start_distance = 5.0f;
            p.fog.color = Vec3{0.7f, 0.8f, 0.95f};
            p.fog.sun_inscattering = 0.4f;
            p.sun_intensity = 1.0f;
            p.sun_color = Vec3{1.0f, 0.95f, 0.85f};
            p.ambient_intensity = 0.25f;
            p.ambient_color = Vec3{0.35f, 0.45f, 0.6f};
            p.precipitation_rate = 0.0f;
            p.wind_strength = 2.0f;
            p.wetness_accumulation_rate = 0.0f;
            break;

        case WeatherType::Cloudy:
            p.fog.base_density = 0.012f;
            p.fog.height_falloff = 0.06f;
            p.fog.start_distance = 4.0f;
            p.fog.color = Vec3{0.65f, 0.7f, 0.8f};
            p.fog.sun_inscattering = 0.3f;
            p.sun_intensity = 0.75f;
            p.sun_color = Vec3{0.9f, 0.9f, 0.9f};
            p.ambient_intensity = 0.35f;
            p.ambient_color = Vec3{0.4f, 0.45f, 0.55f};
            p.precipitation_rate = 0.0f;
            p.wind_strength = 3.5f;
            p.wetness_accumulation_rate = 0.0f;
            break;

        case WeatherType::Overcast:
            p.fog.base_density = 0.022f;
            p.fog.height_falloff = 0.07f;
            p.fog.start_distance = 3.0f;
            p.fog.color = Vec3{0.55f, 0.6f, 0.68f};
            p.fog.sun_inscattering = 0.2f;
            p.sun_intensity = 0.4f;
            p.sun_color = Vec3{0.75f, 0.75f, 0.8f};
            p.ambient_intensity = 0.4f;
            p.ambient_color = Vec3{0.45f, 0.48f, 0.55f};
            p.precipitation_rate = 0.05f;
            p.wind_strength = 4.0f;
            p.wetness_accumulation_rate = 0.01f;
            break;

        case WeatherType::Rain:
            p.fog.base_density = 0.035f;
            p.fog.height_falloff = 0.08f;
            p.fog.start_distance = 2.0f;
            p.fog.color = Vec3{0.45f, 0.5f, 0.6f};
            p.fog.sun_inscattering = 0.15f;
            p.sun_intensity = 0.25f;
            p.sun_color = Vec3{0.6f, 0.65f, 0.7f};
            p.ambient_intensity = 0.3f;
            p.ambient_color = Vec3{0.35f, 0.4f, 0.48f};
            p.precipitation_rate = 0.75f;
            p.wind_strength = 6.0f;
            p.wetness_accumulation_rate = 0.15f;
            break;

        case WeatherType::Thunderstorm:
            p.fog.base_density = 0.05f;
            p.fog.height_falloff = 0.09f;
            p.fog.start_distance = 1.0f;
            p.fog.color = Vec3{0.3f, 0.35f, 0.45f};
            p.fog.sun_inscattering = 0.1f;
            p.sun_intensity = 0.1f;
            p.sun_color = Vec3{0.5f, 0.55f, 0.65f};
            p.ambient_intensity = 0.2f;
            p.ambient_color = Vec3{0.25f, 0.28f, 0.35f};
            p.precipitation_rate = 1.0f;
            p.wind_strength = 14.0f;
            p.wetness_accumulation_rate = 0.3f;
            break;

        case WeatherType::Foggy:
            p.fog.base_density = 0.08f;
            p.fog.height_falloff = 0.03f;
            p.fog.start_distance = 0.5f;
            p.fog.color = Vec3{0.7f, 0.72f, 0.75f};
            p.fog.sun_inscattering = 0.5f;
            p.sun_intensity = 0.2f;
            p.sun_color = Vec3{0.8f, 0.8f, 0.8f};
            p.ambient_intensity = 0.45f;
            p.ambient_color = Vec3{0.5f, 0.52f, 0.55f};
            p.precipitation_rate = 0.0f;
            p.wind_strength = 1.0f;
            p.wetness_accumulation_rate = 0.02f;
            break;

        case WeatherType::Snow:
            p.fog.base_density = 0.025f;
            p.fog.height_falloff = 0.05f;
            p.fog.start_distance = 2.0f;
            p.fog.color = Vec3{0.75f, 0.8f, 0.88f};
            p.fog.sun_inscattering = 0.3f;
            p.sun_intensity = 0.5f;
            p.sun_color = Vec3{0.85f, 0.9f, 0.95f};
            p.ambient_intensity = 0.45f;
            p.ambient_color = Vec3{0.5f, 0.55f, 0.65f};
            p.precipitation_rate = 0.65f;
            p.wind_strength = 4.5f;
            p.wetness_accumulation_rate = 0.05f;
            break;
    }

    return p;
}

void WeatherSystem::set_weather(WeatherType type, f32 transition_duration) {
    set_custom_weather(get_default_preset(type), transition_duration);
}

void WeatherSystem::set_custom_weather(const WeatherPreset& preset, f32 transition_duration) {
    if (transition_duration <= 0.0f) {
        m_source = preset;
        m_target = preset;
        m_current = preset;
        m_transitioning = false;
        m_transition_progress = 1.0f;
    } else {
        m_source = m_current;
        m_target = preset;
        m_transitioning = true;
        m_transition_time = 0.0f;
        m_transition_duration = transition_duration;
        m_transition_progress = 0.0f;
    }
}

void WeatherSystem::blend_presets(f32 alpha) {
    m_current.type = (alpha >= 0.5f) ? m_target.type : m_source.type;

    // Blend fog properties
    m_current.fog.base_density = lerp_f32(m_source.fog.base_density, m_target.fog.base_density, alpha);
    m_current.fog.base_height = lerp_f32(m_source.fog.base_height, m_target.fog.base_height, alpha);
    m_current.fog.height_falloff = lerp_f32(m_source.fog.height_falloff, m_target.fog.height_falloff, alpha);
    m_current.fog.start_distance = lerp_f32(m_source.fog.start_distance, m_target.fog.start_distance, alpha);
    m_current.fog.max_opacity = lerp_f32(m_source.fog.max_opacity, m_target.fog.max_opacity, alpha);
    m_current.fog.color = lerp_vec3(m_source.fog.color, m_target.fog.color, alpha);
    m_current.fog.sun_inscattering = lerp_f32(m_source.fog.sun_inscattering, m_target.fog.sun_inscattering, alpha);

    // Blend lighting and atmosphere
    m_current.sun_intensity = lerp_f32(m_source.sun_intensity, m_target.sun_intensity, alpha);
    m_current.sun_color = lerp_vec3(m_source.sun_color, m_target.sun_color, alpha);
    m_current.ambient_intensity = lerp_f32(m_source.ambient_intensity, m_target.ambient_intensity, alpha);
    m_current.ambient_color = lerp_vec3(m_source.ambient_color, m_target.ambient_color, alpha);

    m_current.precipitation_rate = lerp_f32(m_source.precipitation_rate, m_target.precipitation_rate, alpha);
    m_current.wind_strength = lerp_f32(m_source.wind_strength, m_target.wind_strength, alpha);
    m_current.wind_direction = lerp_vec3(m_source.wind_direction, m_target.wind_direction, alpha).normalized();
    m_current.wetness_accumulation_rate = lerp_f32(m_source.wetness_accumulation_rate, m_target.wetness_accumulation_rate, alpha);
}

void WeatherSystem::update(f32 dt) {
    if (dt <= 0.0f) {
        return;
    }

    if (m_transitioning) {
        m_transition_time += dt;
        f32 alpha = std::clamp(m_transition_time / m_transition_duration, 0.0f, 1.0f);
        // Smoothstep curve for natural weather transition
        f32 smooth_alpha = alpha * alpha * (3.0f - 2.0f * alpha);
        m_transition_progress = alpha;
        blend_presets(smooth_alpha);

        if (alpha >= 1.0f) {
            m_transitioning = false;
            m_current = m_target;
        }
    }

    // Ground wetness accumulation or drying
    if (m_current.wetness_accumulation_rate > 0.0f) {
        m_ground_wetness = std::min(1.0f, m_ground_wetness + m_current.wetness_accumulation_rate * dt);
    } else {
        m_ground_wetness = std::max(0.0f, m_ground_wetness - m_drying_speed * dt);
    }

    // Dynamic wind gust oscillation
    m_time_accumulator += dt;
    f32 gust = 1.0f + 0.25f * std::sin(m_time_accumulator * 1.5f) + 0.15f * std::sin(m_time_accumulator * 4.0f);
    m_wind_vector = m_current.wind_direction.normalized() * (m_current.wind_strength * gust);
}

f32 WeatherSystem::compute_fog_factor(const Vec3& ray_start, const Vec3& ray_end) const {
    Vec3 diff = ray_end - ray_start;
    f32 dist = diff.length();
    if (dist <= m_current.fog.start_distance) {
        return 0.0f;
    }

    f32 s = dist - m_current.fog.start_distance;
    Vec3 dir = diff * (1.0f / dist);
    Vec3 p0 = ray_start + dir * m_current.fog.start_distance;

    f32 delta_y = ray_end.y - p0.y;
    f32 rel_y = p0.y - m_current.fog.base_height;

    f32 lambda = std::max(1e-4f, m_current.fog.height_falloff);
    f32 avg_density = 0.0f;

    if (std::abs(delta_y) < 1e-3f) {
        avg_density = m_current.fog.base_density * std::exp(-lambda * rel_y);
    } else {
        f32 integral = (1.0f - std::exp(-lambda * delta_y)) / (lambda * delta_y);
        avg_density = m_current.fog.base_density * std::exp(-lambda * rel_y) * integral;
    }

    f32 optical_depth = avg_density * s;
    f32 transmittance = std::exp(-optical_depth);
    return std::clamp(1.0f - transmittance, 0.0f, m_current.fog.max_opacity);
}

Vec3 WeatherSystem::compute_fog_color(const Vec3& ray_dir, const Vec3& sun_dir) const {
    f32 cos_theta = std::clamp(ray_dir.normalized().dot(sun_dir.normalized()), -1.0f, 1.0f);

    // Forward scattering Henyey-Greenstein approximation (g = 0.7)
    constexpr f32 g = 0.7f;
    constexpr f32 g2 = g * g;
    f32 denom = 1.0f + g2 - 2.0f * g * cos_theta;
    f32 phase = (1.0f - g2) / (std::pow(denom, 1.5f) * 4.0f * k_pi);

    // Normalize phase to [0, 1] range for visual glow
    f32 inscatter_weight = std::clamp(phase * (4.0f * k_pi * 0.25f), 0.0f, 1.0f) * m_current.fog.sun_inscattering;

    return lerp_vec3(m_current.fog.color, m_current.sun_color, inscatter_weight);
}

} // namespace nf::rendering
