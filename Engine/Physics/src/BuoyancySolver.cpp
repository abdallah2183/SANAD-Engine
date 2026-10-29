#include <NF/Physics/BuoyancySolver.hpp>
#include <cmath>
#include <algorithm>

namespace nf::physics {

constexpr f32 PI = 3.14159265358979323846f;

BuoyancyResult BuoyancySolver::solve_sphere(
    const WaterSurface& water,
    const Vec3& sphere_pos,
    f32 radius,
    const Vec3& linear_vel,
    const Vec3& angular_vel,
    f32 gravity
) {
    BuoyancyResult result;
    if (radius <= 0.0f) {
        return result;
    }

    const f32 total_volume = (4.0f / 3.0f) * PI * (radius * radius * radius);
    const f32 bottom_y = sphere_pos.y - radius;
    const f32 top_y = sphere_pos.y + radius;

    if (bottom_y >= water.height) {
        // Completely above water
        return result;
    }

    result.is_submerged = true;

    f32 submerged_vol = 0.0f;
    Vec3 cob = sphere_pos;

    if (top_y <= water.height) {
        // Fully submerged
        result.submerged_fraction = 1.0f;
        submerged_vol = total_volume;
        cob = sphere_pos;
    } else {
        // Partially submerged: depth of cap
        const f32 d = water.height - bottom_y;
        submerged_vol = (PI * d * d / 3.0f) * (3.0f * radius - d);
        result.submerged_fraction = std::clamp(submerged_vol / total_volume, 0.0f, 1.0f);
        const f32 cap_centroid_offset = (d * (4.0f * radius - d)) / (4.0f * (3.0f * radius - d));
        cob.y = bottom_y + cap_centroid_offset;
    }

    result.center_of_buoyancy = cob;

    // Upward Archimedes force: F = rho * V * g
    const f32 force_mag = water.fluid_density * submerged_vol * gravity;
    result.buoyant_force = Vec3(0.0f, force_mag, 0.0f);

    // Fluid drag opposes relative motion
    const Vec3 rel_vel = linear_vel - water.flow_velocity;
    result.fluid_drag_force = -rel_vel * (water.linear_drag * result.submerged_fraction * water.fluid_density * 0.01f);
    result.fluid_drag_torque = -angular_vel * (water.angular_drag * result.submerged_fraction * water.fluid_density * 0.01f);

    return result;
}

BuoyancyResult BuoyancySolver::solve_box(
    const WaterSurface& water,
    const Vec3& box_pos,
    const Vec3& half_extents,
    const Quat& orientation,
    const Vec3& linear_vel,
    const Vec3& angular_vel,
    f32 gravity
) {
    BuoyancyResult result;
    if (half_extents.x <= 0.0f || half_extents.y <= 0.0f || half_extents.z <= 0.0f) {
        return result;
    }

    const f32 total_volume = 8.0f * half_extents.x * half_extents.y * half_extents.z;

    // 8 local corners
    const Vec3 corners[8] = {
        Vec3(-half_extents.x, -half_extents.y, -half_extents.z),
        Vec3( half_extents.x, -half_extents.y, -half_extents.z),
        Vec3(-half_extents.x,  half_extents.y, -half_extents.z),
        Vec3( half_extents.x,  half_extents.y, -half_extents.z),
        Vec3(-half_extents.x, -half_extents.y,  half_extents.z),
        Vec3( half_extents.x, -half_extents.y,  half_extents.z),
        Vec3(-half_extents.x,  half_extents.y,  half_extents.z),
        Vec3( half_extents.x,  half_extents.y,  half_extents.z)
    };

    u32 submerged_count = 0;
    Vec3 submerged_sum{0.0f, 0.0f, 0.0f};

    for (int i = 0; i < 8; ++i) {
        const Vec3 world_pt = box_pos + orientation.rotate(corners[i]);
        if (world_pt.y < water.height) {
            submerged_count++;
            submerged_sum += world_pt;
        }
    }

    if (submerged_count == 0) {
        return result;
    }

    result.is_submerged = true;
    result.submerged_fraction = static_cast<f32>(submerged_count) / 8.0f;
    result.center_of_buoyancy = submerged_sum * (1.0f / static_cast<f32>(submerged_count));

    const f32 submerged_vol = total_volume * result.submerged_fraction;
    const f32 force_mag = water.fluid_density * submerged_vol * gravity;
    result.buoyant_force = Vec3(0.0f, force_mag, 0.0f);

    const Vec3 rel_vel = linear_vel - water.flow_velocity;
    result.fluid_drag_force = -rel_vel * (water.linear_drag * result.submerged_fraction * water.fluid_density * 0.01f);
    result.fluid_drag_torque = -angular_vel * (water.angular_drag * result.submerged_fraction * water.fluid_density * 0.01f);

    return result;
}

} // namespace nf::physics
