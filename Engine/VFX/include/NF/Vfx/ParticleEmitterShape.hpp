#pragma once

#include <NF/Core/Math.hpp>
#include <cstdint>

namespace nf::vfx {

/// Geometry primitive for particle emission volume.
enum class EmitterShapeType : uint8_t {
    Point,
    Sphere,
    Box,
    Cone,
    Ring,
    Cylinder
};

struct EmitterShapeConfig {
    EmitterShapeType type = EmitterShapeType::Point;
    Vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 1.0f;
    float inner_radius = 0.0f;          // For Ring or hollow Sphere
    Vec3 box_extents{1.0f, 1.0f, 1.0f}; // For Box (half extents)
    float height = 1.0f;                // For Cone or Cylinder
    float cone_angle_deg = 30.0f;       // Angle of cone opening
    bool surface_only = false;          // True to spawn strictly on surface/perimeter
    bool direction_outward = true;      // Initial direction points outward from center/normal
};

struct SpawnPoint {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 1.0f, 0.0f};
};

/// Deterministically generates a particle spawn point and outward direction given 3 pseudo-random values in [0, 1).
SpawnPoint generate_spawn_point(const EmitterShapeConfig& config, float r1, float r2, float r3);

} // namespace nf::vfx
