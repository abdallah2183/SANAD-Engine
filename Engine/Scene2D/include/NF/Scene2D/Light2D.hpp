#pragma once

// NF/Scene2D/Light2D.hpp — 2D lights, shadow geometry and baked lighting.
// Design doc Section 54 (2D Engine): "2D lights", "Shadows".
//
// Deliberately CPU-only. Scene2D sits above ECS/Scene and has no RHI or
// Rendering dependency (Scripts/check_layering.sh), and a 2D light pipeline
// that reached into a GPU light buffer would break that layering for a single
// shader's benefit. Instead the lighting is evaluated on the CPU and folded
// into the packed RGBA8 colour every sprite already carries, so the batcher
// and the renderer stay untouched and the same lit scene renders identically
// on any backend — including a headless test.
//
// Shadow model: a point light's shadow of a straight occluder edge is exactly
// the wedge between the two rays from the light through the edge's endpoints
// (the angular interval the edge sweeps). That wedge is a convex quad once the
// endpoints are projected out to the light's radius — anything past the radius
// is unlit anyway — so a baked shadow is four orientation tests per sample
// rather than a ray cast per occluder. Baking is optional: `is_occluded` is the
// exact ray test and is what `sample` falls back to when no bake has run.
//
// Determinism (design doc Section 114): light order is insertion order,
// occluder order is the caller's, flicker is a hash-seeded sinusoid rather than
// an RNG, and no query ever allocates. Two identical update sequences bake
// identical lighting.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include "Math2D.hpp"

#include <vector>

namespace nf::scene2d {

/// One shadow-casting world-space edge. Winding does not matter — the shadow
/// wedge is defined by the rays through the endpoints, not by which side of the
/// edge is "front", so a hand-authored wall and a tile-extracted rect both work.
struct OccluderSegment {
    Vec2 a{0.0f, 0.0f};
    Vec2 b{0.0f, 0.0f};
};

/// A light's shadow of one occluder edge. Vertices are wound clockwise in
/// y-down space, so a point is inside when it lies on the non-negative side of
/// all four directed edges — one orientation test per edge, no division.
struct ShadowQuad {
    Vec2 v[4];
};

/// How intensity falls off along the radius. `Smooth` is the hermite taper
/// t*t*(3-2t): zero derivative at both ends, so a light's disc has no visible
/// seam where it meets the unlit world. `Quadratic` is the tighter 1/d^2-style
/// taper for hot pools of light; `Linear` is the flat, readable one for
/// gameplay readability (a player can eyeball the safe radius).
enum class LightFalloff2D {
    Linear,
    Smooth,
    Quadratic,
};

/// A radial or cone-shaped 2D light. Everything is in world units and degrees;
/// solvers convert internally.
struct Light2D {
    /// World-space centre the light shines from.
    Vec2 position{0.0f, 0.0f};
    /// Reach in world units. Past this radius the light contributes nothing.
    f32 radius = 8.0f;
    /// Linear RGB, may exceed 1.0 — the pack step saturates, and over-bright
    /// lights wash out to white rather than clamp mid-tint.
    Vec3 color{1.0f, 1.0f, 1.0f};
    /// Multiplier on the falloff, so one dim light and one bright light can
    /// share a radius.
    f32 intensity = 1.0f;
    LightFalloff2D falloff = LightFalloff2D::Smooth;

    /// Cone axis and full width, degrees. A width of 360 (the default) is a
    /// full radial light and skips the arc test entirely.
    f32 direction_deg = 0.0f;
    f32 spread_deg = 360.0f;

    /// Whether this light throws shadows. A non-shadow-casting light is the
    /// cheap option for ambient fills, and is also what a glowing pickup wants
    /// (it should not paint the wall behind it dark).
    bool cast_shadows = true;

    /// Flicker: the multiplier oscillates around 1 by `flicker_amount`, at
    /// `flicker_speed` cycles per second. `flicker_phase` is a hash seed — two
    /// torches side by side with different phases strobe out of step instead of
    /// pulsing in unison, and the whole thing replays bit-identically because
    /// the phase feeds a hash, never an RNG (design doc Section 114).
    f32 flicker_amount = 0.0f;
    f32 flicker_speed = 4.0f;
    f32 flicker_phase = 0.0f;

    /// Parallax layer the light belongs to. A light on a 0.5 parallax background
    /// should be queried at the background's effective position; the system
    /// that moves lights is responsible for applying it, not the light itself.
    f32 parallax = 1.0f;

    /// Full radial rather than a cone — checked once here so the hot path
    /// skips the arc test.
    bool is_cone() const { return spread_deg < 359.5f; }

    /// Fraction of the light reaching `world_point`: 1 at the light's position,
    /// 0 at or beyond the radius. Cone lights additionally zero out anything
    /// outside the arc. This is the geometry only — it does not test shadows,
    /// which is the Lighting2D's job.
    f32 attenuation(Vec2 world_point) const;

    /// Time-varying multiplier, roughly in [1-amount, 1+amount]. A light with
    /// no flicker always returns exactly 1.
    f32 flicker(f32 time) const;
};

/// Evaluates the full lighting at a world point: ambient plus every light that
/// reaches it, minus whatever is in shadow. Owns the occluder set and the
/// optional shadow bake.
class Lighting2D {
public:
    Lighting2D() = default;

    /// --- lights -----------------------------------------------------------
    /// Every light mutation clears the bake, since a baked shadow list is
    /// indexed by light order and a stale one would silently shadow the wrong
    /// light rather than crash.
    void clear_lights() {
        m_lights.clear();
        m_baked = false;
    }
    void add_light(const Light2D& light) {
        m_lights.push_back(light);
        m_baked = false;
    }
    const std::vector<Light2D>& lights() const { return m_lights; }

    /// --- occluders --------------------------------------------------------
    /// Replaces the whole occluder set. Taken by value: a frame's occluders are
    /// typically built once from the tilemap and handed over wholesale.
    void set_occluders(std::vector<OccluderSegment> segs) {
        m_occluders = std::move(segs);
        m_baked = false;
    }

    /// Convenience for the common source: the tilemap's merged collision rects,
    /// each becoming its four boundary edges. Winding is clockwise in y-down
    /// space so the edges face outward, though the shadow bake does not depend
    /// on it.
    void set_occluders_from_rects(const std::vector<Rect>& rects);

    const std::vector<OccluderSegment>& occluders() const { return m_occluders; }

    /// Light that reaches everything regardless of occlusion. Dark rooms are
    /// not black — a small ambient term keeps silhouettes readable, which is
    /// also what stops a fully shadowed sprite from vanishing into the
    /// background.
    void set_ambient(const Vec3& c) { m_ambient = c; }
    const Vec3& ambient() const { return m_ambient; }

    /// --- bake -------------------------------------------------------------
    /// Precomputes shadow quads for every shadow-casting light and stamps the
    /// flicker time. Call once per fixed step after occluders and lights move;
    /// `sample` then costs four orientation tests per occluder per light
    /// instead of a ray cast. Without a bake, `sample` falls back to the exact
    /// ray test, which is slower but equally correct — the bake is an
    /// optimisation, not a prerequisite.
    void bake(f32 time = 0.0f);

    bool is_baked() const { return m_baked; }
    usize shadow_quad_count(usize light_index) const;
    usize total_shadow_quad_count() const;

    /// --- queries ----------------------------------------------------------
    /// True when the baked shadow of light `light_index` covers the point.
    /// Returns false for a light that casts no shadows or an unbaked bake.
    bool in_shadow(usize light_index, Vec2 world_point) const;

    /// Exact, bake-free occlusion: does anything block the straight path from
    /// `from` to `to`? Used as the fallback in `sample` and as the reference
    /// implementation the bake is verified against.
    bool is_occluded(Vec2 from, Vec2 to) const;

    /// Full lighting at a world point: ambient plus every reaching light.
    /// Components may exceed 1 — saturate at pack time.
    Vec3 sample(Vec2 world_point) const;

    /// Scalar version: total white-intensity at a point, for tests and for
    /// gameplay queries (is this tile lit enough to be visible?).
    f32 sample_luminance(Vec2 world_point) const;

    usize light_count() const { return m_lights.size(); }
    usize occluder_count() const { return m_occluders.size(); }

private:
    std::vector<Light2D> m_lights;
    std::vector<OccluderSegment> m_occluders;
    std::vector<std::vector<ShadowQuad>> m_shadows;
    Vec3 m_ambient{0.08f, 0.08f, 0.11f};
    f32 m_time = 0.0f;
    bool m_baked = false;
};

/// Unpacks a packed RGBA8 colour back to linear RGB (the inverse of
/// SpriteBatcher's `pack_color_vec`) so a lighting pass can modify a sprite's
/// existing tint rather than replacing it.
Vec3 unpack_color_rgb(u32 packed);

} // namespace nf::scene2d
