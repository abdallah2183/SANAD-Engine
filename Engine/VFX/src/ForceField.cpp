#include <NF/VFX/ForceField.hpp>
#include <cmath>
#include <algorithm>

namespace nf::vfx {

Vec3 ForceField::evaluate(const Vec3& particle_pos, const Vec3& particle_vel) const {
    float falloff = 1.0f;
    if (radius > 0.0001f) {
        float dist = (particle_pos - position).length();
        if (dist >= radius) {
            return Vec3(0.0f, 0.0f, 0.0f);
        }
        float factor = std::clamp(1.0f - (dist / radius), 0.0f, 1.0f);
        falloff = std::pow(factor, std::max(0.1f, falloff_exp));
    }

    switch (type) {
        case ForceType::DirectionalWind: {
            float len = direction.length();
            Vec3 dir = (len > 0.0001f) ? (direction / len) : Vec3(0.0f, 1.0f, 0.0f);
            return dir * (strength * falloff);
        }
        case ForceType::PointAttractor: {
            Vec3 delta = position - particle_pos;
            float dist = delta.length();
            if (dist < 0.0001f) {
                return Vec3(0.0f, 0.0f, 0.0f);
            }
            Vec3 dir = delta / dist;
            return dir * (strength * falloff);
        }
        case ForceType::PointRepulsor: {
            Vec3 delta = particle_pos - position;
            float dist = delta.length();
            Vec3 dir = (dist > 0.0001f) ? (delta / dist) : Vec3(0.0f, 1.0f, 0.0f);
            return dir * (strength * falloff);
        }
        case ForceType::Vortex: {
            float axis_len = direction.length();
            Vec3 axis = (axis_len > 0.0001f) ? (direction / axis_len) : Vec3(0.0f, 1.0f, 0.0f);
            Vec3 v = particle_pos - position;
            float proj = v.dot(axis);
            Vec3 radial = v - axis * proj;
            Vec3 tangent = axis.cross(radial);
            float tan_len = tangent.length();
            if (tan_len < 0.0001f) {
                return Vec3(0.0f, 0.0f, 0.0f);
            }
            Vec3 tan_dir = tangent / tan_len;
            return tan_dir * (strength * falloff);
        }
        case ForceType::Drag: {
            return particle_vel * (-strength * falloff);
        }
    }
    return Vec3(0.0f, 0.0f, 0.0f);
}

Vec3 ForceFieldManager::evaluate_total_force(const Vec3& particle_pos, const Vec3& particle_vel) const {
    Vec3 net_force(0.0f, 0.0f, 0.0f);
    for (const auto& field : m_fields) {
        net_force = net_force + field.evaluate(particle_pos, particle_vel);
    }
    return net_force;
}

} // namespace nf::vfx
