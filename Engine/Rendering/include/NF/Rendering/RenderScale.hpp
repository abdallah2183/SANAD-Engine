#pragma once

// NF/Rendering/RenderScale.hpp — internal render-target scale (Phase 37).
//
// The window/panel size and the size the 3D pass actually renders at are two
// different numbers. Keeping them apart buys two things with no RHI change:
//
//   * Supersampling. A scale above 1 renders the whole deferred chain — depth,
//     GBuffer, lighting, bloom — at a higher resolution and lets the tonemap
//     pass's bilinear read average it down to the output. At exactly 2.0 each
//     output pixel averages 2x2 internal texels, which is SSAA: every geometry
//     edge is resolved, not only the high-luma ones FXAA can see.
//   * A cheap resolution knob. A scale below 1 renders fewer pixels and lets
//     the same bilinear read upscale; that is the half of dynamic resolution
//     that does not need a swapchain change.
//
// This is NOT MSAA. MSAA resolves coverage per sample inside one raster pass;
// supersampling shades every internal pixel. It costs the square of the scale
// in fragment work, which is why the ceiling is 2.0 and why the value is
// authored rather than silently automatic. MSAA remains the separate RHI item.
//
// The renderer never trusts the number it is given: `sanitize_render_scale`
// maps a missing/NaN/out-of-range value to a legal one, and both the scene
// loader and the editor use those same bounds so a value one layer accepts is
// never re-clamped by the next.

#include <NF/Core/Types.hpp>

namespace nf::rendering {

/// Smallest scale the renderer accepts. Below this the image is so coarse that
/// the bilinear upscale reads as a different scene, not a cheaper one.
inline constexpr f32 kMinRenderScale = 0.25f;
/// Largest scale the renderer accepts. 2.0 is exactly 4x the fragment work of
/// 1.0 and exactly a 2x2 box average per output pixel at that size.
inline constexpr f32 kMaxRenderScale = 2.0f;
/// Hard ceiling on either internal axis, so a nonsense window size plus a 2x
/// scale cannot overflow the arithmetic or ask for an image no device allows.
inline constexpr u32 kMaxRenderTargetDimension = 16384u;

/// Clamps a render scale into [kMinRenderScale, kMaxRenderScale]. A non-finite
/// value maps to 1.0 (the identity), not to the minimum: a corrupted number
/// must not silently turn the frame into a quarter-resolution blur.
f32 sanitize_render_scale(f32 scale);

/// The internal size for an output of `output_width` x `output_height` at
/// `scale`. Both axes use the same scale but round independently, and each is
/// clamped to at least one texel so a tiny test target still has valid
/// attachments. A zero output axis means "not sized yet" and returns 0x0.
struct RenderTargetSize {
    u32 width = 0;
    u32 height = 0;

    bool operator==(const RenderTargetSize&) const = default;
};

RenderTargetSize scaled_render_size(u32 output_width, u32 output_height, f32 scale);

} // namespace nf::rendering
