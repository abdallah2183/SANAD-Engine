// NF/Rendering/CameraShake.cpp — trauma-based camera shake.

#include <NF/Rendering/CameraShake.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {

void CameraShake::add_trauma(float amount) {
    m_trauma = std::clamp(m_trauma + amount, 0.0f, 1.0f);
}

void CameraShake::update(float dt_seconds) {
    if (dt_seconds > 0.0f) {
        m_time += dt_seconds;
        m_trauma = std::max(0.0f, m_trauma - m_decay * dt_seconds);
    }
}

float CameraShake::noise_x(float t) const {
    return (std::sin(t * 39.7f) + std::sin(t * 27.3f + 1.7f) * 0.5f +
            std::sin(t * 61.1f + 4.2f) * 0.25f) /
           1.75f;
}

float CameraShake::noise_y(float t) const {
    return (std::sin(t * 33.1f + 0.6f) + std::sin(t * 47.9f + 2.9f) * 0.5f +
            std::sin(t * 23.7f + 5.1f) * 0.25f) /
           1.75f;
}

float CameraShake::noise_z(float t) const {
    return (std::sin(t * 29.3f + 3.3f) + std::sin(t * 43.7f + 0.9f) * 0.5f +
            std::sin(t * 59.3f + 2.2f) * 0.25f) /
           1.75f;
}

Vec3 CameraShake::position_offset() const {
    const float s = m_trauma * m_trauma * m_position_strength;
    return Vec3{noise_x(m_time) * s, noise_y(m_time) * s, noise_z(m_time) * s};
}

Vec3 CameraShake::rotation_offset_deg() const {
    const float s = m_trauma * m_trauma * m_rotation_strength;
    // Different phases per axis so rotation never mirrors translation.
    return Vec3{noise_y(m_time * 1.13f + 7.0f) * s, noise_z(m_time * 0.97f + 3.0f) * s,
                noise_x(m_time * 1.07f + 5.0f) * s * 0.5f};
}

} // namespace nf::rendering
