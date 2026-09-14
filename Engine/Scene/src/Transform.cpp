#include <NF/Scene/Transform.hpp>
#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>

namespace nf::scene {

void set_parent(ecs::World& world, ecs::Entity child, ecs::Entity parent) {
    if (!world.is_alive(child)) return;
    if (parent.valid() && !world.is_alive(parent)) return;
    if (child == parent) return;

    auto* child_t = world.get<Transform>(child);
    if (!child_t) child_t = &world.add<Transform>(child);

    // Remove from old parent's children list
    if (child_t->parent.valid()) {
        auto* old_parent_t = world.get<Transform>(child_t->parent);
        if (old_parent_t) {
            // We store children implicitly via parent field; for get_children we iterate all transforms
            // To keep it simple, we don't maintain a separate children list in the component.
            // Instead, get_children scans the world. This avoids stale children lists.
        }
    }

    child_t->parent = parent;
    child_t->dirty = true;

    // Prevent cycles: if parent is descendant of child, reject
    if (parent.valid()) {
        ecs::Entity cur = parent;
        std::set<u32> visited;
        while (cur.valid()) {
            if (cur == child) {
                // Cycle detected, revert
                child_t->parent = ecs::kInvalidEntity;
                NF_LOG_WARN(LogCategory::Core, "set_parent: cycle detected, rejected");
                return;
            }
            if (visited.count(cur.id)) break;
            visited.insert(cur.id);
            auto* cur_t = world.get<Transform>(cur);
            if (!cur_t) break;
            cur = cur_t->parent;
        }
    }
}

void remove_parent(ecs::World& world, ecs::Entity child) {
    set_parent(world, child, ecs::kInvalidEntity);
}

ecs::Entity get_parent(const ecs::World& world, ecs::Entity child) {
    auto* t = world.get<Transform>(child);
    return t ? t->parent : ecs::kInvalidEntity;
}

std::vector<ecs::Entity> get_children(const ecs::World& world, ecs::Entity parent) {
    std::vector<ecs::Entity> out;
    auto entities = world.query<Transform>();
    for (ecs::Entity e : entities) {
        auto* t = world.get<Transform>(e);
        if (t && t->parent == parent) out.push_back(e);
    }
    return out;
}

void propagate_transforms(ecs::World& world) {
    // BFS from roots (entities with no parent or parent not alive)
    auto all = world.query<Transform>();
    // Build parent -> children map for fast traversal
    std::map<u32, std::vector<ecs::Entity>> children_map;
    std::vector<ecs::Entity> roots;
    for (ecs::Entity e : all) {
        auto* t = world.get<Transform>(e);
        if (!t) continue;
        if (!t->parent.valid() || !world.is_alive(t->parent) || !world.has<Transform>(t->parent)) {
            roots.push_back(e);
        } else {
            children_map[t->parent.id].push_back(e);
        }
    }

    std::queue<ecs::Entity> q;
    for (auto r : roots) q.push(r);

    // Roots: world = local
    for (auto r : roots) {
        auto* t = world.get<Transform>(r);
        if (t) {
            t->world_x = t->local_x;
            t->world_y = t->local_y;
            t->world_z = t->local_z;
            t->dirty = false;
        }
    }

    // BFS
    std::set<u32> visited;
    for (auto r : roots) visited.insert(r.id);

    while (!q.empty()) {
        ecs::Entity cur = q.front(); q.pop();
        auto* cur_t = world.get<Transform>(cur);
        if (!cur_t) continue;
        auto it = children_map.find(cur.id);
        if (it == children_map.end()) continue;
        for (ecs::Entity child : it->second) {
            if (visited.count(child.id)) continue;
            visited.insert(child.id);
            auto* child_t = world.get<Transform>(child);
            if (!child_t) continue;
            child_t->world_x = cur_t->world_x + child_t->local_x;
            child_t->world_y = cur_t->world_y + child_t->local_y;
            child_t->world_z = cur_t->world_z + child_t->local_z;
            child_t->dirty = false;
            q.push(child);
        }
    }
}

void transform_system(ecs::World& world) {
    propagate_transforms(world);
}

Quat quat_from_euler_xyz_degrees(float rx_deg, float ry_deg, float rz_deg) {
    // Built through compose_trs rather than by composing axis rotations here:
    // that keeps the convention in exactly one function, so the forward and
    // inverse directions cannot drift apart.
    float m16[16];
    compose_trs(0.0f, 0.0f, 0.0f, rx_deg, ry_deg, rz_deg, 1.0f, 1.0f, 1.0f, m16);
    Mat4 m = Mat4::identity();
    for (int i = 0; i < 16; ++i) {
        m.m[i / 4][i % 4] = m16[i]; // m16 is column-major: [col * 4 + row]
    }
    return Quat::from_matrix(m);
}

void euler_xyz_degrees_from_quat(const Quat& q, float& out_rx, float& out_ry, float& out_rz) {
    // Mat4 is column-major, so m[col][row] is R[row][col].
    const Mat4 m = q.to_matrix();

    // From R = Ry * Rx * Rz:
    //   R[1][2] = -sin(x)
    //   R[0][2] = sin(y)cos(x),  R[2][2] = cos(y)cos(x)
    //   R[1][0] = cos(x)sin(z),  R[1][1] = cos(x)cos(z)
    const float r12 = m.m[2][1];
    const float r02 = m.m[2][0];
    const float r22 = m.m[2][2];
    const float r10 = m.m[0][1];
    const float r11 = m.m[1][1];

    // asin is only defined on [-1, 1]; a quaternion that has drifted off the
    // unit sphere can push r12 just outside it and produce a NaN rotation.
    const float sin_x = std::clamp(-r12, -1.0f, 1.0f);
    const float rx = std::asin(sin_x);
    const float cy_cx = std::sqrt(r02 * r02 + r22 * r22);

    float ry;
    float rz;
    if (cy_cx > 1e-6f) {
        ry = std::atan2(r02, r22);
        rz = std::atan2(r10, r11);
    } else {
        // Gimbal lock: cos(x) == 0, so Y and Z rotate the same axis and only
        // their sum is determined. Pin Z to zero and put the whole turn in Y,
        // which reproduces the same orientation.
        ry = std::atan2(-m.m[0][2], m.m[0][0]);
        rz = 0.0f;
    }

    constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
    out_rx = rx * kRadToDeg;
    out_ry = ry * kRadToDeg;
    out_rz = rz * kRadToDeg;
}

void compose_trs(float px, float py, float pz,
                 float rx_deg, float ry_deg, float rz_deg,
                 float sx, float sy, float sz,
                 float out_m16[16]) {
    constexpr float kPi = 3.14159265358979323846f;
    const float rx = rx_deg * kPi / 180.0f;
    const float ry = ry_deg * kPi / 180.0f;
    const float rz = rz_deg * kPi / 180.0f;
    const float cx = std::cos(rx), sxr = std::sin(rx);
    const float cy = std::cos(ry), syr = std::sin(ry);
    const float cz = std::cos(rz), szr = std::sin(rz);
    // R = Ry * Rx * Rz (column-major rotations about Y, X, Z)
    // Ry = [cy,0,-syr; 0,1,0; syr,0,cy], Rx = [1,0,0; 0,cx,sxr; 0,-sxr,cx],
    // Rz = [cz,szr,0; -szr,cz,0; 0,0,1]  (column-major storage below)
    float r00 = cy * cz + syr * sxr * szr;
    float r10 = cx * szr;
    float r20 = -syr * cz + cy * sxr * szr;
    float r01 = -cy * szr + syr * sxr * cz;
    float r11 = cx * cz;
    float r21 = syr * szr + cy * sxr * cz;
    float r02 = syr * cx;
    float r12 = -sxr;
    float r22 = cy * cx;
    // M = T * R * S: scale the rotation columns, then set translation.
    out_m16[0] = r00 * sx; out_m16[1] = r10 * sx; out_m16[2] = r20 * sx; out_m16[3] = 0.0f;
    out_m16[4] = r01 * sy; out_m16[5] = r11 * sy; out_m16[6] = r21 * sy; out_m16[7] = 0.0f;
    out_m16[8] = r02 * sz; out_m16[9] = r12 * sz; out_m16[10] = r22 * sz; out_m16[11] = 0.0f;
    out_m16[12] = px; out_m16[13] = py; out_m16[14] = pz; out_m16[15] = 1.0f;
}

} // namespace nf::scene
