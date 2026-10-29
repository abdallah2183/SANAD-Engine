#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp>
#include <array>

namespace nf::scene2d {

/// Margin insets for 9-slice sprite borders (in world units or texture pixels)
struct NineSliceMargins {
    f32 left = 0.0f;
    f32 right = 0.0f;
    f32 top = 0.0f;
    f32 bottom = 0.0f;
};

/// UV coordinate boundary rectangle
struct UVRect {
    f32 u0 = 0.0f;
    f32 v0 = 0.0f;
    f32 u1 = 1.0f;
    f32 v1 = 1.0f;
};

/// Single quad geometry patch in the 9-slice grid
struct SliceQuad {
    Vec2 pos_min{0.0f, 0.0f};
    Vec2 pos_max{0.0f, 0.0f};
    UVRect uv{};
};

/// 9-Slice Grid Generator for scalable UI dialogs, health bars, and 2D platform borders
class NineSliceSprite {
public:
    NineSliceSprite() = default;
    NineSliceSprite(NineSliceMargins margins, UVRect uv_rect);

    void set_margins(NineSliceMargins margins) { m_margins = margins; }
    [[nodiscard]] const NineSliceMargins& margins() const noexcept { return m_margins; }

    void set_source_uv(UVRect uv) { m_source_uv = uv; }
    [[nodiscard]] const UVRect& source_uv() const noexcept { return m_source_uv; }

    /// Build the 9 quads for a specified destination size around a center point
    /// Output layout: [0]=Top-Left, [1]=Top-Center, [2]=Top-Right,
    ///                [3]=Mid-Left, [4]=Center,     [5]=Mid-Right,
    ///                [6]=Bot-Left, [7]=Bot-Center, [8]=Bot-Right
    [[nodiscard]] std::array<SliceQuad, 9> build_quads(const Vec2& target_size, const Vec2& center_pos = {0.0f, 0.0f}) const;

private:
    NineSliceMargins m_margins;
    UVRect m_source_uv;
};

} // namespace nf::scene2d
