#pragma once

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

// Composes a column-major 4x4 matrix (same layout as rendering::Mat4:
// out[12..14] hold translation) from translation + XYZ euler rotation
// (degrees, applied as R = Ry * Rx * Rz) + scale: M = T * R * S.
// Shared by the Runtime (render matrix) and the Editor gizmo so both agree.
void compose_trs(float px, float py, float pz,
                 float rx_deg, float ry_deg, float rz_deg,
                 float sx, float sy, float sz,
                 float out_m16[16]);

} // namespace nf::scene
