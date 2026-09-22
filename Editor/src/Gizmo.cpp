#include <NF/Editor/Gizmo.hpp>
#include <NF/Editor/Commands.hpp>

#include <NF/Core/Math.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cmath>

namespace nf::editor {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Rounds `value` onto a grid of `step`, or returns it untouched when snapping
// is off for this channel. Negative values round symmetrically about zero, so
// a drag back through the origin lands on the origin, not on -step.
float snap_to_grid(float value, float step) {
    if (step <= 0.0f) {
        return value;
    }
    return std::round(value / step) * step;
}

void normalize3(float& x, float& y, float& z) {
    const float l = std::sqrt(x * x + y * y + z * z);
    if (l > 1e-9f) {
        x /= l;
        y /= l;
        z /= l;
    }
}

void camera_basis(const ViewCamera& cam, float fwd[3], float right[3], float up[3]) {
    float fx = cam.tx - cam.px;
    float fy = cam.ty - cam.py;
    float fz = cam.tz - cam.pz;
    normalize3(fx, fy, fz);
    // right = fwd x world_up(0,1,0) = (-fz, 0, fx)
    float r_x = -fz;
    float r_y = 0.0f;
    float r_z = fx;
    normalize3(r_x, r_y, r_z);
    if (r_x == 0.0f && r_y == 0.0f && r_z == 0.0f) {
        r_x = 1.0f;
    }
    // up = right x fwd
    const float u_x = r_y * fz - r_z * fy;
    const float u_y = r_z * fx - r_x * fz;
    const float u_z = r_x * fy - r_y * fx;
    fwd[0] = fx;
    fwd[1] = fy;
    fwd[2] = fz;
    right[0] = r_x;
    right[1] = r_y;
    right[2] = r_z;
    up[0] = u_x;
    up[1] = u_y;
    up[2] = u_z;
}

} // namespace

Ray pick_ray(const ViewCamera& cam, float ndc_x, float ndc_y) {
    float fwd[3]{}, right[3]{}, up[3]{};
    camera_basis(cam, fwd, right, up);
    const float tan_half = std::tan(cam.fov_y_deg * kPi / 360.0f);
    float dx = fwd[0] + right[0] * (ndc_x * tan_half * cam.aspect) + up[0] * (ndc_y * tan_half);
    float dy = fwd[1] + right[1] * (ndc_x * tan_half * cam.aspect) + up[1] * (ndc_y * tan_half);
    float dz = fwd[2] + right[2] * (ndc_x * tan_half * cam.aspect) + up[2] * (ndc_y * tan_half);
    normalize3(dx, dy, dz);
    Ray r;
    r.ox = cam.px;
    r.oy = cam.py;
    r.oz = cam.pz;
    r.dx = dx;
    r.dy = dy;
    r.dz = dz;
    return r;
}

bool ray_hit_aabb(const Ray& ray, const AABB& box, float& out_t) {
    float tmin = 0.0f;
    float tmax = 1e30f;
    const float o[3] = {ray.ox, ray.oy, ray.oz};
    const float d[3] = {ray.dx, ray.dy, ray.dz};
    const float mn[3] = {box.min_x, box.min_y, box.min_z};
    const float mx[3] = {box.max_x, box.max_y, box.max_z};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-9f) {
            if (o[i] < mn[i] || o[i] > mx[i]) {
                return false;
            }
        } else {
            float t0 = (mn[i] - o[i]) / d[i];
            float t1 = (mx[i] - o[i]) / d[i];
            if (t0 > t1) {
                const float tmp = t0;
                t0 = t1;
                t1 = tmp;
            }
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax) {
                return false;
            }
        }
    }
    out_t = tmin;
    return true;
}

GizmoDelta gizmo_delta_for_drag(GizmoMode mode, const ViewCamera& cam, float distance,
                                float ndc_x0, float ndc_y0, float ndc_x1, float ndc_y1) {
    GizmoDelta d;
    const float dnx = ndc_x1 - ndc_x0;
    const float dny = ndc_y1 - ndc_y0;
    if (mode == GizmoMode::Translate) {
        // World units per NDC at the target distance (matches perspective).
        const float tan_half = std::tan(cam.fov_y_deg * kPi / 360.0f);
        const float h = 2.0f * distance * tan_half;
        const float w = h * cam.aspect;
        float fwd[3]{}, right[3]{}, up[3]{};
        camera_basis(cam, fwd, right, up);
        const float ox = right[0] * (dnx * w * 0.5f) + up[0] * (dny * h * 0.5f);
        const float oy = right[1] * (dnx * w * 0.5f) + up[1] * (dny * h * 0.5f);
        const float oz = right[2] * (dnx * w * 0.5f) + up[2] * (dny * h * 0.5f);
        d.dx = ox;
        d.dy = oy;
        d.dz = oz;
    } else if (mode == GizmoMode::Rotate) {
        d.yaw_deg = dnx * 180.0f;
        d.pitch_deg = -dny * 180.0f;
    } else {
        d.dscale = dnx;
    }
    return d;
}

scene::Transform apply_gizmo_delta(const scene::Transform& base, const GizmoDelta& d,
                                   GizmoMode mode, GizmoSpace space, const GizmoSnap& snap) {
    scene::Transform out = base;
    if (mode == GizmoMode::Translate) {
        const float dx = snap_to_grid(d.dx, snap.translate_step);
        const float dy = snap_to_grid(d.dy, snap.translate_step);
        const float dz = snap_to_grid(d.dz, snap.translate_step);
        if (dx == 0.0f && dy == 0.0f && dz == 0.0f) {
            return out; // no movement: leave the base exactly as it was
        }
        if (space == GizmoSpace::Local) {
            // v0.1 propagates translation only (a child's world_xyz is
            // parent_world + local, no parent rotation), so a world-space
            // pointer delta becomes a local-axis delta by rotating back
            // through the entity's own rotation: local = R^-1 * world.
            // Parents that rotate do not drag their children in v0.1
            // (documented Transform scope); Local uses the entity's own axes.
            const Quat q =
                scene::quat_from_euler_xyz_degrees(base.rot_x, base.rot_y, base.rot_z);
            const Vec3 dl = q.conjugate().rotate(Vec3(dx, dy, dz));
            out.local_x = base.local_x + dl.x;
            out.local_y = base.local_y + dl.y;
            out.local_z = base.local_z + dl.z;
        } else {
            out.local_x = base.local_x + dx;
            out.local_y = base.local_y + dy;
            out.local_z = base.local_z + dz;
        }
    } else if (mode == GizmoMode::Rotate) {
        const float yaw = snap_to_grid(d.yaw_deg, snap.rotate_step_deg);
        const float pitch = snap_to_grid(d.pitch_deg, snap.rotate_step_deg);
        if (yaw == 0.0f && pitch == 0.0f) {
            return out;
        }
        // Euler angles do not compose (pitch then yaw is not yaw+pitch once
        // either leaves the zero plane), so the delta composes as a
        // quaternion and the result converts back to XYZ euler through the
        // canonical pair in NF/Scene/Transform.hpp.
        const Quat q_old =
            scene::quat_from_euler_xyz_degrees(base.rot_x, base.rot_y, base.rot_z);
        const Quat q_delta = scene::quat_from_euler_xyz_degrees(pitch, yaw, 0.0f);
        // World: the delta applies in world axes, so it pre-multiplies;
        // Local: it applies in the entity's own axes, so it post-multiplies.
        const Quat q_new = (space == GizmoSpace::World) ? (q_delta * q_old) : (q_old * q_delta);
        scene::euler_xyz_degrees_from_quat(q_new, out.rot_x, out.rot_y, out.rot_z);
    } else {
        // Scale snaps on the FACTOR grid (0.25 step -> 0.25x, 0.5x, 0.75x,
        // 1.0x...), not on the pointer delta: snapping the delta would make
        // the grid depend on where the gesture started.
        const float f = snap_to_grid(1.0f + d.dscale, snap.scale_step);
        const float nf = (f > 0.01f) ? f : 0.01f; // never collapse to zero/negative
        out.scale_x = base.scale_x * nf;
        out.scale_y = base.scale_y * nf;
        out.scale_z = base.scale_z * nf;
    }
    return out;
}

bool GizmoDrag::begin(ecs::World& world, const std::vector<ecs::Entity>& entities, GizmoMode mode,
                      GizmoSpace space, const GizmoSnap& snap, std::string& out_err) {
    cancel();
    std::vector<ecs::Entity> skipped;
    for (ecs::Entity e : entities) {
        if (!e.valid() || !world.is_alive(e)) {
            continue;
        }
        const auto* t = world.get<scene::Transform>(e);
        if (t == nullptr) {
            skipped.push_back(e);
            continue;
        }
        m_entities.push_back(e);
        m_starts.push_back(*t);
    }
    if (m_entities.empty()) {
        out_err = skipped.empty() ? "No entity selected" : "Selected entities have no Transform";
        return false;
    }
    if (!skipped.empty()) {
        // Reported, not fatal: a group selection may include non-transform
        // holders, and the draggable part of it should still move.
        out_err = "Skipped " + std::to_string(skipped.size()) +
                  (skipped.size() == 1 ? " entity" : " entities") + " without a Transform";
    }
    m_mode = mode;
    m_space = space;
    m_snap = snap;
    m_total = GizmoDelta{};
    m_active = true;
    return true;
}

bool GizmoDrag::begin(ecs::World& world, ecs::Entity e, GizmoMode mode, GizmoSpace space,
                      std::string& out_err) {
    std::vector<ecs::Entity> one;
    if (e.valid()) {
        one.push_back(e);
    }
    return begin(world, one, mode, space, GizmoSnap{}, out_err);
}

void GizmoDrag::accumulate(const GizmoDelta& d) {
    if (!m_active) {
        return;
    }
    if (m_mode == GizmoMode::Translate) {
        m_total.dx += d.dx;
        m_total.dy += d.dy;
        m_total.dz += d.dz;
    } else if (m_mode == GizmoMode::Rotate) {
        m_total.yaw_deg += d.yaw_deg;
        m_total.pitch_deg += d.pitch_deg;
    } else {
        m_total.dscale += d.dscale;
    }
}

std::unique_ptr<ICommand> GizmoDrag::commit(ecs::World& world) {
    if (!m_active) {
        return nullptr;
    }
    m_active = false;
    if (m_entities.empty()) {
        cancel();
        return nullptr;
    }
    // Keep only entries that actually moved (the snapped grid can hold the
    // accumulated delta at zero) and are still alive with a Transform. The
    // command is one transaction for the survivors, never one per entity.
    std::vector<SetTransformsCommand::Entry> moved;
    moved.reserve(m_entities.size());
    for (size_t i = 0; i < m_entities.size(); ++i) {
        if (!world.is_alive(m_entities[i])) {
            continue;
        }
        if (world.get<scene::Transform>(m_entities[i]) == nullptr) {
            continue;
        }
        const scene::Transform after =
            apply_gizmo_delta(m_starts[i], m_total, m_mode, m_space, m_snap);
        if (after.local_x == m_starts[i].local_x && after.local_y == m_starts[i].local_y &&
            after.local_z == m_starts[i].local_z && after.rot_x == m_starts[i].rot_x &&
            after.rot_y == m_starts[i].rot_y && after.rot_z == m_starts[i].rot_z &&
            after.scale_x == m_starts[i].scale_x && after.scale_y == m_starts[i].scale_y &&
            after.scale_z == m_starts[i].scale_z) {
            continue;
        }
        moved.push_back(SetTransformsCommand::Entry{m_entities[i], m_starts[i], after});
    }
    const std::string verb = (m_mode == GizmoMode::Translate)
                                 ? "Move"
                                 : (m_mode == GizmoMode::Rotate ? "Rotate" : "Scale");
    cancel();
    if (moved.empty()) {
        return nullptr; // a no-op drag never pollutes the undo stack
    }
    return std::make_unique<SetTransformsCommand>(std::move(moved), verb);
}

void GizmoDrag::cancel() {
    m_active = false;
    m_entities.clear();
    m_starts.clear();
    m_total = GizmoDelta{};
}

bool GizmoDrag::live_apply(ecs::World& world) {
    if (!m_active || m_entities.empty()) {
        return false;
    }
    bool applied = false;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        if (!world.is_alive(m_entities[i])) {
            continue;
        }
        auto* t = world.get<scene::Transform>(m_entities[i]);
        if (t == nullptr) {
            continue;
        }
        *t = apply_gizmo_delta(m_starts[i], m_total, m_mode, m_space, m_snap);
        t->dirty = true;
        applied = true;
    }
    return applied;
}

bool GizmoDrag::abort(ecs::World& world) {
    if (!m_active) {
        return false;
    }
    bool restored = false;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        if (!world.is_alive(m_entities[i])) {
            continue;
        }
        auto* t = world.get<scene::Transform>(m_entities[i]);
        if (t == nullptr) {
            continue;
        }
        *t = m_starts[i];
        t->dirty = true;
        restored = true;
    }
    cancel();
    return restored;
}

} // namespace nf::editor
