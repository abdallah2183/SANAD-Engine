#pragma once

#include <NF/Core/Math.hpp>
#include <NF/ECS/ECS.hpp>

namespace nf::scene {

// Transform component — stored in ECS World
//
// v0.1 model: translation accumulates through the hierarchy (world_xyz).
// Rotation (degrees, XYZ euler) and scale are local-only in v0.1: they are
// stored, edited, saved and composed into the render matrix, but they do not
// affect children's world_xyz accumulation (that lands with the full TRS
// hierarchy upgrade). Defaults are identity so existing scenes behave as before.
struct Transform {
    float local_x = 0, local_y = 0, local_z = 0;
    float world_x = 0, world_y = 0, world_z = 0;
    float rot_x = 0, rot_y = 0, rot_z = 0; // degrees, XYZ euler order
    float scale_x = 1, scale_y = 1, scale_z = 1;
    ecs::Entity parent = ecs::kInvalidEntity;
    bool dirty = true;
};

// Transform hierarchy helpers (operate on ecs::World)
void set_parent(ecs::World& world, ecs::Entity child, ecs::Entity parent);
void remove_parent(ecs::World& world, ecs::Entity child);
ecs::Entity get_parent(const ecs::World& world, ecs::Entity child);
std::vector<ecs::Entity> get_children(const ecs::World& world, ecs::Entity parent);

// Propagates local → world for all entities. Uses iterative BFS to avoid recursion.
void propagate_transforms(ecs::World& world);

// System that can be registered in the scheduler
void transform_system(ecs::World& world);

// Composes a column-major 4x4 matrix (out[col*4+row], translation at
// out[12..14]) from translation + XYZ euler rotation (degrees,
// R = Ry * Rx * Rz) + scale: M = T * R * S. Column-vector application.
// Prefer compose_trs_mat4 below (same transform as nf::Mat4) for all new
// code; this raw-array form stays as the byte-level reference the
// equivalence test pins (GPU uploads have consumed these exact bytes since
// v0.1, so any drift re-poses every rotated object in the scene).
void compose_trs(float px, float py, float pz,
                 float rx_deg, float ry_deg, float rz_deg,
                 float sx, float sy, float sz,
                 float out_m16[16]);

/// Same transform as compose_trs, as the engine's row-vector nf::Mat4:
/// memcpy of result.m is byte-identical to the legacy column-major array
/// (row-major-flat of the row-vector form == column-major-flat of the
/// column-vector form), so GPU uploads do not change. Translation in row 3.
/// Prefer this for all CPU-side work (extraction, bounds, tests).
Mat4 compose_trs_mat4(float px, float py, float pz,
                      float rx_deg, float ry_deg, float rz_deg,
                      float sx, float sy, float sz);

/// The inverse of compose_trs's rotation: the XYZ euler angles in degrees
/// (R = Ry * Rx * Rz) that reproduce a quaternion's rotation.
///
/// Used to write a physics body's orientation back into a Transform. It lives
/// here, next to compose_trs, because the two must agree on the convention — a
/// mismatch would make a rotating body drift or spin the wrong way, and the
/// round-trip test is what keeps them honest.
void euler_xyz_degrees_from_quat(const Quat& q, float& out_rx, float& out_ry, float& out_rz);

/// The forward direction: the quaternion equivalent of compose_trs's rotation
/// for the given XYZ euler degrees. Exists so a physics body can be created from
/// a Transform without either side guessing the convention.
Quat quat_from_euler_xyz_degrees(float rx_deg, float ry_deg, float rz_deg);

} // namespace nf::scene
