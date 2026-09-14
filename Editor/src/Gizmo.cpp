#include <NF/Editor/Gizmo.hpp>
#include <NF/Editor/Commands.hpp>

#include <algorithm>
#include <cmath>

namespace nf::editor {

namespace {

constexpr float kPi = 3.14159265358979323846f;

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
                                   GizmoMode mode, GizmoSpace space) {
    (void)space; // v0.1: rotation applies to local euler in both spaces (documented).
    scene::Transform out = base;
    if (mode == GizmoMode::Translate) {
        out.local_x = base.local_x + d.dx;
        out.local_y = base.local_y + d.dy;
        out.local_z = base.local_z + d.dz;
    } else if (mode == GizmoMode::Rotate) {
        out.rot_y = base.rot_y + d.yaw_deg;
        out.rot_x = base.rot_x + d.pitch_deg;
    } else {
        const float f = 1.0f + d.dscale;
        const float nf = (f > 0.01f) ? f : 0.01f;
        out.scale_x = base.scale_x * nf;
        out.scale_y = base.scale_y * nf;
        out.scale_z = base.scale_z * nf;
    }
    return out;
}

bool GizmoDrag::begin(ecs::World& world, ecs::Entity e, GizmoMode mode, GizmoSpace space,
                      std::string& out_err) {
    cancel();
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "No entity selected";
        return false;
    }
    const auto* t = world.get<scene::Transform>(e);
    if (t == nullptr) {
        out_err = "Selected entity has no Transform";
        return false;
    }
    m_entity = e;
    m_mode = mode;
    m_space = space;
    m_start = *t;
    m_total = GizmoDelta{};
    m_active = true;
    return true;
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
    if (!m_entity.valid() || !world.is_alive(m_entity)) {
        m_entity = ecs::kInvalidEntity;
        return nullptr;
    }
    const auto* cur = world.get<scene::Transform>(m_entity);
    if (cur == nullptr) {
        m_entity = ecs::kInvalidEntity;
        return nullptr;
    }
    const scene::Transform after = apply_gizmo_delta(m_start, m_total, m_mode, m_space);
    ecs::Entity e = m_entity;
    m_entity = ecs::kInvalidEntity;
    // A no-op drag still yields no command (keeps the undo stack clean).
    if (after.local_x == m_start.local_x && after.local_y == m_start.local_y &&
        after.local_z == m_start.local_z && after.rot_x == m_start.rot_x &&
        after.rot_y == m_start.rot_y && after.rot_z == m_start.rot_z &&
        after.scale_x == m_start.scale_x && after.scale_y == m_start.scale_y &&
        after.scale_z == m_start.scale_z) {
        return nullptr;
    }
    return std::make_unique<SetTransformCommand>(e, m_start, after);
}

void GizmoDrag::cancel() {
    m_active = false;
    m_entity = ecs::kInvalidEntity;
    m_total = GizmoDelta{};
}

} // namespace nf::editor
