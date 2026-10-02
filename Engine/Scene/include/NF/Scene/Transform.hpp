#pragma once

#include <NF/Core/Math.hpp>
#include <NF/ECS/ECS.hpp>

namespace nf::scene {

// Transform component — stored in ECS World
//
// FULL TRS. local_* / rot_* / scale_* are what the user edits and what the
// Inspector shows. world_* / world_rot / world_scale_* are the composed result,
// written by propagate_transforms() and read by extraction, bounds, picking and
// the gizmo.
//
// This used to accumulate translation only: world_x = parent.world_x + local_x,
// with rotation and scale dropped on the floor. The consequences were not
// subtle and all of them were user-visible:
//   * a rotated object RENDERED UNROTATED (extract_render_objects built a
//     translation-only matrix, so the mesh kept its authored pose while its
//     transform said otherwise);
//   * a scaled object rendered at 1x1, so "make the ground bigger" did nothing
//     visible;
//   * a child's local offset was added in WORLD space, so a parent rotated 90
//     degrees moved its children the wrong way;
//   * bounds, culling and picking were all computed against the untransformed
//     box, so a scaled object was clickable and cullable in the wrong place.
//
// world_rot is a QUATERNION, not euler degrees, because composing two euler
// triples is a gimbal-lock trap: rotate a parent 90 degrees on X and every
// child's stored (rx, ry, rz) would have to be re-derived in a different order.
// Quaternions compose without a branch. Use compose_trs_mat4 /
// quat_from_euler_xyz_degrees below for the euler boundary.
struct Transform {
    float local_x = 0, local_y = 0, local_z = 0;
    float world_x = 0, world_y = 0, world_z = 0;
    float rot_x = 0, rot_y = 0, rot_z = 0; // degrees, XYZ euler order
    float scale_x = 1, scale_y = 1, scale_z = 1;
    // Composed world rotation/scale. Identity / unit for a root, and the
    // parent's composed values composed with this node's local ones otherwise.
    Quat world_rot;
    float world_scale_x = 1, world_scale_y = 1, world_scale_z = 1;
    ecs::Entity parent = ecs::kInvalidEntity;
    bool dirty = true;
};

/// The world matrix for this transform: translation (world_xyz), rotation
/// (world_rot) and scale (world_scale_*) composed in that order. The one place
/// a render matrix is built from a Transform — extraction, bounds, picking and
/// the gizmo all go through here, so they cannot disagree about where a thing
/// is or which way it faces.
Mat4 world_matrix(const Transform& t);

// Transform hierarchy helpers (operate on ecs::World)
void set_parent(ecs::World& world, ecs::Entity child, ecs::Entity parent);
void remove_parent(ecs::World& world, ecs::Entity child);
ecs::Entity get_parent(const ecs::World& world, ecs::Entity child);
std::vector<ecs::Entity> get_children(const ecs::World& world, ecs::Entity parent);

// Propagates local → world for all entities: position, rotation and scale.
// Uses iterative BFS to avoid recursion (a deep prefab chain would blow the
// stack otherwise).
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

/// The same transform from a QUATERNION instead of euler degrees, for the
/// hierarchy path: propagate_transforms accumulates world_rot, and a quaternion
/// is what it has to compose with. euler -> quat -> matrix must be byte-identical
/// to euler -> matrix, which compose_trs_quat_mat4_matching_compose_trs pins.
Mat4 compose_quat_scale_mat4(const Vec3& position, const Quat& rotation,
                             const Vec3& scale);

/// The local TRS of `t` as a matrix — the transform a CHILD inherits, i.e. what
/// a parent's world matrix has to be composed with. Public because prefab
/// instancing and the scene serializer both need "this node in its parent's
/// space" and neither should be re-deriving it.
Mat4 local_matrix(const Transform& t);

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
