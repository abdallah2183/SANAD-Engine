// NF/Scene2D/SpriteBatcher.cpp — sort + transform + index bake.

#include <NF/Scene2D/SpriteBatcher.hpp>
#include <NF/Scene2D/Camera2D.hpp>

#include <algorithm>

namespace nf::scene2d {

void SpriteBatcher::begin() {
    clear();
}

void SpriteBatcher::end(const Camera2D& camera) {
    bake(&camera);
}

void SpriteBatcher::end_identity() {
    bake(nullptr);
}

void SpriteBatcher::bake(const Camera2D* camera) {
    if (m_staging.empty()) return;

    // stable so equal (page, depth) sprites keep their submission order — the
    // difference between a deterministic bake and a per-run shuffle.
    std::stable_sort(m_staging.begin(), m_staging.end(),
                     [](const SpriteDraw& a, const SpriteDraw& b) {
                         if (a.page != b.page) return a.page < b.page;
                         return a.depth < b.depth;
                     });

    const usize reserve_verts = m_staging.size() * 4;
    const usize reserve_indices = m_staging.size() * 6;
    m_vertices.reserve(reserve_verts);
    m_indices.reserve(reserve_indices);

    u32 current_page = u32_max;
    for (const SpriteDraw& s : m_staging) {
        const Mat3x2 view = camera ? camera->transform_world_to_screen(s.parallax)
                                   : Mat3x2::identity();
        if (s.page != current_page || m_batches.empty()) {
            current_page = s.page;
            SpriteBatch b;
            b.page = s.page;
            b.vertex_offset = static_cast<u32>(m_vertices.size());
            b.index_offset = static_cast<u32>(m_indices.size());
            b.min_depth = s.depth;
            b.max_depth = s.depth;
            m_batches.push_back(b);
        }
        SpriteBatch& b = m_batches.back();
        b.min_depth = b.min_depth < s.depth ? b.min_depth : s.depth;
        b.max_depth = b.max_depth > s.depth ? b.max_depth : s.depth;

        emit_quad(s, view);

        b.vertex_count += 4;
        b.index_count += 6;
    }

    m_staging.clear();
}

void SpriteBatcher::emit_quad(const SpriteDraw& s, const Mat3x2& view) {
    const Vec2 anchor_offset = Vec2{s.anchor.x * s.size.x, s.anchor.y * s.size.y};

    f32 u0 = s.u0, u1 = s.u1, v0 = s.v0, v1 = s.v1;
    if (s.flip_x) std::swap(u0, u1);
    if (s.flip_y) std::swap(v0, v1);

    // Matrices apply left to right, so a corner of the unit square is scaled to
    // size first, shifted so the anchor sits at the origin, rotated about that
    // anchor, then parked at the sprite's position — `anchor` is both the
    // placement pivot and the rotation pivot, so a non-centred anchor still
    // spins the way an artist expects. Scaling before the anchor shift is what
    // makes `anchor` a fraction of the rendered quad rather than of the unit
    // square.
    const Mat3x2 local = Mat3x2::scaling(s.size) *
                         Mat3x2::translation(-anchor_offset) *
                         Mat3x2::rotation_deg(s.rotation_deg) *
                         Mat3x2::translation(s.position);
    // The view applies last: world space in, screen space out.
    const Mat3x2 m = local * view;

    const u32 base = static_cast<u32>(m_vertices.size());
    m_vertices.push_back({m.transform_point(Vec2{0.0f, 0.0f}).x,
                          m.transform_point(Vec2{0.0f, 0.0f}).y, u0, v0, s.color});
    m_vertices.push_back({m.transform_point(Vec2{1.0f, 0.0f}).x,
                          m.transform_point(Vec2{1.0f, 0.0f}).y, u1, v0, s.color});
    m_vertices.push_back({m.transform_point(Vec2{1.0f, 1.0f}).x,
                          m.transform_point(Vec2{1.0f, 1.0f}).y, u1, v1, s.color});
    m_vertices.push_back({m.transform_point(Vec2{0.0f, 1.0f}).x,
                          m.transform_point(Vec2{0.0f, 1.0f}).y, u0, v1, s.color});

    // Two triangles, counter-clockwise in y-down space (winding visible to a
    // default front-face setup). Flips swap UVs, not indices, so winding never
    // depends on sprite state.
    m_indices.push_back(base + 0u);
    m_indices.push_back(base + 1u);
    m_indices.push_back(base + 2u);
    m_indices.push_back(base + 0u);
    m_indices.push_back(base + 2u);
    m_indices.push_back(base + 3u);
}

} // namespace nf::scene2d
