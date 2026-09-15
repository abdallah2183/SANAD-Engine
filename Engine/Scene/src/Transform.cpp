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
    // Built through compose_trs_mat4: one convention, one code path from
    // euler degrees to matrix, so the forward and inverse directions cannot
    // drift apart (and no raw-array index juggling to get subtly wrong).
    return Quat::from_matrix(compose_trs_mat4(0.0f, 0.0f, 0.0f, rx_deg, ry_deg, rz_deg, 1.0f,
                                              1.0f, 1.0f));
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
    const Mat4 m = compose_trs_mat4(px, py, pz, rx_deg, ry_deg, rz_deg, sx, sy, sz);
    // Mat4 is row-major (m[row][col]); the legacy array is column-major
    // (out[col*4+row]). Reinterpreting the same 16 floats across the two
    // layouts is exactly a transpose, which is what converts between the
    // row-vector CPU convention and the column-vector GPU convention while
    // preserving the transform. Writing it as an explicit loop (rather than
    // duplicating the trig below) keeps the rotation in one place.
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out_m16[r * 4 + c] = m.m[r][c];
        }
    }
}

Mat4 compose_trs_mat4(float px, float py, float pz,
                      float rx_deg, float ry_deg, float rz_deg,
                      float sx, float sy, float sz) {
    constexpr float kPi = 3.14159265358979323846f;
    const float rx = rx_deg * kPi / 180.0f;
    const float ry = ry_deg * kPi / 180.0f;
    const float rz = rz_deg * kPi / 180.0f;
    const float cx = std::cos(rx), sxr = std::sin(rx);
    const float cy = std::cos(ry), syr = std::sin(ry);
    const float cz = std::cos(rz), szr = std::sin(rz);
    // R = Ry * Rx * Rz, same rotation the legacy column-major compose_trs
    // builds (r{row}{col} name the rotation entries before scaling).
    const float r00 = cy * cz + syr * sxr * szr;
    const float r10 = cx * szr;
    const float r20 = -syr * cz + cy * sxr * szr;
    const float r01 = -cy * szr + syr * sxr * cz;
    const float r11 = cx * cz;
    const float r21 = syr * szr + cy * sxr * cz;
    const float r02 = syr * cx;
    const float r12 = -sxr;
    const float r22 = cy * cx;
    // Row-vector storage: m[r][c] holds the legacy column-major byte
    // out[r*4+c], i.e. the same 16 floats reinterpreted across the two
    // layouts (row-major-flat of the row-vector form == column-major-flat
    // of the column-vector form). Translation lands in row 3, so
    // transform_point applies it; the w column stays (0,0,0,1).
    Mat4 m = Mat4::identity();
    m.m[0][0] = r00 * sx; m.m[0][1] = r10 * sx; m.m[0][2] = r20 * sx;
    m.m[1][0] = r01 * sy; m.m[1][1] = r11 * sy; m.m[1][2] = r21 * sy;
    m.m[2][0] = r02 * sz; m.m[2][1] = r12 * sz; m.m[2][2] = r22 * sz;
    m.m[0][3] = 0.0f; m.m[1][3] = 0.0f; m.m[2][3] = 0.0f;
    m.m[3][0] = px; m.m[3][1] = py; m.m[3][2] = pz; m.m[3][3] = 1.0f;
    return m;
}

} // namespace nf::scene
