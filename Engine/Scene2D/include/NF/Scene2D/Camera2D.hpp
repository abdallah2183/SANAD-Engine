#pragma once

// NF/Scene2D/Camera2D.hpp — 2D camera: view transform, zoom, rotation,
// pixel-perfect snapping, parallax layers.
// Design doc Section 54 (2D Engine): "Pixel perfect", "Cameras", "Parallax".

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include "Math2D.hpp"

#include <cmath>

namespace nf::scene2d {

/// Parallax factor attached to a background layer. 1.0 = locked to the world
/// (the gameplay layer), <1.0 = scrolls slower (far background), >1.0 = scrolls
/// faster (foreground props). A layer at 0.5 appears to sit twice as far away.
struct ParallaxLayer {
    f32 scroll = 1.0f;   ///< Effective camera travel per unit of world travel.
    f32 auto_scale = 1.0f; ///< Extra zoom applied to this layer only.

    static constexpr ParallaxLayer world() { return {1.0f, 1.0f}; }
    static constexpr ParallaxLayer background(f32 distance) { return {distance, distance}; }
};

class Camera2D {
public:
    /// World position at the centre of the viewport.
    Vec2 position{0.0f, 0.0f};
    /// Zoom multiplier on top of `pixels_per_unit`. zoom > 1 enlarges.
    f32 zoom = 1.0f;
    /// Camera roll, degrees, clockwise in y-down space.
    f32 rotation_deg = 0.0f;
    /// Render target size in pixels. Defaults to a common window size so a
    /// freshly constructed camera has a sane projection.
    u32 viewport_w = 1280;
    u32 viewport_h = 720;
    /// How many texture pixels make up one world unit at zoom 1. With 16 PPU, a
    /// 16x16 tile occupies exactly one world unit.
    u32 pixels_per_unit = 16;
    /// Snap the camera so texels land on pixel boundaries. Only meaningful with
    /// an axis-aligned camera; rotation deliberately disables the snap (there is
    /// no pixel grid to align to while spinning).
    bool pixel_perfect = true;

    /// Pixels rendered per world unit. The single conversion factor every
    /// world<->screen routine below derives from.
    f32 pixels_per_world_unit() const {
        return zoom * static_cast<f32>(pixels_per_unit);
    }

    /// Centre of the viewport in pixels — half is computed in f32 so an odd
    /// viewport produces a .5 centre rather than silently dropping a pixel.
    Vec2 viewport_center() const {
        return Vec2{static_cast<f32>(viewport_w), static_cast<f32>(viewport_h)} * 0.5f;
    }

    /// Applies pixel-perfect snapping in place and returns the snapped
    /// position. The snap rounds the camera onto the texel grid: a camera that
    /// sits on a texel boundary puts every world point on a pixel boundary too,
    /// which is what stops pixel art from shimmering as the camera moves.
    Vec2 snapped_position() const {
        if (!pixel_perfect || rotation_deg != 0.0f) return position;
        const f32 step = 1.0f / pixels_per_world_unit();
        const f32 sx = std::round(position.x / step) * step;
        const f32 sy = std::round(position.y / step) * step;
        return {sx, sy};
    }

    /// Effective camera origin for a parallax layer: the camera travels
    /// `scroll` times as far, so the layer lags behind the world.
    Vec2 effective_position(f32 parallax) const {
        return snapped_position() * parallax;
    }

    Vec2 world_to_screen(Vec2 world, f32 parallax = 1.0f) const {
        return transform_world_to_screen(parallax).transform_point(world);
    }

    Vec2 screen_to_world(Vec2 screen, f32 parallax = 1.0f) const {
        return transform_screen_to_world(parallax).transform_point(screen);
    }

    /// Full view transform for a layer: world -> screen pixels. The 2D analogue
    /// of the 3D view-projection matrix, and the one thing a renderer needs to
    /// draw the layer.
    Mat3x2 transform_world_to_screen(f32 parallax = 1.0f) const {
        const f32 scale = pixels_per_world_unit() * parallax;
        const Vec2 origin = effective_position(parallax);
        // Matrices apply left to right: move the world under the camera, rotate
        // it the opposite way to the camera's roll, scale to pixels, then park
        // the origin at the viewport centre.
        return Mat3x2::translation(-origin) * Mat3x2::rotation_deg(-rotation_deg)
             * Mat3x2::scaling(Vec2{scale, scale}) * Mat3x2::translation(viewport_center());
    }

    Mat3x2 transform_screen_to_world(f32 parallax = 1.0f) const {
        return transform_world_to_screen(parallax).inverse_orthonormal();
    }

    /// World-space rectangle visible through the camera at the given parallax.
    /// Used for culling sprites and streaming tilemap chunks.
    Rect visible_bounds(f32 parallax = 1.0f) const {
        const Mat3x2 inv = transform_screen_to_world(parallax);
        const Vec2 tl = inv.transform_point(Vec2{0.0f, 0.0f});
        const Vec2 br = inv.transform_point(Vec2{static_cast<f32>(viewport_w),
                                                 static_cast<f32>(viewport_h)});
        const f32 minx = tl.x < br.x ? tl.x : br.x;
        const f32 miny = tl.y < br.y ? tl.y : br.y;
        const f32 maxx = tl.x > br.x ? tl.x : br.x;
        const f32 maxy = tl.y > br.y ? tl.y : br.y;
        return Rect{minx, miny, maxx - minx, maxy - miny};
    }

    /// Half-extents of the visible region, in world units: the culling radius.
    Vec2 visible_half_extent(f32 parallax = 1.0f) const {
        return visible_bounds(parallax).half();
    }

    /// Frame-independent shake: `trauma` decays on its own, and the actual
    /// offset is a hash of an advancing counter so repeated runs of a replay
    /// produce byte-identical shake (design doc Section 114 — determinism).
    void add_trauma(f32 amount) {
        trauma = trauma + amount > 1.0f ? 1.0f : trauma + amount;
    }

    /// Advances shake decay. Call once per fixed step, never per render frame:
    /// tying it to the frame rate would make the shake play out at different
    /// speeds on different machines.
    void update(f32 dt) {
        if (trauma > 0.0f) {
            trauma -= dt * trauma_decay;
            if (trauma < 0.0f) trauma = 0.0f;
        }
    }

    /// Current shake offset in pixels, derived from a hash rather than an RNG.
    Vec2 shake_offset(f32 seed) const {
        if (trauma <= 0.0f) return Vec2{0.0f, 0.0f};
        const f32 amp = trauma * trauma * max_shake_pixels;
        return {hash_to_unit(seed + shake_phase) * 2.0f * amp - amp,
                hash_to_unit(seed + shake_phase + 0.5f) * 2.0f * amp - amp};
    }

    f32 trauma = 0.0f;
    f32 trauma_decay = 1.5f;   ///< Trauma lost per second.
    f32 max_shake_pixels = 6.0f;
    f32 shake_phase = 0.0f;    ///< Advances each fixed step by the caller.
};

} // namespace nf::scene2d
