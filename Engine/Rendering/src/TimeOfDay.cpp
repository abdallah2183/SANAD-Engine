// NF/Rendering/TimeOfDay.cpp — day/night cycle driver.

#include <NF/Rendering/TimeOfDay.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float wrap24(float h) {
    float w = std::fmod(h, 24.0f);
    if (w < 0.0f) w += 24.0f;
    // fmod(24,24) == 0 already; guard the -0 edge for stable keys.
    return w == 0.0f ? 0.0f : w;
}

Vec3 mix_vec(const Vec3& a, const Vec3& b, float t) {
    const float c = std::clamp(t, 0.0f, 1.0f);
    return {a.x + (b.x - a.x) * c, a.y + (b.y - a.y) * c, a.z + (b.z - a.z) * c};
}

} // namespace

void TimeOfDay::advance(float dt_seconds) {
    if (!(m_day_length > 0.0f) || !(dt_seconds > 0.0f)) return;
    m_time_hours = wrap24(m_time_hours + dt_seconds * 24.0f / m_day_length);
}

void TimeOfDay::set_time_hours(float hours) {
    m_time_hours = (hours >= 0.0f && hours < 24.0f) ? hours : wrap24(hours);
}

float TimeOfDay::sun_elevation() const {
    return std::sin(kPi * (m_time_hours - 6.0f) / 12.0f);
}

Vec3 TimeOfDay::sun_direction() const {
    // theta: 0 at 06:00 (east +X), pi/2 at noon (+Y), pi at 18:00 (-X west).
    const float theta = kPi * (m_time_hours - 6.0f) / 12.0f;
    Vec3 to_sun{std::cos(theta), std::sin(theta), 0.35f};
    const float len = to_sun.length();
    if (len < 1e-6f) return Vec3{0.0f, -1.0f, 0.0f};
    to_sun = to_sun / len;
    if (!is_day()) {
        // Moonlight: fixed high steel-blue source so nights stay readable.
        return Vec3{0.3f, -0.8f, -0.45f}.normalized();
    }
    return -to_sun; // travel direction: sun -> scene
}

DirectionalLight TimeOfDay::make_light() const {
    DirectionalLight light;
    light.enabled = true;
    light.shadows_enabled = true;
    light.direction = sun_direction();
    const float e = sun_elevation();
    if (!is_day()) {
        light.color = Vec3{0.35f, 0.45f, 0.65f};
        light.intensity = 0.25f;
        return light;
    }
    // Warm at the horizon (e=0), white overhead (e=1).
    light.color = mix_vec(Vec3{1.0f, 0.45f, 0.18f}, Vec3{1.0f, 0.98f, 0.92f},
                          std::clamp(e * 1.6f, 0.0f, 1.0f));
    light.intensity = 0.4f + 2.8f * std::clamp(e, 0.0f, 1.0f);
    return light;
}

SkyParams TimeOfDay::make_sky() const {
    SkyParams day; // the renderer's long-standing default look
    const float e = sun_elevation();
    if (e >= 0.45f) return day; // full day: no blending cost in spirit

    const SkyParams sunset{
        Vec3{0.28f, 0.18f, 0.42f}, // zenith: dusk purple
        Vec3{0.98f, 0.45f, 0.22f}, // horizon: ember orange
        Vec3{0.10f, 0.08f, 0.12f}, // ground
        Vec3{0.02f, 0.02f, 0.03f}, // clear
        1.2f, 1.6f, true, // bigger glow at dusk
    };
    const SkyParams night{
        Vec3{0.015f, 0.03f, 0.08f}, // zenith: deep blue
        Vec3{0.05f, 0.08f, 0.15f}, // horizon: faint glow
        Vec3{0.01f, 0.01f, 0.02f}, // ground
        Vec3{0.005f, 0.008f, 0.02f}, // clear
        0.4f, 0.4f, true, // shy moon glow
    };
    auto blend = [](const SkyParams& a, const SkyParams& b, float t) {
        SkyParams out;
        out.zenith = mix_vec(a.zenith, b.zenith, t);
        out.horizon = mix_vec(a.horizon, b.horizon, t);
        out.ground = mix_vec(a.ground, b.ground, t);
        out.clear = mix_vec(a.clear, b.clear, t);
        out.sun_disk = a.sun_disk + (b.sun_disk - a.sun_disk) * t;
        out.sun_glow = a.sun_glow + (b.sun_glow - a.sun_glow) * t;
        out.enabled = true;
        return out;
    };
    if (e >= 0.0f) {
        // Day -> sunset across the golden band.
        return blend(day, sunset, 1.0f - e / 0.45f);
    }
    // Sunset -> night below the horizon.
    return blend(sunset, night, std::clamp(-e * 2.5f, 0.0f, 1.0f));
}

} // namespace nf::rendering
