#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp>

namespace nf::physics {

struct WaterSurface {
    f32 height = 0.0f;                      // Water surface plane height along Y axis (meters)
    Vec3 flow_velocity{0.0f, 0.0f, 0.0f};  // Linear water flow current velocity (m/s)
    f32 fluid_density = 1000.0f;             // kg/m^3 (fresh water = 1000 kg/m^3, salt water = 1025 kg/m^3)
    f32 linear_drag = 2.0f;                 // Fluid drag coefficient
    f32 angular_drag = 2.5f;                // Angular fluid drag coefficient
};

struct BuoyancyResult {
    bool is_submerged = false;
    f32 submerged_fraction = 0.0f;          // 0.0 (completely above) to 1.0 (fully underwater)
    Vec3 center_of_buoyancy{0.0f, 0.0f, 0.0f};
    Vec3 buoyant_force{0.0f, 0.0f, 0.0f};      // Upward Archimedes force in Newtons
    Vec3 fluid_drag_force{0.0f, 0.0f, 0.0f};  // Viscous resistance opposing relative motion
    Vec3 fluid_drag_torque{0.0f, 0.0f, 0.0f}; // Viscous resistance opposing rotation
};

class BuoyancySolver {
public:
    /// Calculate buoyancy and fluid resistance for a sphere
    static BuoyancyResult solve_sphere(
        const WaterSurface& water,
        const Vec3& sphere_position,
        f32 radius,
        const Vec3& linear_velocity,
        const Vec3& angular_velocity,
        f32 gravity = 9.81f
    );

    /// Calculate buoyancy and fluid resistance for an oriented bounding box
    static BuoyancyResult solve_box(
        const WaterSurface& water,
        const Vec3& box_position,
        const Vec3& half_extents,
        const Quat& orientation,
        const Vec3& linear_velocity,
        const Vec3& angular_velocity,
        f32 gravity = 9.81f
    );
};

} // namespace nf::physics
