#include <NF/Physics/WindField.hpp>
#include <cmath>

namespace nf::physics {

Vec3 WindZone::sample(const Vec3& sample_pos, float time) const {
    if (!enabled || strength <= 1e-4f) {
        return Vec3{0, 0, 0};
    }

    // Temporal gust pulse envelope
    float gust = 1.0f;
    if (pulse_frequency > 1e-4f && pulse_magnitude > 1e-4f) {
        gust += std::sin(time * pulse_frequency * 6.2831853f) * pulse_magnitude;
        gust = std::max(0.0f, gust);
    }

    const float effective_strength = strength * gust;

    switch (type) {
        case WindZoneType::Directional: {
            // Directional zone applies globally or across its influence cylinder
            if (radius > 1e-3f) {
                const Vec3 delta = sample_pos - position;
                const float dist = delta.length();
                if (dist >= radius) return Vec3{0, 0, 0};
                const float atten = std::pow(std::clamp(1.0f - (dist / radius), 0.0f, 1.0f), falloff_exponent);
                return direction.normalized() * (effective_strength * atten);
            }
            return direction.normalized() * effective_strength;
        }

        case WindZoneType::Spherical: {
            const Vec3 delta = sample_pos - position;
            const float dist = delta.length();
            if (dist >= radius || dist < 1e-4f) return Vec3{0, 0, 0};

            const float atten = std::pow(std::clamp(1.0f - (dist / radius), 0.0f, 1.0f), falloff_exponent);
            const Vec3 radial_dir = delta / dist;
            return radial_dir * (effective_strength * atten);
        }

        case WindZoneType::Vortex: {
            // Rotates tangentially around the zone's direction axis
            const Vec3 axis = direction.normalized();
            const Vec3 delta = sample_pos - position;
            // Project delta onto axis
            const float proj = delta.dot(axis);
            const Vec3 on_axis = axis * proj;
            const Vec3 radial = delta - on_axis;
            const float r_dist = radial.length();

            if (r_dist >= radius || r_dist < 1e-4f) return Vec3{0, 0, 0};

            const float atten = std::pow(std::clamp(1.0f - (r_dist / radius), 0.0f, 1.0f), falloff_exponent);
            // Tangential velocity is axis x radial
            const Vec3 tangent = axis.cross(radial).normalized();
            // Subtle upward updraft along axis
            const Vec3 updraft = axis * 0.2f;
            return (tangent + updraft).normalized() * (effective_strength * atten);
        }
    }

    return Vec3{0, 0, 0};
}

size_t WindFieldManager::add_zone(const WindZone& zone) {
    m_zones.push_back(zone);
    return m_zones.size() - 1;
}

void WindFieldManager::remove_zone(size_t index) {
    if (index < m_zones.size()) {
        m_zones.erase(m_zones.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

const WindZone* WindFieldManager::get_zone(size_t index) const {
    return index < m_zones.size() ? &m_zones[index] : nullptr;
}

WindZone* WindFieldManager::get_zone(size_t index) {
    return index < m_zones.size() ? &m_zones[index] : nullptr;
}

void WindFieldManager::set_global_wind(const Vec3& direction, float speed) {
    m_global_direction = direction.normalized();
    m_global_speed = std::max(0.0f, speed);
}

void WindFieldManager::set_turbulence(float strength, float frequency) {
    m_turbulence_strength = std::max(0.0f, strength);
    m_turbulence_frequency = std::max(0.01f, frequency);
}

Vec3 WindFieldManager::sample_wind(const Vec3& world_pos, float time) const {
    // 1. Ambient global wind
    Vec3 total_wind = m_global_direction * m_global_speed;

    // 2. Multi-octave 3D spatial turbulence
    if (m_turbulence_strength > 1e-4f) {
        const float freq = m_turbulence_frequency;
        const float phase = time * 2.0f;

        const float turb_x = std::sin(world_pos.y * freq + phase) * 0.6f +
                             std::cos(world_pos.z * freq * 2.1f + phase * 1.3f) * 0.4f;
        const float turb_y = std::sin(world_pos.z * freq + phase * 0.9f) * 0.5f +
                             std::cos(world_pos.x * freq * 1.7f + phase * 1.1f) * 0.5f;
        const float turb_z = std::cos(world_pos.x * freq + phase * 1.2f) * 0.6f +
                             std::sin(world_pos.y * freq * 2.3f + phase * 0.8f) * 0.4f;

        total_wind += Vec3{turb_x, turb_y, turb_z} * m_turbulence_strength;
    }

    // 3. Local zones contribution
    for (const auto& zone : m_zones) {
        total_wind += zone.sample(world_pos, time);
    }

    return total_wind;
}

float WindFieldManager::sample_speed(const Vec3& world_pos, float time) const {
    return sample_wind(world_pos, time).length();
}

void WindFieldManager::clear() {
    m_zones.clear();
}

} // namespace nf::physics
