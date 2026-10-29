#pragma once

#include <NF/Core/Math.hpp>
#include <vector>
#include <cstdint>

namespace nf::vfx {

/// Type of environmental force applied to simulated particles.
enum class ForceType : uint8_t {
    DirectionalWind,
    PointAttractor,
    PointRepulsor,
    Vortex,
    Drag
};

/// A single spatial force field affector.
struct ForceField {
    ForceType type = ForceType::DirectionalWind;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 1.0f, 0.0f}; // Wind direction or Vortex axis
    float strength = 5.0f;
    float radius = 0.0f;              // 0.0f = infinite unbounded range
    float falloff_exp = 1.0f;         // 1.0 = linear, 2.0 = inverse-square

    /// Evaluates force acceleration vector (m/s^2) exerted on a particle.
    Vec3 evaluate(const Vec3& particle_pos, const Vec3& particle_vel) const;
};

/// Aggregates and applies multiple environmental force fields to particle simulations.
class ForceFieldManager {
public:
    ForceFieldManager() = default;

    void add_field(const ForceField& field) { m_fields.push_back(field); }
    void clear() { m_fields.clear(); }
    size_t field_count() const { return m_fields.size(); }
    const std::vector<ForceField>& fields() const { return m_fields; }

    /// Evaluates net cumulative force acceleration vector for a particle.
    Vec3 evaluate_total_force(const Vec3& particle_pos, const Vec3& particle_vel) const;

private:
    std::vector<ForceField> m_fields;
};

} // namespace nf::vfx
