#pragma once

#include <NF/Core/Math.hpp>
#include <vector>
#include <cstdint>

namespace nf::vfx {

/// Vertex data generated for rendering dynamic ribbon and trail strips.
struct RibbonVertex {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec2 uv{0.0f, 0.0f};
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct RibbonPoint {
    Vec3 position{0.0f, 0.0f, 0.0f};
    float age = 0.0f;
    float width = 0.2f;
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

/// Dynamic camera-facing ribbon trail generator (for sword slashes, projectile trails, vehicle tire tracks).
class RibbonTrail {
public:
    explicit RibbonTrail(float lifetime = 1.0f, float min_vertex_dist = 0.05f);

    /// Appends a new sample point if moved farther than min_vertex_dist.
    void add_point(const Vec3& position, float width = 0.2f, const Vec4& color = Vec4(1.0f, 1.0f, 1.0f, 1.0f));

    /// Advances trail age, decays width/alpha, and removes expired points.
    void update(float dt);

    void clear();

    size_t point_count() const { return m_points.size(); }
    const std::vector<RibbonPoint>& points() const { return m_points; }

    void set_lifetime(float lifetime) { m_lifetime = (lifetime > 0.01f) ? lifetime : 0.01f; }
    float lifetime() const { return m_lifetime; }

    void set_min_vertex_distance(float dist) { m_min_dist = dist; }
    float min_vertex_distance() const { return m_min_dist; }

    /// Generates triangulated camera-facing quad strips with continuous UV coordinates.
    void generate_mesh(std::vector<RibbonVertex>& out_vertices,
                       std::vector<uint32_t>& out_indices,
                       const Vec3& camera_pos = Vec3(0.0f, 0.0f, -10.0f)) const;

private:
    float m_lifetime = 1.0f;
    float m_min_dist = 0.05f;
    std::vector<RibbonPoint> m_points;
};

} // namespace nf::vfx
