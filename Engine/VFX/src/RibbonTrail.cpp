#include <NF/VFX/RibbonTrail.hpp>
#include <algorithm>
#include <cmath>

namespace nf::vfx {

RibbonTrail::RibbonTrail(float lifetime, float min_vertex_dist)
    : m_lifetime((lifetime > 0.01f) ? lifetime : 0.01f), m_min_dist(min_vertex_dist) {}

void RibbonTrail::add_point(const Vec3& position, float width, const Vec4& color) {
    if (!m_points.empty()) {
        float dist = (position - m_points.back().position).length();
        if (dist < m_min_dist) {
            return;
        }
    }

    RibbonPoint pt;
    pt.position = position;
    pt.age = 0.0f;
    pt.width = std::max(0.001f, width);
    pt.color = color;
    m_points.push_back(pt);
}

void RibbonTrail::update(float dt) {
    if (dt <= 0.0f) {
        return;
    }

    for (auto& pt : m_points) {
        pt.age += dt;
    }

    m_points.erase(
        std::remove_if(m_points.begin(), m_points.end(),
                       [this](const RibbonPoint& pt) { return pt.age >= m_lifetime; }),
        m_points.end());
}

void RibbonTrail::clear() {
    m_points.clear();
}

void RibbonTrail::generate_mesh(std::vector<RibbonVertex>& out_vertices,
                               std::vector<uint32_t>& out_indices,
                               const Vec3& camera_pos) const {
    out_vertices.clear();
    out_indices.clear();

    if (m_points.size() < 2) {
        return;
    }

    size_t n = m_points.size();
    out_vertices.reserve(n * 2);
    out_indices.reserve((n - 1) * 6);

    for (size_t i = 0; i < n; ++i) {
        const auto& pt = m_points[i];

        Vec3 tangent(0.0f, 0.0f, 1.0f);
        if (i == 0) {
            tangent = m_points[1].position - m_points[0].position;
        } else if (i == n - 1) {
            tangent = m_points[n - 1].position - m_points[n - 2].position;
        } else {
            tangent = m_points[i + 1].position - m_points[i - 1].position;
        }

        float t_len = tangent.length();
        tangent = (t_len > 0.0001f) ? (tangent / t_len) : Vec3(0.0f, 0.0f, 1.0f);

        Vec3 view_dir = pt.position - camera_pos;
        float v_len = view_dir.length();
        view_dir = (v_len > 0.0001f) ? (view_dir / v_len) : Vec3(0.0f, 1.0f, 0.0f);

        Vec3 normal = tangent.cross(view_dir);
        float n_len = normal.length();
        if (n_len > 0.0001f) {
            normal = normal / n_len;
        } else {
            normal = tangent.cross(Vec3(0.0f, 1.0f, 0.0f)).normalized();
        }

        float life_fraction = std::clamp(pt.age / m_lifetime, 0.0f, 1.0f);
        float current_width = pt.width * (1.0f - life_fraction);
        float alpha = pt.color.w * (1.0f - life_fraction);

        Vec4 vcolor(pt.color.x, pt.color.y, pt.color.z, alpha);
        float v_coord = static_cast<float>(i) / static_cast<float>(n - 1);

        RibbonVertex v_left;
        v_left.position = pt.position - normal * (0.5f * current_width);
        v_left.uv = Vec2(0.0f, v_coord);
        v_left.color = vcolor;

        RibbonVertex v_right;
        v_right.position = pt.position + normal * (0.5f * current_width);
        v_right.uv = Vec2(1.0f, v_coord);
        v_right.color = vcolor;

        out_vertices.push_back(v_left);
        out_vertices.push_back(v_right);
    }

    for (size_t i = 0; i + 1 < n; ++i) {
        uint32_t v0 = static_cast<uint32_t>(i * 2);
        uint32_t v1 = static_cast<uint32_t>(i * 2 + 1);
        uint32_t v2 = static_cast<uint32_t>((i + 1) * 2);
        uint32_t v3 = static_cast<uint32_t>((i + 1) * 2 + 1);

        // Quad triangles: (v0, v1, v2) and (v2, v1, v3)
        out_indices.push_back(v0);
        out_indices.push_back(v1);
        out_indices.push_back(v2);

        out_indices.push_back(v2);
        out_indices.push_back(v1);
        out_indices.push_back(v3);
    }
}

} // namespace nf::vfx
