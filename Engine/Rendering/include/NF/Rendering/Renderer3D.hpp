#pragma once

// NF/Rendering/Renderer3D.hpp — the integrated Basic 3D pipeline
//
//   RenderWorld → Frustum Culling → Depth Prepass → GBuffer → PBR Lighting
//                                                    → HDR → Exposure → Tonemap → RHI
//
// A purely deferred pipeline holds ONE surface per pixel, so it cannot draw a
// transparent surface: blending into the gbuffer would average the material
// parameters of two surfaces and the lighting pass would light a surface that
// never existed. Surfaces with a base colour alpha below 1 are therefore drawn
// in a forward Transparency pass after Lighting, over the lit HDR image:
//
//   ... → GBuffer → Lighting → Transparency → Tonemap → ...
//
// Everything below runs through the RenderGraph with generic, backend-neutral
// resource states; the Vulkan backend alone turns transitions into layouts/
// stages/access masks. The renderer owns its pass resources (depth target,
// gbuffer attachments, HDR target are RenderGraph-owned textures) and rebuilds
// the per-frame pass list on every render() call.
//
// Contract:
//   - render(cmd, ..., frame_slot) must only be called when THAT slot's
//     previous GPU work has completed (the caller waits the slot's fence) —
//     it resets the slot's per-frame descriptor allocator and rewrites the
//     slot's frame-uniform buffer. Two slots exist so the GPU can still be
//     executing the previous frame while the CPU records the next one; a
//     caller that serialises (tests, tools) can keep using slot 0 forever.
//   - After render() the command buffer is ended but NOT submitted; the
//     caller submits (and presents, when rendering to a swapchain image).
//   - Lights/exposure are renderer state; meshes/materials come from the
//     libraries; the game world is consumed only through RenderWorld.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Rendering/Material.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/PipelineCache.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/LocalShadows.hpp>
#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/ShadowCascades.hpp>
#include <NF/Rendering/Sky.hpp>
#include <NF/Rendering/SkyEnv.hpp>

#include <cstddef>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace nf::rendering {

// --- Light types (renderer-side for this milestone; ECS extraction later) ---

struct DirectionalLight {
    Vec3 direction{0.0f, -1.0f, 0.0f}; // direction the light travels
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool enabled = true;
    // Shadow mapping (Phase 13): depth-tested PCF against a shadow atlas.
    // Strength scales the occlusion 0..1 (1 = full dark).
    bool shadows_enabled = true;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0005f;
    // Cascades fitted around the camera frustum, clamped to
    // [1, kMaxShadowCascades]. 1 is the degenerate single fitted map and is
    // what a scene gets when it wants the cheapest shadows; the default spends
    // the same atlas memory on four ranges that follow the viewer. See
    // ShadowCascades.hpp for why the box is no longer pinned to the origin.
    u32 shadow_cascades = kMaxShadowCascades;
    // Farthest view-space distance shadows are cast to. 0 means the camera's
    // far plane. Capping below the far plane concentrates the atlas on the
    // range that is actually readable — the usual open-world knob.
    float shadow_distance = 0.0f;
};

struct PointLight {
    Vec3 position{0.0f, 2.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 10.0f;
    // Phase 21: shadows from this light. Off by default — a point light costs
    // SIX depth renders, so the game opts in per light rather than paying for
    // every torch in the scene. Lights past kMaxShadowPointLights are lit but
    // unshadowed whatever this says, and shadow_distance caps the projector's
    // far plane (0 = the light's radius).
    bool shadows_enabled = false;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0005f;
    float shadow_distance = 0.0f;
};

struct SpotLight {
    Vec3 position{0.0f, 3.0f, 0.0f};
    Vec3 direction{0.0f, -1.0f, 0.0f}; // direction the light travels
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 2.0f;
    float inner_angle_rad = 0.35f;
    float outer_angle_rad = 0.6f;
    // The cone's reach. This was a hard-coded 25.0 in lighting.frag with a
    // matching kLocalShadowSpotDefaultFar on the CPU — two literals that had to
    // agree and nothing tied together — so a spot could not be given a short
    // reach even in a closet-sized room, and its shadow projector spent depth
    // precision out to 25 units regardless. The default IS that old 25 so every
    // existing scene lights and shadows bit for bit as before; the shader reads
    // it back through SpotGPU::outer_range.y and the shadow projector through
    // the same fallback below.
    float range = kLocalShadowSpotDefaultFar;
    // Phase 21: one depth render per light, so this is cheap to leave on.
    // shadow_distance 0 means `range`, i.e. as far as this light can brighten
    // anything; spending depth precision further out only blurs the shadow.
    bool shadows_enabled = false;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0005f;
    float shadow_distance = 0.0f;
};

// The tone-mapping operator applied to HDR linear colour *before* gamma. It is
// a sibling of exposure, not part of PostFxParams: those run after gamma, this
// one does not. The whole block still reaches tonemap.frag as one float[4]
// push constant (exposure, vignette, saturation, mode), so the mode rides the
// same channel; the shader compares it by range, not equality.
enum class TonemapMode : int {
    // 1 - exp(-x). The shipped default and the *only* mode the golden pixels
    // were authored against; keep it at 0 so an unset block reproduces the old
    // frame bit for bit.
    Exponential = 0,
    ACES       = 1, // Narkowicz's fitted filmic curve.
    Reinhard   = 2, // x / (1 + x), the classic photographic operator.
    Linear     = 3, // exposure only; for diagnostics and HDR capture.
};

// --- Bloom (design §206) -------------------------------------------------
//
// A real multi-pass chain, not a single-shader glow: the HDR image is
// thresholded into a half-resolution level, that level is downsampled with a
// 13-tap box filter into three more, and the tonemap pass sums all four with
// bilinear taps — a bilinear fetch of a smaller level IS a 2x2 tent filter, so
// the "upsample" costs no passes at all. The alternative (an explicit
// upsample-and-add pass per level) needs additive blending, and the RHI's
// blend state lives on the render-pass attachment rather than the pipeline;
// spending four more passes to avoid one sampler read per level is the wrong
// trade.
//
// `enabled` is the only switch that costs anything: with it false the renderer
// records no bloom pass at all, and the tonemap shader is compiled so that a
// zero intensity never even reads the bloom bindings.
struct BloomParams {
    bool enabled = false;
    // Luminance above which a pixel contributes to the glow. Below it a pixel
    // contributes nothing, so a normal-lit scene (peak luminance around 1-2
    // with the default exposure) does not bloom its whole surface.
    float threshold = 1.0f;
    // Width of the soft knee, in the same luminance units. 0 is a hard cut,
    // which makes the bloom's edge pop as the camera moves; a small knee
    // (0.2-0.5) fades the contribution in and is what the default ships with.
    float knee = 0.5f;
    // Multiplier applied to the summed levels before it is added back. 0 is
    // exactly "no bloom" regardless of `enabled`, which is what lets a test
    // pin "the sum is added, not assigned".
    float intensity = 1.0f;
    // Blur spread, as a multiplier on the downsample kernel's tap spacing.
    // 1 is the reference kernel; below 1 tightens the glow toward a
    // highlight-only halo, above 1 widens it.
    float radius = 1.0f;
};

// --- Colour grading (design §206) ----------------------------------------
//
// Applied in HDR *before* the tonemap operator, which is where the design
// document puts grading. That ordering is deliberate and worth stating: a
// contrast operation performed after the tonemap is a curve applied to an
// already-compressed image, so raising it clips highlights that the operator
// had carefully rolled off; in HDR it is a scene-luminance remap and the
// tonemap still gets to do its job on the result.
struct ColorGradeParams {
    bool enabled = false;
    // Scale about `pivot`. 1 is neutral. Applied per channel, so a saturated
    // pixel can clip one channel before another — that is the point of a
    // contrast control and not a bug to smooth away.
    float contrast = 1.0f;
    // The luminance the contrast scale leaves untouched. In HDR units, so 1.0
    // is "as bright as a fully lit white surface", not mid-grey.
    float pivot = 1.0f;
    // Warm/cool balance, -1 (blue) .. +1 (orange). Implemented as a red/blue
    // gain pair, not as a hue rotation: a photographer's white balance moves
    // the two ends of the visible spectrum against each other.
    float temperature = 0.0f;
    // Green/magenta balance, -1 (green) .. +1 (magenta). Green is moved
    // against the red/blue average, which is the axis a white balance leaves
    // uncorrected.
    float tint = 0.0f;
    // Per-channel power applied last. 1 is neutral. >1 darkens midtones,
    // <1 lifts them — a gamma control, distinct from contrast's linear scale.
    float gamma = 1.0f;
};

// --- Sharpening (design §206) --------------------------------------------
//
// An unsharp mask on the linear HDR image: the tonemap pass takes a
// four-neighbour average and adds the difference back. It runs on the HDR
// signal rather than the tonemapped one because a sharpen applied after the
// tonemap amplifies the operator's own compression artefacts (visible as
// halos around bright edges) instead of the scene's detail.
struct SharpenParams {
    bool enabled = false;
    // Strength of the added difference. 0 is exactly "off" regardless of
    // `enabled`, so a disabled stage and a zero-strength one are the same
    // image.
    float amount = 0.0f;
    // Neighbour offset as a multiplier on one output texel. 1 samples the
    // adjacent texel (a tight sharpen); larger values reach further and are
    // the usual cure for a sharpen that only touches single-pixel noise.
    float radius = 1.0f;
};

/// Lens effects (§206): the two UV-space warps a real lens imposes on the
/// image before it reaches the sensor.
///
/// Both are read from the SAME place, which is why they are one stage rather
/// than two: the HDR is fetched at a warped coordinate, and chromatic
/// aberration is the same warp applied per channel with a slightly different
/// strength. Neither filters anything, so neither needs a neighbourhood — which
/// is also what makes both exactly mirrorable on the CPU (see lens_sample_uv).
///
/// Every value's neutral is EXACTLY identity: with both at zero the fetch is
/// `texture(hdr, uv)` and the frame is bit-for-bit what it was before the stage
/// existed. The golden pixels depend on that.
struct LensParams {
    bool enabled = false;
    /// Radial barrel/pincushion distortion, as a coefficient on r² (r measured
    /// from the centre of the frame in half-extent units). Positive pulls the
    /// edges OUT (barrel — the classic wide-angle bow), negative pinches them
    /// in (pincushion). 0 is no distortion.
    ///
    /// Applied to the FETCH coordinate, so the image moves opposite the
    /// coefficient's name at the edges — the usual convention, and the one that
    /// keeps 0 meaning "nothing happens".
    float distortion = 0.0f;
    /// Chromatic aberration: red is fetched slightly further out than blue, so
    /// a high-contrast edge splits into a warm/cool fringe. Expressed as a
    /// fraction of the radial distance, so it scales with the frame and a value
    /// of 0.01 is a subtle fringe. 0 is none. Green is never displaced, so a
    /// neutral grey pixel stays neutral.
    float chromatic_aberration = 0.0f;
};

// The whole §206 post stack, minus the stages that are separate renderer state
// by history: exposure and the tonemap mode are their own setters because they
// predate this struct and the golden pixels are pinned to their defaults.
//
// Every stage is neutral by default and every neutral value is EXACTLY
// identity, so a renderer that never touches this renders the same frame it
// rendered before the stack existed. That is a hard contract, not a
// convention: `Tests/RHITests` pins it by comparing a default-constructed
// params block against a hand-computed identity.
//
// `saturation` and `vignette` stay first so the aggregate initialisation
// `PostFxParams{1.15f, 0.28f}` that samples and tests already use keeps
// meaning what it meant.
/// Depth of field (§206).
///
/// A linear circle-of-confusion ramp rather than a thin-lens model: the two
/// things an author actually wants to set are WHERE the image is sharp and HOW
/// WIDE the sharp band is, and a physical model would replace both with an
/// aperture and a focal length that mean the same thing to nobody who is not a
/// photographer.
struct DofParams {
    bool enabled = false;
    /// World-space distance from the camera that stays in focus.
    float focus_distance = 10.0f;
    /// The distance over which the blur ramps from zero (at `focus_distance`)
    /// to the full radius. Larger = more of the scene reads as sharp. At or
    /// below zero the ramp would divide by zero, which is refused rather than
    /// silently treated as "everything is sharp".
    float focus_range = 2.0f;
    /// Blur radius in output texels at full confusion. **0 is exactly "off"**,
    /// and that is how the renderer disables the stage: `enabled` is folded
    /// into this one value at push time, the way `bloom_on` folds the intensity.
    /// The shader has no separate flag, so an off stage costs one compare.
    float max_radius = 6.0f;
};

/// Tap count of the depth-of-field gather. Must match tonemap.frag's kDofTaps.
inline constexpr int kDofTaps = 12;

/// Motion blur (§206), from depth reprojection.
///
/// The velocity comes from reprojecting each pixel's world position through the
/// PREVIOUS frame's view-projection, so this needs no velocity buffer and no
/// extra gbuffer attachment — the depth target the depth-of-field stage already
/// reads carries everything required.
///
/// It is therefore CAMERA motion blur: a surface's own movement is not in the
/// depth buffer, so a fast object under a still camera does not smear.
/// Per-object motion needs a velocity attachment the deferred path does not
/// write, which is a renderer change rather than a post stage.
struct MotionBlurParams {
    bool enabled = false;
    /// Multiplier on the reprojected velocity. **0 is exactly "off"**, and that
    /// is how the renderer disables the stage: `enabled` is folded into it at
    /// push time.
    float intensity = 1.0f;
    /// Cap on the smear, in uv units, so a camera that teleports smears by a
    /// bounded amount instead of stretching one frame across the screen.
    float max_length = 0.05f;
};

/// Tap count of the motion-blur smear. Must match tonemap.frag's kMotionTaps.
inline constexpr int kMotionTaps = 8;

/// Edge length of the colour-grading LUT: a `kLutSize^3` cube stored as a 2D
/// strip `kLutSize` slices wide and `kLutSize` texels tall (`kLutSize*kLutSize`
/// x `kLutSize`). Must match tonemap.frag's kLutSize.
///
/// A strip rather than a 3D texture because the RHI's sampled-image views are
/// 2D; the shader does the slice blend itself, which costs one extra fetch and
/// keeps the whole stage inside the texture format the rest of the engine
/// already uploads.
inline constexpr u32 kLutSize = 16;

struct PostFxParams {
    float saturation = 1.0f; // 0 = grayscale, 1 = neutral, >1 = vivid
    float vignette = 0.0f;   // 0 = off .. 1 = strong corner darkening
    /// How much of the colour-grading LUT to apply, 0..1. **0 is exactly "off"**:
    /// the shader never reads the LUT texture, so a scene with no LUT — and a
    /// renderer with no LUT bound — is bit-for-bit the frame it was before this
    /// stage existed. The LUT texture itself arrives through
    /// `set_color_lut()`; this is only the strength.
    float lut_strength = 0.0f;
    BloomParams bloom{};
    ColorGradeParams grade{};
    SharpenParams sharpen{};
    LensParams lens{};
    DofParams dof{};
    MotionBlurParams motion{};
};

/// CPU mirror of the tonemap operator (pre-gamma). Matches tonemap.frag
/// exactly, including the published ACES constants — do not simplify those,
/// the shader spells the same polynomial.
Vec3 tonemap(Vec3 hdr, float exposure, TonemapMode mode);

/// CPU mirror of the tonemap tail (saturation + vignette on LDR color).
/// Matches tonemap.frag exactly; uv is the fullscreen uv in [0, 1].
Vec3 apply_postfx(Vec3 color, Vec2 uv, const PostFxParams& params);

/// CPU mirror of bloom.frag's prefilter branch: the soft-knee threshold that
/// decides how much of an HDR pixel survives into the bloom chain.
///
/// The knee is what stops the glow from switching on per-pixel as a surface
/// crosses the threshold — a hard cut makes a moving light's bloom crawl. The
/// formula is the standard soft-knee one: below `threshold - knee` nothing
/// passes, above `threshold + knee` everything above the threshold passes, and
/// in between the contribution is a quadratic ramp. `knee` at or below zero is
/// the hard cut, and a non-positive `threshold` passes everything (useful for
/// a diagnostic that wants to see the chain's own response).
Vec3 bloom_prefilter(Vec3 hdr, float threshold, float knee);

/// CPU mirror of bloom.frag's downsample branch, for a 4x4 neighbourhood read
/// around `uv` from an image of `src_w` x `src_h` texels. `radius` multiplies
/// the tap spacing, matching the shader's own parameter.
///
/// The image is passed as a flat span in row-major order so this is testable
/// without a GPU; the sampler's bilinear filtering is approximated by clamping
/// to the nearest texel, which is what the tests compare against (the shader
/// samples with a linear filter, so a test that asserts equality with a
/// non-trivial image would be pinning the filtering mode, not the kernel).
Vec3 bloom_downsample(std::span<const Vec3> src, u32 src_w, u32 src_h,
                      Vec2 uv, float radius);

/// CPU mirror of the unsharp mask the tonemap pass applies before the bloom
/// add. `blurred` is the four-neighbour average the shader computes.
Vec3 unsharp_hdr(Vec3 center, Vec3 blurred, float amount);

/// CPU mirror of the colour grade. Matches tonemap.frag's grade branch.
Vec3 apply_color_grade(Vec3 hdr, const ColorGradeParams& params);

/// CPU mirror of the colour-grading LUT stage. `rgba` is the `kLutSize^3` cube
/// stored as a STRIP: `kLutSize` slices side by side, each `kLutSize` texels
/// wide and `kLutSize` tall, row-major with a top-left origin — the layout
/// `ImageDecode` produces and the shader reads. `strength` at 0 is exactly
/// identity, and a LUT of the wrong size is ignored rather than read out of
/// bounds.
Vec3 apply_color_lut(Vec3 color, std::span<const u8> rgba, float strength);

/// CPU mirror of the lens stage: the coordinate the HDR is FETCHED from for
/// channel `channel` (0 = red, 1 = green, 2 = blue), after barrel distortion
/// and chromatic aberration. Matches lens_sample_uv() in tonemap.frag.
///
/// This is a separate mirror from apply_post_chain on purpose: the lens stage
/// rewrites WHERE the image is read from, and apply_post_chain receives the
/// colour already fetched — it has no coordinate to warp. Keeping the two apart
/// is what lets both be exact: with distortion and aberration at zero this
/// returns `uv` unchanged for every channel, which is the identity the golden
/// pixels rest on, and a test can pin that per channel.
Vec2 lens_sample_uv(Vec2 uv, const LensParams& params, int channel);

/// CPU mirror of the depth-of-field circle of confusion, in output texels.
/// Matches dof_coc() in tonemap.frag: 0 inside the focus band, rising linearly
/// to `max_radius` outside it, and 0 whenever the stage is off.
///
/// This is the part of the stage with the physics in it, so it is the part that
/// is mirrored. The gather that consumes it needs a whole image (12 taps of
/// depth and colour), which is why it is covered by a GPU test instead — the
/// same split the bloom chain uses.
float dof_coc(float view_depth, const DofParams& params);

/// CPU mirror of the motion-blur reprojection: where the surface under `uv` was
/// on the PREVIOUS frame's screen, in uv. Matches motion_prev_uv() in
/// tonemap.frag.
///
/// `depth` is the raw depth-buffer value in [0, 1]; anything at the far plane
/// (no surface, i.e. the sky) returns `uv` unchanged, because there is nothing
/// to reproject. `inv_view_proj` is the current inverse and `prev_view_proj`
/// the previous forward view-projection.
Vec2 motion_prev_uv(Vec2 uv, float depth, const Mat4& inv_view_proj,
                    const Mat4& prev_view_proj);

/// CPU mirror of the smear the motion-blur stage actually applies: the
/// reprojected velocity scaled by `intensity` and capped at `max_length`, in uv
/// units. Zero whenever the stage is off — and, importantly, zero whenever the
/// camera has not moved, which is what makes the stage free in a still frame.
Vec2 motion_smear(Vec2 uv, float depth, const Mat4& inv_view_proj,
                  const Mat4& prev_view_proj, const MotionBlurParams& params);

/// CPU mirror of the whole tonemap.frag fragment body, in the shader's own
/// order: sharpen -> bloom add -> exposure -> grade -> tonemap operator ->
/// gamma -> saturation -> vignette.
///
/// `blurred` and `bloom` are pass outputs the CPU cannot compute without a
/// whole image, so the caller supplies them; everything the shader does with
/// them, and the order it does it in, is mirrored here. This exists so the
/// stack's *ordering* is testable: a stage moved in the shader without moving
/// it here fails the comparison, which is the failure that actually matters
/// and that no per-stage unit test can see.
Vec3 apply_post_chain(Vec3 hdr, Vec3 blurred, Vec3 bloom, Vec2 uv,
                      float exposure, TonemapMode mode, const PostFxParams& params);

class Renderer3D {
public:
    static constexpr u32 kMaxPointLights = 8;  // must match lighting.frag
    static constexpr u32 kMaxSpotLights = 8;   // must match lighting.frag

    // Bloom mip count. Every level is half the previous one, so level i covers
    // 1/(2^(i+1)) of each axis and the widest level's bilinear taps reach
    // across a large fraction of the screen — which is why four is enough and
    // why the sum of the levels reads as one wide glow rather than four
    // visible rings. Must match the binding count tonemap.frag declares.
    static constexpr u32 kBloomLevels = 4;
    static_assert(kBloomLevels >= 2,
                  "one level is a blur, not a bloom: the sum needs at least "
                  "two scales to read as a glow");

    struct Stats {
        u32 extracted = 0;   // objects received in the RenderWorld
        u32 visible = 0;     // after frustum culling
        u32 draw_calls = 0;
        f64 cull_us = 0;         // CPU time spent culling
        f64 draw_prep_us = 0;    // CPU time composing draws
        // Material descriptor sets built this frame. Material instances are
        // shared and their sets are cached, so in steady state this stays at 0
        // regardless of object count — that is the point of the cache, and the
        // number exists so a test can assert it.
        u32 material_sets_built = 0;
        // Objects routed to the forward Transparency pass (base colour alpha <
        // 1). Zero means the pass recorded no draw and the frame is pixel-
        // identical to one this renderer produced before the pass existed.
        u32 transparent_objects = 0;
        // Bloom passes recorded this frame: 0 when the stage is off, and
        // kBloomLevels when it is on (one prefilter plus kBloomLevels-1
        // downsamples). Exposed for the same reason `transparent_objects` is —
        // "the stage is enabled" and "the stage recorded work" are different
        // claims, and only the second one is worth asserting.
        u32 bloom_levels_recorded = 0;
    };

    Renderer3D() = default;
    ~Renderer3D();

    Renderer3D(const Renderer3D&) = delete;
    Renderer3D& operator=(const Renderer3D&) = delete;

    /// Loads shaders, builds render passes/pipelines and allocates the
    /// graph-owned targets (Depth, GBuffer0-2, HDR) at the given resolution.
    bool init(rhi::IGraphicsDevice& device, const std::filesystem::path& shader_dir,
              u32 width, u32 height);
    void shutdown();

    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    void resize(u32 width, u32 height);

    /// The renderer resolves RenderObject mesh handles against this library.
    /// Must be set before render(); ownership stays with the caller.
    void set_mesh_library(MeshLibrary* library) { m_mesh_library = library; }

    // --- Materials ---
    MaterialLibrary& materials() { return *m_material_library; }
    Material* gbuffer_material() { return m_gbuffer_material.get(); }

    // --- Lights ---
    void set_directional_light(const DirectionalLight& light) { m_directional = light; }
    const DirectionalLight& directional_light() const { return m_directional; }
    // Scalar ambient: the flat fill the IBL bake REPLACES when it is live
    // (see set_ibl_enabled). Kept as the fallback for a renderer/device that
    // never baked — an unset IBL reads as this, never as black.
    void set_ambient(float ambient) { m_ambient = ambient; }

    // --- Image-based lighting (sky environment bake) ---
    // On by default: the sky gradient + haze + sun light every surface
    // directionally (diffuse) and glossily (specular split-sum), which is what
    // lifts shaded sides out of flat grey and gives metals something real to
    // reflect. The multipliers are artistic trims on top of a physical base:
    // 1.0 = the bake as-is. Pure state (no device), like set_sky.
    void set_ibl_enabled(bool enabled) { m_ibl_enabled = enabled; }
    bool ibl_enabled() const { return m_ibl_enabled; }
    void set_ibl_intensity(float diffuse, float specular) {
        m_ibl_diffuse = diffuse;
        m_ibl_specular = specular;
    }

    // --- Screen-space ambient occlusion ---
    // On by default: hemisphere occlusion from the depth buffer darkens
    // crevices, corners and contact points (the ambient — scalar and IBL —
    // is multiplied, never the direct light). Radius is in world units;
    // bias lifts sample rays off the surface so flat walls do not freckle.
    // Pure state (no device).
    void set_ssao_enabled(bool enabled) { m_ssao_enabled = enabled; }
    bool ssao_enabled() const { return m_ssao_enabled; }
    void set_ssao_intensity(float intensity) { m_ssao_intensity = intensity; }
    float ssao_intensity() const { return m_ssao_intensity; }
    void set_ssao_radius(float radius) { m_ssao_radius = radius; }
    float ssao_radius() const { return m_ssao_radius; }
    void set_ssao_bias(float bias) { m_ssao_bias = bias; }
    float ssao_bias() const { return m_ssao_bias; }

    // --- Sky (Phase 13): procedural gradient + sun disk, painted by the
    // lighting pass where depth reads far. Pure state: no device needed.
    void set_sky(const SkyParams& sky) { m_sky = sky; }
    const SkyParams& sky() const { return m_sky; }

    u32 add_point_light(const PointLight& light);
    u32 add_spot_light(const SpotLight& light);
    void clear_point_lights() { m_point_lights.clear(); }
    void clear_spot_lights() { m_spot_lights.clear(); }
    PointLight& point_light(u32 index) { return m_point_lights[index]; }
    SpotLight& spot_light(u32 index) { return m_spot_lights[index]; }
    u32 point_light_count() const { return static_cast<u32>(m_point_lights.size()); }
    u32 spot_light_count() const { return static_cast<u32>(m_spot_lights.size()); }

    // --- Distance fog (applied in the lighting and transparency passes) ---
    // Linear fade of a surface toward a haze colour over world-space distance
    // from the camera. Off by default, and the default band sits past every
    // test scene, so a renderer that never calls this renders as before — the
    // same "unset is a no-op" contract as the tonemap mode.
    //
    // Deliberately NOT a SkyParams field: SkyParams is the sky's own state and
    // lives in Sky.hpp, and fog is a surface effect the lighting pass owns.
    // A scene that wants the terrain to fade INTO its horizon should set
    // `color` to the sky's horizon colour itself; the coupling is the caller's,
    // because two scenes disagree about whether haze should match the sky.
    struct FogParams {
        bool enabled = false;
        Vec3 color{0.62f, 0.66f, 0.70f}; // haze tint, linear
        float start = 60.0f;             // distance the fade begins, world units
        float end = 120.0f;              // distance the surface is fully haze
    };

    void set_fog(const FogParams& fog) { m_fog = fog; }
    const FogParams& fog() const { return m_fog; }

    void set_exposure(float exposure) { m_exposure = exposure; }
    float exposure() const { return m_exposure; }

    // --- Tone mapping (pre-gamma operator on HDR colour) ---
    // Default is Exponential, which is what every golden pixel was authored
    // against, so a renderer that never calls this renders as before.
    void set_tonemap_mode(TonemapMode mode) { m_tonemap_mode = mode; }
    TonemapMode tonemap_mode() const { return m_tonemap_mode; }

    // --- Post FX (design §206: the post stack) ---
    // One setter for the whole stack. There is deliberately no per-stage setter
    // set: a caller that wants to change one stage reads the current block,
    // changes the field and writes it back, which keeps the "who owns this
    // state" question answerable (the caller does, and it round-trips).
    void set_postfx(const PostFxParams& params) { m_postfx = params; }
    const PostFxParams& postfx() const { return m_postfx; }

    /// The colour-grading LUT, as a 2D strip of `kLutSize^3` texels. NOT owned:
    /// the caller keeps the view alive, and it must stay alive until the next
    /// call — the runtime hands it the view it already caches for the path.
    ///
    /// Passing nullptr (or calling `clear_color_lut`) unbinds it, and the
    /// renderer binds its own 1x1 white texture in the slot instead: an
    /// unwritten binding is not a valid set, and the shader's `lut_strength`
    /// guard means the substitute is then never sampled.
    void set_color_lut(const rhi::TextureView* lut_view) { m_color_lut_view = lut_view; }
    void clear_color_lut() { m_color_lut_view = nullptr; }
    bool has_color_lut() const { return m_color_lut_view != nullptr; }

    /// True when the bloom chain is actually recorded this frame: the stage is
    /// enabled AND the shaders/pipelines for it came up. A device or a shader
    /// directory without bloom_*.spv leaves this false while everything else
    /// still renders — the same degradation contract as present_texture.
    bool bloom_active() const {
        return m_postfx.bloom.enabled && m_bloom_pipeline != nullptr;
    }

    // --- Terrain layer splat (design 59: splat maps) ---
    // A surface the mesh has classified carries `uv1 = (slot, blend)`; slot 0
    // is the base material itself, so the palette only answers for the layers
    // above it. Slots are 1..kSplatPaletteLayers-1 and default to white, which
    // is why an author who never calls this renders as before. Flat colours
    // for now; the slot/blend plumbing already reaches the shader, so a
    // per-layer texture array is an additive change to this API.
    static constexpr u32 kSplatPaletteLayers = 8; // must match gbuffer.frag

    void set_splat_layer_color(u32 slot, const Vec3& color);
    Vec3 splat_layer_color(u32 slot) const;

    // --- LOD policy ---
    // max_distances[i] is the farthest camera distance still drawn at lod i
    // (see select_lod). Defaults sit beyond any test scene, so content
    // without LODs — or an author who never touches this — renders exactly
    // as before. Empty bands disable selection entirely (always lod 0).
    void set_lod_max_distances(std::vector<float> bands) { m_lod_max_distances = std::move(bands); }
    const std::vector<float>& lod_max_distances() const { return m_lod_max_distances; }

    // --- Shadow quality ---
    // Global, unlike the per-light count/distance above: these describe the
    // atlas itself. Cascade count and distance are art direction and travel on
    // the light (DirectionalLight::shadow_cascades / ::shadow_distance); tile
    // resolution and the split blend are engine settings that must not differ
    // between two lights sharing one atlas.
    //
    // The tile size may be changed at any time, including after init(): the
    // atlas is rebuilt on the next render(). Default 1024 makes the 2x2 atlas
    // 2048x2048 — byte-for-byte the memory the single shadow map used before
    // cascades existed, now spent on four ranges instead of one.
    void set_shadow_tile_size(u32 tile);
    u32 shadow_tile_size() const { return m_shadow_tile_size; }
    u32 shadow_atlas_size() const { return m_shadow_tile_size * kShadowTileGrid; }
    void set_shadow_lambda(float lambda);
    float shadow_lambda() const { return m_cascades.lambda; }
    void set_shadow_fade_range(float fade_range);
    float shadow_fade_range() const { return m_cascades.fade_range; }

    // --- Local shadow quality (Phase 21) ---
    // Same contract as the cascade tile size, for the point/spot atlas: it is
    // a property of the atlas, not of any one light, and applies on the next
    // render(). Default 512 makes the 7x7 atlas 3584 — larger than the
    // directional one per-tile, because a point light's six tiles each cover a
    // much shorter range than a cascade and a coarse texel there shows up as a
    // hard, crawling shadow edge on a light the player stands next to.
    void set_local_shadow_tile_size(u32 tile);
    u32 local_shadow_tile_size() const { return m_local_shadow_tile_size; }
    u32 local_shadow_atlas_size() const {
        return m_local_shadow_tile_size * kLocalShadowTileGrid;
    }
    /// The effective global cascade settings (sanitised).
    CascadeConfig shadow_config() const { return sanitize_cascade_config(m_cascades); }

    /// Records one full frame into `cmd` (which must be in recording state):
    ///   Frustum Culling → Shadow atlases → DepthPrepass → GBuffer → Lighting
    ///     → Transparency → Tonemap.
    /// The tonemapped result lands in `out_target`. When the target is a
    /// swapchain image, pass out_is_present_source=true so the final layout
    /// is presentation-ready.
    ///
    /// `frame_slot` selects which per-slot frame resources (frame-uniform
    /// buffer, descriptor allocator) this call uses. The caller must have
    /// waited the GPU work previously recorded through the SAME slot — two
    /// slots let one frame execute on the GPU while the next is recorded.
    /// Callers that serialise every frame may always pass 0 (the default).
    bool render(rhi::CommandBuffer& cmd, const RenderWorld& render_world,
                const Camera& camera, rhi::Texture& out_target,
                bool out_is_present_source = false, u32 frame_slot = 0);

    /// Records ONLY the final tonemap pass, sampling `src` (an already-rendered
    /// LDR target, e.g. the editor's offscreen viewport) into `out_target`. No
    /// scene, no shadows: this exists so a caller that already holds a finished
    /// frame image can present it without re-running the whole pipeline — the
    /// editor used to render its full scene a SECOND time per frame purely to
    /// fill the swapchain behind the UI.
    ///
    /// `src_view` is the sampled view of `src` (required — the RHI texture
    /// interface has no implicit view). Same slot contract as render(); the
    /// slot's PRESENT allocator is used, separate from the scene allocator so
    /// a caller may run a scene render and a present in the same frame.
    bool present_texture(rhi::CommandBuffer& cmd, rhi::Texture& src,
                         rhi::TextureView* src_view, rhi::Texture& out_target,
                         bool out_is_present_source = false, u32 frame_slot = 0);

    const Stats& last_stats() const { return m_stats; }


    /// Readback-friendly access to intermediate targets for tests. The depth
    /// target carries TransferSrc usage, so copy_texture_to_buffer works.
    rhi::Texture* depth_target() { return m_graph->get_texture(m_depth_handle); }
    /// 0 = base color, 1 = normal, 2 = surface, 3 = emissive radiance.
    ///
    /// Index 3 exists because the emission COLOUR had no home in the deferred
    /// path: the material block has carried `emission.rgb` since it was written,
    /// but the gbuffer had only three attachments and the lighting pass is
    /// fullscreen with no per-object data, so `lighting.frag` multiplied the
    /// ALBEDO by `emission_strength` and the colour was parsed, saved,
    /// round-tripped and never read. A material with a dark base colour and a
    /// bright emission therefore rendered black.
    rhi::Texture* gbuffer_target(u32 index);
    rhi::Texture* hdr_target() { return m_graph->get_texture(m_hdr_handle); }

private:
    bool create_resolution_dependent(u32 width, u32 height);
    void destroy_resolution_dependent();
    /// Registers the bloom mip chain with the graph. MUST run before
    /// m_graph->compile(): a graph texture is materialized by compile(), and a
    /// handle created after the last compile() resolves to nullptr until the
    /// next one — which is the next frame's compile(), i.e. a first frame that
    /// records bloom passes against nothing.
    void create_bloom_textures(u32 width, u32 height);

    // Frame uniform block — must stay in sync with lighting.frag (std140).
    struct FrameUniforms {
        float inv_view_proj[16];
        float cam_pos_ambient[4];   // xyz camera, w ambient
        // The PREVIOUS frame's view-projection, for the depth-reprojection
        // motion blur. It sits here, next to the other per-frame camera data,
        // rather than appended at the end: the tonemap pass declares a PREFIX of
        // this block (a std140 block may do that), and a field at the far end
        // would force it to redeclare the other forty floats.
        //
        // ⚠️ Its position must match brdf.glsl's `FrameUniforms` block exactly —
        // every member after it moves, on BOTH sides. Inserting a field here
        // without inserting it there silently misreads the directional light,
        // the shadows and the sky.
        float prev_view_proj[16];
        float dir_dir_enable[4];    // xyz direction, w enabled
        float dir_color_int[4];     // rgb color, a intensity
        // One world -> shadow-clip transform per cascade. A std140 mat4 array is
        // tightly packed (each element already 16-byte aligned), so this is laid
        // out exactly like four consecutive mat4s.
        float light_view_proj[kMaxShadowCascades][16];
        float shadow_params[4];     // x enabled, y strength, z bias, w texel (1/atlas)
        float cascade_splits[4];    // view-space FAR distance covered by cascade i
        float cascade_info[4];      // x count, y tile uv scale, z fade range, w unused
        float cascade_bias[4];      // per-cascade MINIMUM bias, in NDC (added to z above)
        float cam_forward[4];       // xyz camera forward axis (cascade selection)
        float sky_zenith[4];        // rgb zenith, w unused (Phase 13 sky)
        float sky_horizon[4];       // rgb horizon, w unused
        float sky_ground[4];        // rgb below-horizon, w unused
        float sky_params[4];        // x enabled, y sun disk mul, z sun glow mul, w unused
        float sky_clear[4];         // rgb fallback when the sky is disabled
        // z/w are the local shadow atlas (Phase 21): the shader turns a tile
        // index into atlas uv with the grid and sizes one PCF tap with the tile
        // pixel size, so widening the atlas on the CPU needs no shader edit.
        i32 counts[4];              // x points, y spots, z tile grid, w tile pixels
        struct PointGPU {
            float pos_radius[4];
            float color_int[4];
            float pad[4];
        };
        struct SpotGPU {
            float pos[4];
            float dir_inner[4];
            float color_int[4];
            // x = outer cone cos, y = range. The name is not `outer_pad`
            // anymore because two of the four slots carry real data; yzw were
            // genuinely padding before range moved in, so this costs no extra
            // uniform — the struct is still four vec4s either side of the
            // change, and the offsets below are untouched.
            float outer_range[4];
        };
        PointGPU points[kMaxPointLights];
        SpotGPU spots[kMaxSpotLights];
        // Phase 21: point/spot shadow projectors, one per atlas TILE rather
        // than per light — a point light owns six consecutive tiles and a spot
        // one, so indexing by tile gives the six faces of one light a single
        // lookup rule in the shader. std140 mat4 arrays are tightly packed
        // (every element is already 16-byte aligned), so this is exactly
        // kLocalShadowTileCount consecutive mat4s.
        float local_shadow_view_proj[kLocalShadowTileCount][16];
        // x enabled, y strength, z the tile's DERIVED minimum bias, w the
        // light's own bias. The derived term is per-tile because each tile's
        // texel size and depth span are its own; the artist's term is per-light
        // and duplicated across a point light's six faces. The shader adds the
        // two (see lighting.frag) the way it adds cascade_bias on the
        // directional path.
        float local_shadow_params[kLocalShadowTileCount][4];
        // Fog (distance haze). Appended at the END of the block rather than
        // slotted in by the sky fields: everything above is addressed by
        // offset, and appending moves no existing field. x = enabled is a
        // float because std140 has no bools; the shader compares it.
        float fog_color[4];       // rgb haze tint, w unused
        float fog_params[4];      // x enabled, y start, z end, w unused
        // Procedural clouds (P4 weather). APPENDED, so no field above it moves
        // and the tonemap pass's 144-byte PREFIX of this block is unaffected.
        // x = coverage, y = altitude, z = scale, w = time (hours).
        float sky_cloud[4];
        // Image-based lighting (sky environment bake). APPENDED last, so only
        // the total moves: x = enabled, y = diffuse multiplier, z = specular
        // multiplier, w = env mip count - 1 (0 = sample level 0 only).
        float ibl_params[4];
        // Screen-space ambient occlusion. APPENDED last: x = enabled,
        // y = intensity, z = radius (world units), w = bias.
        float ssao_params[4];
    };
    // Per-field offsets rather than one hand-summed total: a field inserted in
    // the middle shifts everything after it, and the sum only notices when the
    // shift happens to change the total. The named offsets say exactly which
    // field drifted, and where the shader expects it.
    //
    // `prev_view_proj` sits at 80 — deliberately in the middle, so the tonemap
    // pass can declare a PREFIX of the block — and every offset below it moved
    // by one mat4. Both sides (here and brdf.glsl) moved together.
    static_assert(offsetof(FrameUniforms, inv_view_proj) == 0);
    static_assert(offsetof(FrameUniforms, cam_pos_ambient) == 64);
    static_assert(offsetof(FrameUniforms, prev_view_proj) == 80);
    static_assert(offsetof(FrameUniforms, dir_dir_enable) == 144);
    static_assert(offsetof(FrameUniforms, dir_color_int) == 160);
    static_assert(offsetof(FrameUniforms, light_view_proj) == 176);
    static_assert(offsetof(FrameUniforms, shadow_params) == 432);
    static_assert(offsetof(FrameUniforms, cascade_splits) == 448);
    static_assert(offsetof(FrameUniforms, cascade_info) == 464);
    static_assert(offsetof(FrameUniforms, cascade_bias) == 480);
    static_assert(offsetof(FrameUniforms, cam_forward) == 496);
    static_assert(offsetof(FrameUniforms, sky_zenith) == 512);
    static_assert(offsetof(FrameUniforms, sky_horizon) == 528);
    static_assert(offsetof(FrameUniforms, sky_ground) == 544);
    static_assert(offsetof(FrameUniforms, sky_params) == 560);
    static_assert(offsetof(FrameUniforms, sky_clear) == 576);
    static_assert(offsetof(FrameUniforms, counts) == 592);
    static_assert(offsetof(FrameUniforms, points) == 608);
    static_assert(offsetof(FrameUniforms, spots) ==
                  608 + kMaxPointLights * 48); // both are 16-byte aligned vec4s
    // The two light arrays are the last fields whose size depends on the LIGHT
    // limits; the local shadow block depends on the TILE count instead, so its
    // offsets are written as "after the light arrays" rather than as a number.
    static constexpr u32 kLocalShadowBlockOffset =
        608 + kMaxPointLights * 48 + kMaxSpotLights * 64;
    static_assert(offsetof(FrameUniforms, local_shadow_view_proj) ==
                  kLocalShadowBlockOffset);
    static_assert(offsetof(FrameUniforms, local_shadow_params) ==
                  kLocalShadowBlockOffset + kLocalShadowTileCount * 64);
    // Appended, so only the total and the two new offsets move.
    static constexpr u32 kFogBlockOffset =
        kLocalShadowBlockOffset + kLocalShadowTileCount * 80;
    static_assert(offsetof(FrameUniforms, fog_color) == kFogBlockOffset);
    static_assert(offsetof(FrameUniforms, fog_params) == kFogBlockOffset + 16);
    static_assert(offsetof(FrameUniforms, sky_cloud) == kFogBlockOffset + 32);
    static_assert(offsetof(FrameUniforms, ibl_params) == kFogBlockOffset + 48);
    static_assert(offsetof(FrameUniforms, ssao_params) == kFogBlockOffset + 64);
    static_assert(sizeof(FrameUniforms) == kFogBlockOffset + 80,
                  "FrameUniforms must match the shader's std140 layout");

    rhi::IGraphicsDevice* m_device = nullptr;

    // Resolution-dependent pass resources
    u32 m_width = 0;
    u32 m_height = 0;
    std::unique_ptr<RenderGraph> m_graph; // owns Depth/GBuffer/HDR targets

    // --- shaders / pipelines (owned) ---
    std::unique_ptr<rhi::ShaderModule> m_depth_vs, m_depth_fs;
    std::unique_ptr<rhi::ShaderModule> m_gbuffer_vs, m_gbuffer_fs;
    std::unique_ptr<rhi::ShaderModule> m_lighting_vs, m_lighting_fs;
    std::unique_ptr<rhi::ShaderModule> m_forward_vs, m_forward_fs;
    std::unique_ptr<rhi::ShaderModule> m_tonemap_vs, m_tonemap_fs;
    // Bloom chain (see BloomParams). One shader pair serves every level: the
    // prefilter and the downsample are the same "sample one texture, write one
    // colour" shape and differ only in the kernel, which rides a push-constant
    // mode flag. Two pipelines for the same two entry points would be two
    // pipeline objects, two cache keys and two places to forget the mode.
    // Optional at init, like the present pair: a shader directory without
    // bloom_*.spv disables the bloom stage and nothing else.
    std::unique_ptr<rhi::ShaderModule> m_bloom_vs, m_bloom_fs;
    // Present passthrough (see present_texture). Optional at init: an older
    // shader directory without present_*.spv only disables present_texture(),
    // never the scene path.
    std::unique_ptr<rhi::ShaderModule> m_present_vs, m_present_fs;
    PipelineCache* m_pipeline_cache = nullptr;
    std::unique_ptr<Material> m_gbuffer_material;
    rhi::Pipeline* m_depth_pipeline = nullptr;    // owned by m_pipeline_cache
    rhi::Pipeline* m_lighting_pipeline = nullptr; // owned by m_pipeline_cache
    rhi::Pipeline* m_forward_pipeline = nullptr;  // owned by m_pipeline_cache
    rhi::Pipeline* m_tonemap_pipeline_off = nullptr;
    rhi::Pipeline* m_tonemap_pipeline_present = nullptr;
    rhi::Pipeline* m_present_pipeline_off = nullptr;   // owned by m_pipeline_cache
    rhi::Pipeline* m_present_pipeline_present = nullptr;
    rhi::Pipeline* m_bloom_pipeline = nullptr;         // owned by m_pipeline_cache

    /// Lazily creates the swapchain-variant tonemap/present render pass and
    /// pipelines (they need the swapchain extension, which headless/CI devices
    /// do not have). Idempotent; returns false only if the device refused.
    bool ensure_present_variants();

    // --- descriptor layouts ---
    std::unique_ptr<rhi::DescriptorSetLayout> m_material_layout;   // set 0: UBO + albedo
    std::unique_ptr<rhi::DescriptorSetLayout> m_lighting_layout;   // set 0: 5 textures + UBO
    std::unique_ptr<rhi::DescriptorSetLayout> m_tonemap_layout;    // set 0: HDR texture
    // The transparency pass needs BOTH the frame/shadow bindings the lighting
    // pass uses AND the material bindings the gbuffer pass uses, but the RHI
    // pipeline takes one layout — so this is the union in a single set. The
    // binding numbers match brdf.glsl exactly (frame UBO 4, cascade atlas 5,
    // local atlas 6) which is what lets one shader file serve both paths; the
    // material pair sits at 7-8 where neither lighting.frag nor the shared
    // include looks.
    std::unique_ptr<rhi::DescriptorSetLayout> m_forward_layout;    // set 0: material + frame
    // Bloom chain: one sampled image in, one colour out. Deliberately NOT the
    // tonemap layout even though the shape matches, because the two describe
    // different things and a future bloom binding (a per-level weight table)
    // must not silently appear in the tonemap shader's set.
    std::unique_ptr<rhi::DescriptorSetLayout> m_bloom_layout;      // set 0: source level

    // --- render passes (owned) ---
    std::unique_ptr<rhi::RenderPass> m_depth_rp;
    std::unique_ptr<rhi::RenderPass> m_gbuffer_rp;
    std::unique_ptr<rhi::RenderPass> m_lighting_rp;
    // Blends over the HDR image Lighting produced. color_load = Load keeps
    // that image (a clear here would blank the whole scene) and the attachment
    // carries blend_enabled, which is where the RHI looks for blend state —
    // not on the pipeline. Depth is Loaded too: the prepass owns it, and a
    // transparent surface tests against the opaque geometry behind it without
    // writing, so two panes can overlap and still sort correctly.
    std::unique_ptr<rhi::RenderPass> m_transparency_rp;
    std::unique_ptr<rhi::RenderPass> m_tonemap_rp_off;
    // Bloom levels all share one format (RGBA16F, no depth), so they share one
    // render pass and differ only in the framebuffer bound to it. A pass per
    // level would be four identical VkRenderPass objects.
    std::unique_ptr<rhi::RenderPass> m_bloom_rp;
    // The present-source variant is created lazily on first use: it needs the
    // swapchain extension, which a headless/CI device does not have — creating
    // it eagerly would fail vkCreateRenderPass there.
    std::unique_ptr<rhi::RenderPass> m_tonemap_rp_present;
    bool m_tonemap_present_ready = false;

    // --- graph-owned targets ---
    RGTextureHandle m_depth_handle = kInvalidRGHandle;
    RGTextureHandle m_gbuffer0_handle = kInvalidRGHandle;
    RGTextureHandle m_gbuffer1_handle = kInvalidRGHandle;
    RGTextureHandle m_gbuffer2_handle = kInvalidRGHandle;
    // Emissive radiance (emission.rgb * emission_strength), already multiplied
    // so the lighting pass only has to add it. See gbuffer_target().
    RGTextureHandle m_gbuffer3_handle = kInvalidRGHandle;
    RGTextureHandle m_hdr_handle = kInvalidRGHandle;
    // The bloom mip chain, level 0 = half resolution. Graph-owned so the graph
    // transitions them, exactly like HDR; created at the same point in init/
    // resize (BEFORE compile(), which is what materializes a graph texture) and
    // sized from the render target, so a resize rebuilds them.
    RGTextureHandle m_bloom_handles[kBloomLevels] = {};

    std::unique_ptr<rhi::TextureView> m_gbuffer0_view, m_gbuffer1_view;
    std::unique_ptr<rhi::TextureView> m_gbuffer2_view, m_gbuffer3_view;
    std::unique_ptr<rhi::TextureView> m_gbuffer_depth_view;
    std::unique_ptr<rhi::TextureView> m_hdr_view;
    std::unique_ptr<rhi::TextureView> m_bloom_views[kBloomLevels];
    std::unique_ptr<rhi::Sampler> m_sampler;

    // Framebuffers: depth-only, gbuffer, lighting, transparency, and one per
    // output target
    std::unique_ptr<rhi::Framebuffer> m_depth_fb;
    std::unique_ptr<rhi::Framebuffer> m_gbuffer_fb;
    std::unique_ptr<rhi::Framebuffer> m_lighting_fb;
    std::unique_ptr<rhi::Framebuffer> m_transparency_fb;
    std::unique_ptr<rhi::Framebuffer> m_bloom_fbs[kBloomLevels];
    // Keyed by creation serial, NEVER by raw pointer: heap address reuse
    // across create/destroy cycles used to resurrect framebuffers whose image
    // views were long destroyed (invalid framebuffer + potential GPU hang).
    // Serial 0 (backends without the concept) falls back to pointer identity.
    struct TonemapFBKey {
        u64 tex_serial = 0;
        const rhi::Texture* texture = nullptr;
        bool present_source = false;
        bool operator==(const TonemapFBKey& o) const {
            return tex_serial == o.tex_serial && texture == o.texture &&
                   present_source == o.present_source;
        }
    };
    struct TonemapFBKeyHash {
        size_t operator()(const TonemapFBKey& k) const noexcept {
            size_t h = std::hash<u64>{}(k.tex_serial);
            h ^= reinterpret_cast<size_t>(k.texture) + 0x9E3779B9u + (h << 6) + (h >> 2);
            return h ^ (k.present_source ? 0x9E3779B9u : 0u);
        }
    };
    // Upper bound on cached per-target framebuffers.
    //
    // Serial keys fix invalidation-by-reuse, but they introduce the mirror
    // problem: an entry can never be recognised as dead, because nothing tells
    // the renderer that its texture was destroyed. A process that cycles
    // through many targets at one resolution (tests, thumbnails, tooling)
    // would accumulate framebuffers for textures that no longer exist. A
    // renderer realistically keeps one or two live output targets, so dropping
    // the whole cache on overflow costs one framebuffer rebuild and bounds the
    // growth.
    static constexpr usize kMaxTonemapFramebuffers = 8;
    std::unordered_map<TonemapFBKey, std::unique_ptr<rhi::Framebuffer>, TonemapFBKeyHash> m_tonemap_fbs;

    // Per-frame data, one set of each per IN-FLIGHT frame slot. While the GPU
    // executes frame N the CPU records frame N+1 into the other slot: frame
    // uniforms are rewritten every frame (they would corrupt an in-flight
    // read), and a descriptor allocator reset recycles the sets its own slot
    // handed out (an in-flight command buffer may still reference them).
    // present_texture() gets its own allocators because the editor runs a
    // scene render (viewport, cmd A) and a present (cmd B) in the SAME frame —
    // sharing one allocator would recycle sets cmd A is still executing with.
    static constexpr u32 kFramesInFlight = 2;
    std::unique_ptr<rhi::DescriptorAllocator> m_descriptor_allocators[kFramesInFlight];
    std::unique_ptr<rhi::DescriptorAllocator> m_present_allocators[kFramesInFlight];
    std::unique_ptr<MaterialLibrary> m_material_library; // created in init()
    std::unique_ptr<rhi::Buffer> m_frame_uniforms[kFramesInFlight]; // persistent, updated per frame
    std::vector<u32> m_visible;                    // culled indices into the render world

    // CPU scratch for draw submission — written and consumed during recording
    // only (push constants copy their bytes at record time), so a single
    // instance is safe to reuse once the previous frame's recording finished.
    // They live here so the capacity survives frames instead of churning
    // thousands of entries through the allocator on every scene.
    struct PreparedDraw {
        const RenderObject* object;
        const StaticMesh* mesh;
        const rhi::DescriptorSet* material_set; // non-owning: cached on the entry
        u32 lod = 0;                            // effective LOD for this frame
    };
    struct TransparentDraw {
        const RenderObject* object;
        const StaticMesh* mesh;
        const rhi::DescriptorSet* forward_set; // non-owning: held by forward_sets
        u32 lod = 0;
        float distance = 0.0f;                 // camera -> surface, for the sort
    };
    std::vector<PreparedDraw> m_prepared;
    std::vector<TransparentDraw> m_transparent;
    std::vector<std::unique_ptr<rhi::DescriptorSet>> m_forward_sets;

    // State
    MeshLibrary* m_mesh_library = nullptr;
    DirectionalLight m_directional{};
    SkyParams m_sky{};
    std::vector<PointLight> m_point_lights;
    std::vector<SpotLight> m_spot_lights;
    float m_ambient = 0.03f;
    float m_exposure = 1.0f;
    TonemapMode m_tonemap_mode = TonemapMode::Exponential;
    PostFxParams m_postfx{};
    // The colour-grading LUT, owned by the CALLER (the runtime's texture cache).
    // Null means none is bound, and the tonemap pass then binds the white
    // texture in the slot.
    const rhi::TextureView* m_color_lut_view = nullptr;
    // The view-projection the PREVIOUS frame was rendered with, for the
    // reprojection motion blur. `m_has_prev_view_proj` is false until a frame
    // has been rendered, and the first frame then reports ZERO velocity rather
    // than a jump from the identity matrix — which would smear the whole image
    // on the frame a scene opens.
    Mat4 m_prev_view_proj = Mat4::identity();
    bool m_has_prev_view_proj = false;
    FogParams m_fog{};
    Stats m_stats;
    std::vector<float> m_lod_max_distances{40.0f, 100.0f, 250.0f};

    // White 1x1 fallback texture so scalar-only materials still have a valid
    // albedo binding (the shader multiplies by it, i.e. ignores it).
    std::unique_ptr<rhi::Texture> m_white_texture;
    std::unique_ptr<rhi::TextureView> m_white_view;

    // Sky environment bake for image-based lighting (SkyEnv): 128x64 RGBA
    // half-float equirect with a full mip chain, re-baked when the sky or the
    // sun moves. Resolution-independent like the white fallback — it never
    // follows the swapchain — so it is created once in init(), not per frame.
    // m_env_baked_key is the key the LIVE texture was baked from; a mismatch
    // re-bakes before the lighting pass reads it.
    std::unique_ptr<rhi::Texture> m_env_texture;
    std::unique_ptr<rhi::TextureView> m_env_view;
    std::unique_ptr<rhi::Sampler> m_env_sampler;
    bool m_ibl_enabled = true;
    float m_ibl_diffuse = 1.0f;
    float m_ibl_specular = 1.0f;
    bool m_env_has_mips = false;
    bool m_env_baked_once = false;
    SkyEnvKey m_env_baked_key{};
    std::vector<u16> m_env_pixels; // bake scratch: capacity survives re-bakes
    bool ensure_env_map();
    // Re-bakes and re-uploads when the key moved. Returns false when there is
    // no bake to read (device refused, upload failed): the frame then falls
    // back to the scalar ambient, so a failed IBL dims nothing to black.
    bool refresh_env_map(const SkyParams& sky, const Vec3& sun_dir,
                         const Vec3& sun_color, bool light_enabled);

    // Screen-space ambient occlusion (half-resolution raw + bilateral blur).
    // Resolution-dependent like the bloom chain (rebuilt on resize, not per
    // frame); the passes are recorded per frame only when enabled.
    bool m_ssao_enabled = true;
    float m_ssao_intensity = 1.0f;
    float m_ssao_radius = 0.5f;
    float m_ssao_bias = 0.02f;
    RGTextureHandle m_ssao_raw_handle = kInvalidRGHandle;
    RGTextureHandle m_ssao_blur_handle = kInvalidRGHandle;
    std::unique_ptr<rhi::TextureView> m_ssao_raw_view;
    std::unique_ptr<rhi::TextureView> m_ssao_blur_view;
    std::unique_ptr<rhi::Framebuffer> m_ssao_raw_fb;
    std::unique_ptr<rhi::Framebuffer> m_ssao_blur_fb;
    std::unique_ptr<rhi::ShaderModule> m_ssao_vs;
    std::unique_ptr<rhi::ShaderModule> m_ssao_fs;
    std::unique_ptr<rhi::ShaderModule> m_ssao_blur_fs;
    std::unique_ptr<rhi::DescriptorSetLayout> m_ssao_layout;
    std::unique_ptr<rhi::DescriptorSetLayout> m_ssao_blur_layout;
    std::unique_ptr<rhi::RenderPass> m_ssao_rp;
    rhi::Pipeline* m_ssao_pipeline = nullptr;
    rhi::Pipeline* m_ssao_blur_pipeline = nullptr;
    void create_ssao_textures(u32 width, u32 height);

    // Terrain splat palette, bound on every gbuffer material set as binding 2.
    // One std140 array of vec4, mirrored CPU-side so a read-back matches what
    // the shader will see.
    struct SplatPaletteGPU {
        std::array<std::array<float, 4>, kSplatPaletteLayers> layers{};
    };
    SplatPaletteGPU m_splat_palette_cpu{};
    std::unique_ptr<rhi::Buffer> m_splat_palette;

    // Directional shadow atlas: resolution-independent (survives resize without
    // recreation, like the white fallback), but NOT tile-independent — changing
    // the tile size rebuilds it via ensure_shadow_atlas().
    static constexpr u32 kShadowTileSize = 1024;
    static constexpr u32 kShadowAtlasSize = kShadowTileSize * kShadowTileGrid;
    CascadeConfig m_cascades{};
    u32 m_shadow_tile_size = kShadowTileSize;
    // Edge length the LIVE atlas was created at. Tracked separately from
    // m_shadow_tile_size because a tile-size request only takes effect once the
    // rebuild succeeds, and because a failed rebuild must not leave the
    // renderer believing it has an atlas it does not.
    u32 m_shadow_atlas_size = 0;
    std::unique_ptr<rhi::Texture> m_shadow_map;
    std::unique_ptr<rhi::TextureView> m_shadow_view;
    std::unique_ptr<rhi::Framebuffer> m_shadow_fb;

    // Local (point/spot) shadow atlas: a separate texture from the cascade
    // atlas because its grid is 7x7 rather than 2x2 — one texture per light
    // family keeps either's resolution knob from moving the other, and a
    // device that cannot spare the memory for one still gets the other.
    static constexpr u32 kLocalShadowTileSize = 512;
    u32 m_local_shadow_tile_size = kLocalShadowTileSize;
    u32 m_local_shadow_atlas_size = 0;
    std::unique_ptr<rhi::Texture> m_local_shadow_map;
    std::unique_ptr<rhi::TextureView> m_local_shadow_view;
    std::unique_ptr<rhi::Framebuffer> m_local_shadow_fb;

    /// Recreates the directional atlas (texture + view + framebuffer) when it
    /// is missing or was built at a different edge length. Returns false only
    /// if the device refused the resource, in which case the previous atlas is
    /// left intact.
    bool ensure_shadow_atlas(u32 tile_size);
    /// Same contract, for the point/spot atlas.
    bool ensure_local_shadow_atlas(u32 tile_size);
};

} // namespace nf::rendering
