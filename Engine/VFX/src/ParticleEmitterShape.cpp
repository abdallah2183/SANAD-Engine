#include <NF/VFX/ParticleEmitterShape.hpp>
#include <cmath>
#include <algorithm>

namespace nf::vfx {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
}

SpawnPoint generate_spawn_point(const EmitterShapeConfig& config, float r1, float r2, float r3) {
    SpawnPoint sp;
    r1 = std::clamp(r1, 0.0f, 0.99999f);
    r2 = std::clamp(r2, 0.0f, 0.99999f);
    r3 = std::clamp(r3, 0.0f, 0.99999f);

    switch (config.type) {
        case EmitterShapeType::Point: {
            sp.position = config.center;
            sp.direction = Vec3(0.0f, 1.0f, 0.0f);
            break;
        }
        case EmitterShapeType::Sphere: {
            float theta = kTwoPi * r1;
            float phi = std::acos(std::clamp(2.0f * r2 - 1.0f, -1.0f, 1.0f));
            float sin_phi = std::sin(phi);

            Vec3 normal(sin_phi * std::cos(theta), std::cos(phi), sin_phi * std::sin(theta));
            float dist = config.surface_only ? 1.0f : std::cbrt(r3);
            float actual_rad = config.inner_radius + dist * (config.radius - config.inner_radius);

            sp.position = config.center + normal * actual_rad;
            sp.direction = config.direction_outward ? normal : Vec3(0.0f, 1.0f, 0.0f);
            break;
        }
        case EmitterShapeType::Box: {
            Vec3 offset(
                (2.0f * r1 - 1.0f) * config.box_extents.x,
                (2.0f * r2 - 1.0f) * config.box_extents.y,
                (2.0f * r3 - 1.0f) * config.box_extents.z
            );
            sp.position = config.center + offset;
            float len = offset.length();
            sp.direction = (config.direction_outward && len > 0.0001f) ? (offset / len) : Vec3(0.0f, 1.0f, 0.0f);
            break;
        }
        case EmitterShapeType::Cone: {
            float angle_rad = config.cone_angle_deg * (kPi / 180.0f);
            float h = r1 * config.height;
            float cone_rad = std::tan(angle_rad) * h + config.radius;
            float theta = kTwoPi * r2;
            float d = (config.surface_only ? 1.0f : std::sqrt(r3)) * cone_rad;

            Vec3 local(d * std::cos(theta), h, d * std::sin(theta));
            sp.position = config.center + local;

            Vec3 dir(std::cos(theta) * std::sin(angle_rad), std::cos(angle_rad), std::sin(theta) * std::sin(angle_rad));
            sp.direction = config.direction_outward ? dir.normalized() : Vec3(0.0f, 1.0f, 0.0f);
            break;
        }
        case EmitterShapeType::Ring: {
            float theta = kTwoPi * r1;
            float rad = config.inner_radius + r2 * (config.radius - config.inner_radius);
            Vec3 normal(std::cos(theta), 0.0f, std::sin(theta));

            sp.position = config.center + normal * rad;
            sp.direction = config.direction_outward ? normal : Vec3(0.0f, 1.0f, 0.0f);
            break;
        }
        case EmitterShapeType::Cylinder: {
            float theta = kTwoPi * r1;
            float rad = (config.surface_only ? 1.0f : std::sqrt(r2)) * config.radius;
            float h = (r3 - 0.5f) * config.height;

            Vec3 normal(std::cos(theta), 0.0f, std::sin(theta));
            sp.position = config.center + normal * rad + Vec3(0.0f, h, 0.0f);
            sp.direction = config.direction_outward ? normal : Vec3(0.0f, 1.0f, 0.0f);
            break;
        }
    }

    return sp;
}

} // namespace nf::vfx
