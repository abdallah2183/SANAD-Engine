#pragma once

// NF/Scene2D/Math2D.hpp — 2D primitives: rectangles, affine transforms, casting.
// Design doc Section 54 (2D Engine).
//
// Conventions (shared by every header in this module):
//   - Screen space has its origin at the top-left, y grows downward. World
//     space uses the same orientation so a world-to-screen mapping is a pure
//     scale+offset and never flips an axis. Flipping an axis here would mirror
//     every sprite and tile the moment a camera scrolled.
//   - Mat3x2 is row-vector (p' = p * M), the same convention as nf::Mat4 and
//     scene::compose_trs_mat4, so a 2D layer can be uploaded to the existing
//     renderer without a transpose.
//   - Angles are degrees in the public API (matching scene::Transform) and
//     radians inside solvers that integrate them.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <cmath>
#include <cstring>

namespace nf::scene2d {

/// Axis-aligned rectangle in world or screen units.
struct Rect {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;

    constexpr Rect() = default;
    constexpr Rect(f32 x_, f32 y_, f32 w_, f32 h_) : x(x_), y(y_), w(w_), h(h_) {}

    /// Build from a centre + half-extents (physics shapes think in halves).
    static constexpr Rect from_center(Vec2 center, Vec2 half) {
        return Rect{center.x - half.x, center.y - half.y, half.x * 2.0f, half.y * 2.0f};
    }

    constexpr f32 left() const { return x; }
    constexpr f32 right() const { return x + w; }
    constexpr f32 top() const { return y; }
    constexpr f32 bottom() const { return y + h; }
    constexpr Vec2 min() const { return {x, y}; }
    constexpr Vec2 max() const { return {x + w, y + h}; }
    constexpr Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    constexpr Vec2 half() const { return {w * 0.5f, h * 0.5f}; }

    constexpr bool contains(Vec2 p) const {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }

    /// Half-open overlap test: two tiles that merely touch are NOT overlapping.
    /// Closed intervals would make every adjacent solid tile report a contact,
    /// which poisons both collision extraction and navigation.
    constexpr bool overlaps(const Rect& o) const {
        return x < o.x + o.w && x + w > o.x && y < o.y + o.h && y + h > o.y;
    }

    /// Smallest rect covering both (used by broadphase bounds and tile merges).
    Rect merged(const Rect& o) const {
        const f32 lx = x < o.x ? x : o.x;
        const f32 ly = y < o.y ? y : o.y;
        const f32 rx = right() > o.right() ? right() : o.right();
        const f32 ry = bottom() > o.bottom() ? bottom() : o.bottom();
        return Rect{lx, ly, rx - lx, ry - ly};
    }

    Rect translated(Vec2 d) const { return Rect{x + d.x, y + d.y, w, h}; }
};

/// 2D affine transform: rotation/scale plus translation, 6 floats.
///
/// Row-vector: p' = p * M with
///   M = | xx xy  0 |
///       | yx yy  0 |
///       | tx ty  1 |
/// so `transform_point` reads off directly and composition is a plain matrix
/// product. Keeping the storage layout identical to the 3x3 part of nf::Mat4
/// means a 2D camera view can be widened to a 4x4 upload without reordering.
struct Mat3x2 {
    f32 xx = 1.0f, xy = 0.0f;
    f32 yx = 0.0f, yy = 1.0f;
    f32 tx = 0.0f, ty = 0.0f;

    static constexpr Mat3x2 identity() { return Mat3x2{}; }

    static constexpr Mat3x2 translation(Vec2 t) {
        return Mat3x2{1.0f, 0.0f, 0.0f, 1.0f, t.x, t.y};
    }

    static constexpr Mat3x2 scaling(Vec2 s) {
        return Mat3x2{s.x, 0.0f, 0.0f, s.y, 0.0f, 0.0f};
    }

    /// Clockwise in a y-down space, matching the direction a positive rotation
    /// visually turns on screen.
    static Mat3x2 rotation_deg(f32 deg) {
        const f32 rad = to_radians(deg);
        const f32 c = std::cos(rad);
        const f32 s = std::sin(rad);
        return Mat3x2{c, s, -s, c, 0.0f, 0.0f};
    }

    /// TRS composition. Matrices in a product apply left to right (p*M = p
    /// applied to A first when M = A*B), so scale-then-rotate-then-translate is
    /// written S*R*T here — the row-vector form of the column-vector T*R*S that
    /// scene::compose_trs uses. The object origin ends at `translation` and a
    /// point one unit to its right ends `rotation_deg` clockwise from there.
    static Mat3x2 compose(Vec2 translation, f32 rotation_deg, Vec2 scale) {
        return Mat3x2::scaling(scale) * Mat3x2::rotation_deg(rotation_deg)
             * Mat3x2::translation(translation);
    }

    /// M = A * B. Derived by writing out the 2x2 products; the translation row
    /// picks up B's translation plus A's rotation of it.
    constexpr Mat3x2 operator*(const Mat3x2& b) const {
        return Mat3x2{
            xx * b.xx + xy * b.yx, xx * b.xy + xy * b.yy,
            yx * b.xx + yy * b.yx, yx * b.xy + yy * b.yy,
            tx * b.xx + ty * b.yx + b.tx, tx * b.xy + ty * b.yy + b.ty};
    }

    /// Full affine: position, rotation and scale all applied.
    constexpr Vec2 transform_point(Vec2 p) const {
        return {p.x * xx + p.y * yx + tx, p.x * xy + p.y * yy + ty};
    }

    /// Direction only: translation dropped, so normals and velocities rotate
    /// without being re-anchored.
    constexpr Vec2 transform_vector(Vec2 v) const {
        return {v.x * xx + v.y * yx, v.x * xy + v.y * yy};
    }

    /// Inverse of an orthonormal rotation/scale matrix. A general 2x2 inverse
    /// would need a determinant guard; this is only ever called on view
    /// matrices built from non-degenerate camera state, and a zero-determinant
    /// camera is a caller bug worth surfacing as NaN rather than hiding.
    Mat3x2 inverse_orthonormal() const {
        const f32 det = xx * yy - xy * yx;
        const f32 inv = 1.0f / det;
        Mat3x2 r;
        r.xx = yy * inv;
        r.xy = -xy * inv;
        r.yx = -yx * inv;
        r.yy = xx * inv;
        // Inverse translation = -R^-1 * t
        r.tx = -(r.xx * tx + r.yx * ty);
        r.ty = -(r.xy * tx + r.yy * ty);
        return r;
    }
};

/// Triangle area * 2 — the orientation test. Positive when (a,b,c) winds
/// clockwise in y-down space, which is how the batcher decides whether a
/// flipped sprite needs a counter-clockwise index order to stay front-facing.
inline f32 signed_area_2x(Vec2 a, Vec2 b, Vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/// Nearest point on a segment [a,b] to p. Used by circle-vs-segment collision
/// (tile edges, capsule shapes) and by the 2D light occluder queries.
inline Vec2 closest_point_on_segment(Vec2 a, Vec2 b, Vec2 p) {
    const Vec2 d = b - a;
    const f32 len_sq = d.length_sq();
    if (len_sq <= EPSILON) return a;
    const f32 t = (p - a).dot(d) / len_sq;
    const f32 tc = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return a + d * tc;
}

/// Factors an affine matrix back into the translation/rotation/scale triple
/// that `Mat3x2::compose` builds from — the inverse of `compose` for the
/// transform pipeline. Rotation is the heading of the local x axis; the x scale
/// is that axis' length and the y scale follows from the determinant, so a
/// mirrored parent (negative determinant) reads back as a negative y scale and
/// the batcher draws it flipped rather than inside-out.
///
/// Exact for any product of TRS matrices whose rows stay orthogonal. A
/// non-uniform scale *followed by* a rotation shears, and no rotation-scale
/// pair expresses a shear; in that case this returns the nearest
/// rotation-scale factorisation (polar decomposition), which is what a sprite
/// renderer wants anyway. Returns false only when the linear part collapses,
/// in which case the outputs are the identity transform so a caller never sees
/// NaN scale.
inline bool decompose_affine(const Mat3x2& m, Vec2& out_translation,
                             f32& out_rotation_deg, Vec2& out_scale) {
    const f32 sx = std::hypot(m.xx, m.xy);
    out_translation = Vec2{m.tx, m.ty};
    if (sx <= EPSILON) {
        out_rotation_deg = 0.0f;
        out_scale = Vec2{1.0f, 1.0f};
        return false;
    }
    const f32 det = m.xx * m.yy - m.xy * m.yx;
    out_rotation_deg = std::atan2(m.xy, m.xx) * RAD_TO_DEG;
    out_scale = Vec2{sx, det / sx};
    return true;
}

/// Deterministic noise: maps any 32-bit key into [0, 1). Design doc Section 57
/// (Particles) and Section 114 (Determinism) — every incidental variation in
/// the engine (camera shake, particle spread, flicker) derives from this one
/// function, so no RNG is ever seeded and two runs of the same input sequence
/// produce identical output.
inline f32 hash_u32_to_unit(u32 bits) {
    bits ^= 0x9E3779B9u;
    bits = (bits ^ (bits >> 16)) * 0x85EBCA6Bu;
    bits = (bits ^ (bits >> 13)) * 0xC2B2AE35u;
    bits ^= bits >> 16;
    return static_cast<f32>(bits) / static_cast<f32>(0xFFFFFFFFu);
}

/// Float-keyed flavour for callers whose key is a world coordinate or a phase.
inline f32 hash_to_unit(f32 key) {
    u32 bits = 0;
    std::memcpy(&bits, &key, sizeof(bits));
    return hash_u32_to_unit(bits);
}

} // namespace nf::scene2d
