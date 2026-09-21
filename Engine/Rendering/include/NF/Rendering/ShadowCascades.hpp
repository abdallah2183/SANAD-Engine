#pragma once

// NF/Rendering/ShadowCascades.hpp — cascaded directional shadows (design §17).
//
// The shadow system used to be a single orthographic box pinned to the WORLD
// ORIGIN (a ±12 box with the light parked at `dir * -20`). Shadows therefore
// existed only within ~12 units of (0,0,0): walking the camera away from the
// origin silently deleted every shadow in the scene, and any world larger than
// 24x24 units had none at all. This module replaces that box with cascades
// fitted around the CAMERA's frustum slice, so shadows follow the viewer and
// near geometry gets a tighter, higher-resolution projection.
//
// Everything here is pure: no device, no RHI, no allocation. The math is the
// part that can be wrong in ways pixels merely hint at, so it is unit-tested
// on the CPU (Tests/RHITests/test_shadow_cascades.cpp) independently of any GPU.
//
// Storage layout (see Renderer3D): the cascades share ONE 2x2 atlas of
// `tile_size` squares. Cascade i owns the rect at (i%2, i/2). An atlas with
// viewport/scissor per tile was chosen over a `View2DArray` depth texture
// because the RHI's framebuffer path takes `Texture*` and binds
// `texture->view()` — a single-layer 2D view — so layered rendering would have
// required an RHI change for no gain at four cascades.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Rendering/Camera.hpp>

namespace nf::rendering {

/// Cascades the atlas can hold. Fixed by the 2x2 tile layout: raising it means
/// changing the atlas geometry, not just this number.
inline constexpr u32 kMaxShadowCascades = 4;

/// Tile grid per axis (2 => 2x2 = 4 tiles).
inline constexpr u32 kShadowTileGrid = 2;

/// How the cascade split distances and the per-cascade fits are derived.
struct CascadeConfig {
    /// Active cascades, clamped to [1, kMaxShadowCascades]. One cascade is the
    /// degenerate "single fitted map" case and is what a scene gets when it
    /// asks for the old look.
    u32 count = kMaxShadowCascades;

    /// Practical-split blend: 0 = uniform (every cascade the same depth span),
    /// 1 = fully logarithmic (equal depth *ratios*, which is what perspective
    /// projection actually needs). 0.75 weights the near field, where shadow
    /// detail is read, without starving the far plane.
    float lambda = 0.75f;

    /// Farthest view-space distance shadows are cast to. 0 means "the camera's
    /// far plane". Capping this below the far plane spends the whole atlas on
    /// the range that is actually readable and is the usual open-world knob.
    float max_distance = 0.0f;

    /// Fraction of a cascade's span over which the hand-off to the next cascade
    /// is cross-faded. 0 disables blending and leaves a hard seam at each split.
    float fade_range = 0.10f;

    /// Fraction of a cascade's depth span the light is pulled BACK along its
    /// own axis, so geometry standing between the light and the slice still
    /// casts into it. Without this, a tall object in front of the cascade is
    /// clipped away by the ortho near plane and stops casting.
    float caster_extrusion = 0.35f;
};

/// One cascade's projection and the slice of the camera frustum it covers.
struct CascadeFit {
    /// World -> shadow clip. Consumed exactly like `update_camera`'s matrix
    /// (row-vector order, `world * light_view_proj`), so the shader reads it
    /// the same way it reads the camera's.
    Mat4 light_view_proj = Mat4::identity();
    /// View-space distance from the eye to this cascade's near/far planes.
    float split_near = 0.0f;
    float split_far = 0.0f;
    /// Light-space depth span (far - near, both positive distances along the
    /// light's forward axis). This is what a depth bias written in NDC is
    /// measured against: the ortho projection maps this span onto [0,1], so an
    /// NDC bias buys a WORLD offset of `bias * depth_range`.
    float depth_range = 1.0f;
    /// Full width (not half) of the square ortho box actually covered, before
    /// hull rounding — divide by the tile resolution for the texel's world size.
    /// Exposed for tests and for anyone reasoning about texel density.
    float world_extent = 1.0f;
};

/// A tile's rect in atlas UV, [0,1]^2, half-texel inset applied by the caller.
struct CascadeTile {
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
};

/// Practical split scheme (Lauritzen). Fills `out_splits` with `count + 1`
/// boundaries in view-space distance: out[0] == near_z, out[count] == far_z,
/// strictly increasing in between.
///
/// `count` is clamped to [1, kMaxShadowCascades]; `lambda` to [0,1]. A
/// degenerate range (far <= near) collapses to the two boundary values rather
/// than dividing by zero.
void compute_cascade_splits(float near_z, float far_z, u32 count, float lambda,
                            float* out_splits);

/// Fits one cascade's light-space ortho box around the camera frustum slice
/// [split_near, split_far].
///
/// `light_dir` is the direction the light TRAVELS (the same convention as
/// DirectionalLight::direction and lighting.frag). `tile_size` is the square
/// tile's resolution and is what the texel snapping is quantised to — passing
/// the wrong one yields shadows that shimmer as the camera moves, which is
/// precisely the artefact the snapping exists to prevent.
///
/// Returns an identity-projection fit with depth_range 0 when the light
/// direction is degenerate (zero length), rather than producing NaNs that
/// would propagate into the uniform buffer and blank every shadow.
CascadeFit fit_cascade(const Camera& camera, const Vec3& light_dir, float split_near,
                       float split_far, u32 tile_size, float caster_extrusion = 0.35f);

/// The atlas rect owned by cascade `index`.
CascadeTile cascade_tile(u32 index);

/// UV scale of one tile (1/kShadowTileGrid). Shader-side companion to
/// cascade_tile.
constexpr float cascade_tile_scale() {
    return 1.0f / static_cast<float>(kShadowTileGrid);
}

/// Index of the cascade covering `view_depth` (a distance along the camera's
/// forward axis), given `count + 1` split boundaries. Clamped: anything nearer
/// than splits[0] or farther than splits[count] still maps into range, so a
/// caller can never index a cascade that was not rendered.
u32 select_cascade(float view_depth, const float* splits, u32 count);

/// Tangent of the steepest surface-to-light angle the automatic bias is sized
/// for. The bias a flat surface needs across one texel is `texel * tan(theta)`,
/// and tan(theta) is unbounded as the light grazes a surface, so this is a
/// deliberately finite stand-in for "steep enough to matter" rather than a
/// derived constant. 2.0 covers surfaces up to ~63 degrees off the light.
inline constexpr float kShadowBiasSlope = 2.0f;

/// Smallest NDC depth bias that keeps this cascade from self-shadowing.
///
/// A shadow map stores ONE depth per texel, so a flat surface crossing that
/// texel can sit `texel_world * tan(theta)` closer to the light at one edge than
/// the depth recorded at its centre. Every texel whose own surface beats the map
/// by more than the bias self-shadows, and a whole face failing at once is what
/// acne looks like on screen. `texel_world` is `world_extent / tile_size`, and
/// `depth_range` converts that world offset into the NDC units the shader
/// compares in.
///
/// This is the reason the bias cannot be one tuned constant. A cascade fitted to
/// the near slice has smaller texels than a fixed world box AND a shorter depth
/// range than that box, and the two do not cancel: the texel shrinks ~1.5x while
/// the range shrinks ~6x, so the same constant ends up several times too small.
/// Deriving it per cascade keeps each cascade at the minimum that actually
/// works, and lets the artist's constant travel on top as pure extra.
///
/// Degenerate inputs return 0: no bias, rather than a NaN in the uniform buffer.
float cascade_auto_bias(const CascadeFit& fit, u32 tile_size,
                        float slope = kShadowBiasSlope);

/// Clamps a config into what the renderer and the atlas can actually honour.
CascadeConfig sanitize_cascade_config(const CascadeConfig& config);

} // namespace nf::rendering
