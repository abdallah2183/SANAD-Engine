#include <NF/Scene2D/NineSlice.hpp>
#include <algorithm>

namespace nf::scene2d {

NineSliceSprite::NineSliceSprite(NineSliceMargins margins, UVRect uv_rect)
    : m_margins(margins)
    , m_source_uv(uv_rect) {}

std::array<SliceQuad, 9> NineSliceSprite::build_quads(const Vec2& target_size, const Vec2& center_pos) const {
    std::array<SliceQuad, 9> quads;

    const f32 target_w = std::max(0.001f, target_size.x);
    const f32 target_h = std::max(0.001f, target_size.y);

    // Clamp margins if they exceed target size
    f32 ml = m_margins.left;
    f32 mr = m_margins.right;
    f32 mt = m_margins.top;
    f32 mb = m_margins.bottom;

    if (ml + mr > target_w) {
        const f32 scale = target_w / (ml + mr);
        ml *= scale;
        mr *= scale;
    }
    if (mt + mb > target_h) {
        const f32 scale = target_h / (mt + mb);
        mt *= scale;
        mb *= scale;
    }

    const f32 half_w = target_w * 0.5f;
    const f32 half_h = target_h * 0.5f;

    // X positions (left to right)
    const f32 x0 = center_pos.x - half_w;
    const f32 x1 = x0 + ml;
    const f32 x3 = center_pos.x + half_w;
    const f32 x2 = x3 - mr;

    // Y positions (bottom to top)
    const f32 y0 = center_pos.y - half_h;
    const f32 y1 = y0 + mb;
    const f32 y3 = center_pos.y + half_h;
    const f32 y2 = y3 - mt;

    const f32 u_span = m_source_uv.u1 - m_source_uv.u0;
    const f32 v_span = m_source_uv.v1 - m_source_uv.v0;

    // Proportional UV margins
    const f32 u_margin_l = (target_w > 0.0f) ? (ml / target_w) * u_span * 0.25f : 0.0f;
    const f32 u_margin_r = (target_w > 0.0f) ? (mr / target_w) * u_span * 0.25f : 0.0f;
    const f32 v_margin_b = (target_h > 0.0f) ? (mb / target_h) * v_span * 0.25f : 0.0f;
    const f32 v_margin_t = (target_h > 0.0f) ? (mt / target_h) * v_span * 0.25f : 0.0f;

    const f32 u0 = m_source_uv.u0;
    const f32 u1 = u0 + u_margin_l;
    const f32 u3 = m_source_uv.u1;
    const f32 u2 = u3 - u_margin_r;

    const f32 v0 = m_source_uv.v0;
    const f32 v1 = v0 + v_margin_b;
    const f32 v3 = m_source_uv.v1;
    const f32 v2 = v3 - v_margin_t;

    const f32 xs[4] = { x0, x1, x2, x3 };
    const f32 ys[4] = { y0, y1, y2, y3 };
    const f32 us[4] = { u0, u1, u2, u3 };
    const f32 vs[4] = { v0, v1, v2, v3 };

    size_t idx = 0;
    // Row 2 (Top), Row 1 (Center), Row 0 (Bottom)
    for (int row = 2; row >= 0; --row) {
        for (int col = 0; col < 3; ++col) {
            SliceQuad& q = quads[idx++];
            q.pos_min = Vec2(xs[col], ys[row]);
            q.pos_max = Vec2(xs[col + 1], ys[row + 1]);
            q.uv.u0 = us[col];
            q.uv.u1 = us[col + 1];
            q.uv.v0 = vs[row];
            q.uv.v1 = vs[row + 1];
        }
    }

    return quads;
}

} // namespace nf::scene2d
