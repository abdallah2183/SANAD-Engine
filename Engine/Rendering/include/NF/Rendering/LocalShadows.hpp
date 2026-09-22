#pragma once

// NF/Rendering/LocalShadows.hpp — point and spot light shadows (Phase 21).
//
// The directional path (ShadowCascades) fits one ortho box per slice of the
// camera frustum. Local lights cannot: a point light radiates in every
// direction at once and a spot light radiates through a cone, so both need a
// *perspective* projector placed AT the light, not an ortho box parked beside
// one. This module supplies that projector and the conventions the shader
// shares with it.
//
// Faces, not layered rendering
// ----------------------------
// A point light needs six 90-degree frusta to see all the way around. The
// obvious shape is a cubemap, but the renderer's framebuffer binds a
// single-layer 2D view (see ShadowCascades.hpp — the same constraint that put
// the directional cascade in a 2x2 atlas), so the six faces are laid out as
// tiles of one flat 2D atlas instead and the shader reconstructs the face
// index from the direction it wants to test. Six tiles in a 3x2 strip; a spot
// light costs one tile, appended after them.
//
// Everything here is pure: no device, no RHI, no allocation. The part that can
// be wrong — the handedness of each face, the reconstruction rule, the cone
// fit — is exactly the part that only shows up as "the shadow is on the wrong
// side", so it is unit-tested on the CPU (Tests/RHITests/test_local_shadows.cpp)
// the same way the cascade math is.
//
// Convention
// ----------
// `direction` is the way the light TRAVELS, matching SpotLight::direction and
// lighting.frag. Distances are positive along that axis. The projector looks
// down its own -Z with the engine's right-handed, Y-flipped Vulkan projection
// (see Mat4::perspective), so a point exactly on the light's axis lands at
// NDC (0, 0) and the near plane is 0, never negative.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <array>

namespace nf::rendering {

/// Faces of a point light's surroundings, in atlas order. One point light
/// occupies six consecutive tiles; a spot light occupies one.
enum class CubeFace : u32 {
    PosX = 0,
    NegX = 1,
    PosY = 2,
    NegY = 3,
    PosZ = 4,
    NegZ = 5,
};

inline constexpr u32 kCubeFaceCount = 6;

/// How many point/spot lights may cast shadows in one frame. Each point light
/// costs six depth renders and each spot one, so this is a frame-budget cap,
/// not the light cap (kMaxPointLights): lights past it still light the scene,
/// they just cast no shadow. Kept separate so raising the light limit does not
/// silently sextuple the shadow pass.
inline constexpr u32 kMaxShadowPointLights = 4;
inline constexpr u32 kMaxShadowSpotLights = 4;

/// Tiles in the local shadow atlas: six per shadowed point light, one per
/// shadowed spot light, laid out as one row of `kLocalShadowTileGrid`-sized
/// squares. A point light i owns [i*6, i*6+6); a spot light j owns
/// `kMaxShadowPointLights*6 + j`.
inline constexpr u32 kLocalShadowTileCount =
    kMaxShadowPointLights * kCubeFaceCount + kMaxShadowSpotLights;

/// Tiles per atlas row. Chosen as the smallest value whose product covers the
/// tile count while keeping every row full: a partial final row would still
/// allocate the full texture width, and an empty row is wasted memory the
/// shadow pass pays for every frame.
inline constexpr u32 kLocalShadowTileGrid = 7;

/// Total atlas resolution is `tile_size * grid`. Must cover the tile count.
static_assert(kLocalShadowTileGrid * kLocalShadowTileGrid >= kLocalShadowTileCount,
              "local shadow atlas must be able to hold every tile");

/// Atlas tile owned by point light `light`'s `face`, or by spot light `light`.
inline constexpr u32 point_shadow_tile(u32 light, CubeFace face) {
    return light * kCubeFaceCount + static_cast<u32>(face);
}
inline constexpr u32 spot_shadow_tile(u32 light) {
    return kMaxShadowPointLights * kCubeFaceCount + light;
}

/// One face's perspective projector and the depth it looks down.
struct LocalShadowFit {
    /// World -> shadow clip. Consumed like every other matrix in the engine
    /// (row-vector order, `world * view_proj`).
    Mat4 view_proj = Mat4::identity();
    /// Positive near/far along the light's forward axis. The far plane doubles
    /// as the light's reach: nothing beyond `far` is lit by this light anyway,
    /// so spending depth precision on it only blurs the shadow.
    float near_z = 0.05f;
    float far_z = 25.0f;
};

/// Builds a 90-degree projector for one face of a point light at `position`.
///
/// Each face looks along an axis with an `up` chosen so no face degenerates
/// when the others are fine: the Y faces use +Z as up (never parallel to their
/// own axis), the X and Z faces use +Y. A zero `far` yields an identity
/// projector rather than a NaN that would blank the whole atlas.
LocalShadowFit fit_point_face(const Vec3& position, CubeFace face,
                              float near_z = 0.05f, float far_z = 25.0f);

/// Builds a projector for a spot light at `position` aimed along `direction`,
/// its horizontal field of view set to cover `outer_angle_rad` on both sides.
///
/// The cone is a solid angle, and a square frustum covers it with the least
/// wasted resolution when the fov is twice the half-angle: the frustum's edge
/// ray grazes the cone's rim exactly. `outer_angle_rad` is clamped to (0, 90)
/// degrees — a 90-degree half-angle is a hemisphere and a wider one sees
/// behind itself, which no spot light can do.
LocalShadowFit fit_spot(const Vec3& position, const Vec3& direction,
                        float outer_angle_rad, float near_z = 0.05f,
                        float far_z = 25.0f);

/// The world-space axis one face's projector looks along (the direction the
/// light TRAVELS through that face). The shader's reconstruction rule and this
/// must agree by construction, so both live here.
Vec3 face_forward(CubeFace face);

/// Which face a ray FROM the light TO a point falls in, and that point's NDC
/// position within the face. Returns false when the point is behind the
/// face's plane or outside its 90-degree pyramid, so a caller can treat "not
/// in this face" as "not occluded by it" rather than sampling a neighbour.
///
/// `dir` need not be normalised; it must merely be the vector from the light
/// to the point being tested.
bool project_into_face(const Vec3& dir, CubeFace face, Vec2& out_ndc);

/// Picks the face `dir` falls in (the one whose axis it is most aligned with)
/// and reports its NDC. Returns false when `dir` is the zero vector, which
/// occurs only for a point exactly on the light and has no answer.
bool select_face(const Vec3& dir, CubeFace& out_face, Vec2& out_ndc);

/// Tangent of the steepest surface-to-light angle the automatic bias is sized
/// for; the companion of ShadowCascades' kShadowBiasSlope, kept separate so
/// tuning one light family does not move the other.
inline constexpr float kLocalShadowBiasSlope = 2.0f;

/// Smallest NDC depth bias that keeps this projector from self-shadowing.
///
/// Same reasoning as cascade_auto_bias: one depth per texel, a surface
/// crossing it sits `texel_world * tan(theta)` closer at one edge, and the
/// derived minimum is what stops a whole lit face from shading itself. The
/// artist's bias is added on top in the shader. `texel_world` is the world
/// size of one shadow texel and `slope` bounds the surface-to-light angle
/// (kLocalShadowBiasSlope). Degenerate fits return 0 rather than a NaN.
float local_shadow_auto_bias(const LocalShadowFit& fit, u32 tile_size,
                             float slope = kLocalShadowBiasSlope);

/// Near plane every local projector uses. Chosen once, here, so the CPU plan
/// and any caller that wants to reason about a fit's depth range agree.
inline constexpr float kLocalShadowNear = 0.05f;

/// Far plane a spot light's projector opens to when the artist did not cap it:
/// the distance lighting.frag windows the cone's falloff by, so beyond it the
/// light brightens nothing and depth precision spent there only blurs the
/// shadow.
inline constexpr float kLocalShadowSpotDefaultFar = 25.0f;

/// One atlas tile's computed upload. `tiles` in LocalShadowPlan is indexed the
/// same way the uniform array is — by TILE, not by light — so a point light's
/// six faces upload as six consecutive entries exactly where the shader looks
/// them up. Unused tiles stay at their zero-initialised defaults: `enabled` 0,
/// which the shader treats as "nothing occludes", and an identity matrix, which
/// is finite rather than the NaN a degenerate projector would emit.
struct LocalShadowTile {
    Mat4  view_proj   = Mat4::identity();
    float enabled     = 0.0f; // 0/1, compared as > 0.5 in the shader
    float strength    = 1.0f;
    float auto_bias   = 0.0f; // derived from this tile's texel size and depth span
    float artist_bias = 0.0f; // the light's own shadow_bias, added on top
};

/// The whole atlas's worth of projectors for one frame, ready to memcpy into
/// the frame uniform buffer.
struct LocalShadowPlan {
    std::array<LocalShadowTile, kLocalShadowTileCount> tiles{};
    /// Lights that actually received slots. Diagnostic, not a limit: the
    /// assignment is by index, so a gap between these and the light count is
    /// itself the signal that a light opted out or hit the shadow cap.
    u32 point_shadow_lights = 0;
    u32 spot_shadow_lights  = 0;
};

/// Assigns every shadow-casting local light its atlas tiles and builds their
/// projectors — the pure, device-free half of the local shadow pass.
///
/// Tile assignment is BY LIGHT INDEX, not by "the i-th light that wants a
/// shadow": tile `i*6 + face` belongs to point light `i` and tile
/// `spot_shadow_tile(i)` to spot light `i`, so the shader's per-light loop can
/// find a light's tiles from its loop index alone. Assigning slots in
/// opt-in order instead would slide every tile down when one light disables
/// shadows, and the shader would sample light B's projector for light A —
/// silently, since a depth value still comes back. Lights past
/// kMaxShadowPointLights/kMaxShadowSpotLights simply get no tiles whatever they
/// ask for; the cap is a frame-budget knob, not a light cap.
///
/// `PointLights`/`SpotLights` are any ranges whose elements expose the fields
/// PointLight/SpotLight do (`shadows_enabled`, `position`, `radius` or
/// `direction`/`outer_angle_rad`, and the four `shadow_*` knobs). Duck-typed
/// through a template so this header never depends on Renderer3D, which
/// depends on it — and so a test can plan against its own light structs.
template <typename PointLights, typename SpotLights>
LocalShadowPlan plan_local_shadows(const PointLights& points, const SpotLights& spots,
                                   u32 tile_size);

// ---------------------------------------------------------------------------
// Implementation (header-only: it is a template, and its whole point is that
// the assignment logic is visible where the upload is).
// ---------------------------------------------------------------------------

template <typename PointLights, typename SpotLights>
LocalShadowPlan plan_local_shadows(const PointLights& points, const SpotLights& spots,
                                   u32 tile_size) {
    LocalShadowPlan plan;

    // Point lights: six tiles each, one per face. Only the first
    // kMaxShadowPointLights can have any — see the cap's rationale above.
    for (u32 i = 0; i < points.size() && i < kMaxShadowPointLights; ++i) {
        const auto& light = points[i];
        if (!light.shadows_enabled) continue;

        // The projector's far plane is the light's reach. An explicit
        // shadow_distance wins; otherwise the radius, beyond which the falloff
        // windows the light out entirely. A reach at or inside the near plane
        // is a broken light, and the tile stays disabled rather than emitting
        // an identity projector flagged as enabled — the shader would then
        // occlude everything lit by that light.
        const float far_z = (light.shadow_distance > 0.0f) ? light.shadow_distance
                                                           : light.radius;
        if (!(far_z > kLocalShadowNear)) continue;

        for (u32 f = 0; f < kCubeFaceCount; ++f) {
            const CubeFace face = static_cast<CubeFace>(f);
            const LocalShadowFit fit =
                fit_point_face(light.position, face, kLocalShadowNear, far_z);
            const u32 tile = point_shadow_tile(i, face);
            plan.tiles[tile].view_proj   = fit.view_proj;
            plan.tiles[tile].enabled     = 1.0f;
            plan.tiles[tile].strength    = light.shadow_strength;
            plan.tiles[tile].artist_bias = light.shadow_bias;
            plan.tiles[tile].auto_bias   = local_shadow_auto_bias(fit, tile_size);
        }
        ++plan.point_shadow_lights;
    }

    // Spot lights: one tile each, appended after the point lights' six each.
    // A zero aim yields an identity projector from fit_spot, so it is rejected
    // here by length rather than detected after the fact.
    for (u32 i = 0; i < spots.size() && i < kMaxShadowSpotLights; ++i) {
        const auto& light = spots[i];
        if (!light.shadows_enabled) continue;
        if (light.direction.length_sq() <= 1e-12f) continue;

        // The projector's far plane is the light's own reach — beyond it the
        // falloff windows the light out, so depth precision spent there buys a
        // blurrier shadow. An explicit shadow_distance wins for the usual
        // "crisp shadow, soft pool of light" authoring; otherwise SpotLight's
        // range, which is the same value lighting.frag windows by, so the two
        // cannot disagree about where this light ends. A reach at or inside the
        // near plane is a broken light, and the tile stays disabled rather than
        // emitting an identity projector flagged as enabled — the shader would
        // then occlude everything lit by that light.
        const float far_z = (light.shadow_distance > 0.0f) ? light.shadow_distance
                                                           : light.range;
        if (!(far_z > kLocalShadowNear)) continue;

        const LocalShadowFit fit =
            fit_spot(light.position, light.direction, light.outer_angle_rad,
                     kLocalShadowNear, far_z);
        const u32 tile = spot_shadow_tile(i);
        plan.tiles[tile].view_proj   = fit.view_proj;
        plan.tiles[tile].enabled     = 1.0f;
        plan.tiles[tile].strength    = light.shadow_strength;
        plan.tiles[tile].artist_bias = light.shadow_bias;
        plan.tiles[tile].auto_bias   = local_shadow_auto_bias(fit, tile_size);
        ++plan.spot_shadow_lights;
    }

    return plan;
}

} // namespace nf::rendering
