#pragma once

// NF/Rendering/Decals.hpp — CPU contract for deferred screen-space decals
// (Phase 36).
//
// A decal is an oriented box in world space whose +Z face is the projection
// direction: the decal "points at" the surface it was placed on. The authored
// size is baked into `world`, so a decal's local box is ALWAYS the unit cube
// [-0.5, 0.5]^3 — the fragment shader only has to test
// `abs(local.xyz) <= 0.5` after transforming the reconstructed world position
// by `inverse_world`. Local +X/+Y map to albedo UV (see decal.frag).
//
// The CPU side exists for three things the GPU must not do per pixel when a
// cheaper answer is exact:
//   * inverse-world: one 4x4 inversion per decal per frame, not per pixel.
//   * screen-rect projection: scissor each fullscreen-triangle draw to the
//     decal's projected bounds, so an off-screen decal records zero visible
//     fragments (and the pass can skip its draw entirely).
//   * far-to-near ordering: decal blending is straight alpha-over, so the
//     nearest decal must draw last. The sort is deterministic (stable, with an
//     id tie-break) because a frame compared byte-for-byte across runs must not
//     depend on the unstable order of equal distances.
//
// Nothing here touches the RHI: this header is API for tests, the runtime
// extraction step, and any renderer backend.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Rendering/Handles.hpp>

#include <span>
#include <vector>

namespace nf::rendering {

/// One decal ready for the renderer. `world` carries translation, rotation and
/// the authored XYZ size; `inverse_world` is cached beside it. `material` is
/// the decal's albedo/PBR source and must resolve to a MaterialLibrary entry.
struct DecalInstance {
    u32 id = 0;                                // owning entity id (diagnostics)
    Mat4 world = Mat4::identity();             // decal local -> world, size baked in
    Mat4 inverse_world = Mat4::identity();     // cached inverse of `world`
    MaterialHandle material{};                 // invalid = renderer fallback
    float opacity = 1.0f;                      // 0..1 multiplies material alpha
    bool enabled = true;                       // false = never submitted
};

/// A decal's projected screen bounds, in framebuffer pixels. `visible == false`
/// means the decal cannot touch this frame (behind the camera, outside the
/// depth range, or off screen) and needs no draw at all.
struct DecalScreenRect {
    i32 x = 0;
    i32 y = 0;
    i32 width = 0;
    i32 height = 0;
    bool visible = false;
};

/// Composes a decal's world matrix from an entity transform and its authored
/// size. Row-vector convention (see Core/Math.hpp): scale applies FIRST, so the
/// unit cube becomes `size` large before the entity's rotation/translation.
/// This is the one place the "size is baked into world" contract is expressed.
Mat4 decal_world_matrix(const Mat4& entity_world, const Vec3& size);

/// Builds an instance and computes its inverse. Prefer this over filling the
/// struct by hand: a stale `inverse_world` silently projects the decal from the
/// wrong place, which no downstream check can detect.
DecalInstance make_decal(u32 id, const Mat4& world, MaterialHandle material,
                         float opacity = 1.0f, bool enabled = true);

/// Recomputes `inverse_world` from `world`. Call after editing `world`.
void update_decal_inverse(DecalInstance& decal);

/// Projects the decal's eight box corners through `view_proj` and returns the
/// pixel-space scissor rect. Conservative by construction: when a corner is
/// behind the near plane (w <= 0) the rect expands to the whole viewport rather
/// than guessing, because clipping a partially-behind box analytically is the
/// shader's job and a wrong scissor here removes visible pixels.
DecalScreenRect decal_screen_rect(const DecalInstance& decal, const Mat4& view_proj,
                                  u32 width, u32 height);

/// Orders decals far-to-near for alpha-over blending. Disabled entries take
/// part in the ordering too — the renderer skips them — so toggling one never
/// changes the relative order of the enabled entries. Deterministic: equal
/// distances fall back to ascending id.
void sort_decals_far_to_near(std::span<DecalInstance> decals, const Vec3& camera_position);

} // namespace nf::rendering
