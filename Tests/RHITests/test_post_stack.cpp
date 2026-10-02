// RHITests — the post-processing stack (design §206).
//
// The CPU mirrors are the primary subject: every stage of the stack has a CPU
// twin of the shader function it corresponds to, so the maths, the neutral
// values and — most importantly — the ORDER the stages run in are all pinned
// without a GPU. The GPU tests then assert the other half of the claim: that
// enabling the bloom stage actually records the chain, and that its result
// reaches the pixels.
//
// The golden-pixel contract is the thing to protect here. Every stage defaults
// to neutral and every neutral value is exactly identity, so a renderer that
// never touches the stack renders the frame it rendered before the stack
// existed. `post_stack_off_equals_the_legacy_path` is the test that says so.

#include <NF/Test/RHITestCommon.hpp>
#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/RenderWorld.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

using namespace nf;
using namespace nf::rendering;
using nf::test::GpuFixture;
using nf::test::kGpuTimeoutNs;
using nf::test::Pixel;
using nf::test::require_gpu;

namespace {

namespace fs = std::filesystem;

#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

fs::path basic3d_shader_dir() {
    fs::path p(NF_BASIC3D_SHADER_DIR);
    if (!p.empty() && fs::exists(p)) return p;
    return {};
}

Camera make_camera(float z = 3.0f, float aspect = 1.0f) {
    Camera cam{};
    cam.position = {0, 0, z};
    cam.target = {0, 0, 0};
    cam.up = {0, 1, 0};
    cam.aspect = aspect;
    cam.fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
    cam.near_plane = 0.1f;
    cam.far_plane = 100.0f;
    update_camera(cam);
    return cam;
}

/// Renders one frame of an EMPTY scene (so the sky is the only thing in it)
/// through `renderer`, with the post block `params`, and reads the result back.
///
/// An empty scene is deliberate: the sky is a known, bright, non-black HDR
/// image, which is what makes the bloom tests meaningful — a black frame blooms
/// to black and would let a completely broken chain pass.
bool render_empty_scene(rhi::IGraphicsDevice& dev, Renderer3D& renderer,
                        u32 width, u32 height, const PostFxParams& params,
                        std::vector<Pixel>& out_pixels) {
    renderer.set_postfx(params);

    RenderWorld world;
    const Camera camera = make_camera(3.0f, float(width) / float(height));

    rhi::TextureDesc target_desc{};
    target_desc.width = width;
    target_desc.height = height;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled |
                        rhi::ImageUsage::TransferSrc;
    auto target = dev.create_texture(target_desc);
    if (!target) return false;

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    if (!cmd || !fence) return false;

    cmd->begin();
    if (!renderer.render(*cmd, world, camera, *target)) return false;
    rhi::BufferDesc rb_desc{};
    rb_desc.size = usize(width) * height * 4;
    rb_desc.usage = rhi::BufferUsage::TransferDst;
    rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = dev.create_buffer(rb_desc);
    if (!rb) return false;
    cmd->copy_texture_to_buffer(*target, *rb, 0, 0, width, height, 0);
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(kGpuTimeoutNs)) return false;
    dev.wait_idle();

    auto* px = static_cast<Pixel*>(rb->map());
    if (!px) return false;
    out_pixels.assign(px, px + usize(width) * height);
    rb->unmap();
    return true;
}

/// Number of pixels that differ by more than `tolerance` in any channel.
u32 count_different_pixels(const std::vector<Pixel>& a, const std::vector<Pixel>& b,
                           u8 tolerance = 4) {
    u32 n = 0;
    const usize count = std::min(a.size(), b.size());
    for (usize i = 0; i < count; ++i) {
        const int dr = std::abs(int(a[i].r) - int(b[i].r));
        const int dg = std::abs(int(a[i].g) - int(b[i].g));
        const int db = std::abs(int(a[i].b) - int(b[i].b));
        if (dr > tolerance || dg > tolerance || db > tolerance) ++n;
    }
    return n;
}

/// The legacy tail, spelled out: exposure + operator, then gamma, then the
/// saturation/vignette pair. `apply_post_chain` with every new stage off must
/// agree with this, and it is written independently of the implementation on
/// purpose — a test that recomputed it with the same helper would prove
/// nothing.
Vec3 legacy_tail(Vec3 hdr, Vec2 uv, float exposure, TonemapMode mode,
                 const PostFxParams& params) {
    Vec3 mapped = tonemap(hdr, exposure, mode);
    const float inv_gamma = 1.0f / 2.2f;
    mapped = Vec3{std::pow(mapped.x, inv_gamma), std::pow(mapped.y, inv_gamma),
                  std::pow(mapped.z, inv_gamma)};
    return apply_postfx(mapped, uv, params);
}

} // namespace

// ---------------------------------------------------------------------------
// The neutral contract
// ---------------------------------------------------------------------------

NF_TEST(post_stack_default_params_are_neutral) {
    const PostFxParams p{};
    NF_CHECK_NEAR(p.saturation, 1.0f, 1e-6f);
    NF_CHECK_NEAR(p.vignette, 0.0f, 1e-6f);
    NF_CHECK(!p.bloom.enabled);
    NF_CHECK_NEAR(p.bloom.intensity, 1.0f, 1e-6f);
    NF_CHECK(!p.grade.enabled);
    NF_CHECK_NEAR(p.grade.contrast, 1.0f, 1e-6f);
    NF_CHECK_NEAR(p.grade.temperature, 0.0f, 1e-6f);
    NF_CHECK_NEAR(p.grade.tint, 0.0f, 1e-6f);
    NF_CHECK_NEAR(p.grade.gamma, 1.0f, 1e-6f);
    NF_CHECK(!p.sharpen.enabled);
    NF_CHECK_NEAR(p.sharpen.amount, 0.0f, 1e-6f);
}

NF_TEST(post_stack_off_equals_the_legacy_path) {
    // The golden-pixel contract. With the whole stack off, the chain must
    // reproduce the pre-stack pipeline exactly (to float tolerance — the CPU
    // saturation mirror is `luma + (c - luma)*1`, which is not bit-identical to
    // the shader's `mix()`, and that divergence predates this stage).
    const PostFxParams p{};
    const Vec3 hdr{0.7f, 0.35f, 0.12f};
    for (float exposure : {0.5f, 1.0f, 2.5f}) {
        for (TonemapMode mode : {TonemapMode::Exponential, TonemapMode::ACES,
                                 TonemapMode::Reinhard, TonemapMode::Linear}) {
            for (Vec2 uv : {Vec2{0.5f, 0.5f}, Vec2{0.05f, 0.95f}}) {
                // A deliberately absurd bloom image and neighbour average: with
                // the stages off, neither may reach the result.
                const Vec3 got = apply_post_chain(hdr, Vec3{9.0f, 9.0f, 9.0f},
                                                  Vec3{9.0f, 9.0f, 9.0f}, uv, exposure,
                                                  mode, p);
                const Vec3 want = legacy_tail(hdr, uv, exposure, mode, p);
                NF_CHECK_NEAR(got.x, want.x, 1e-5f);
                NF_CHECK_NEAR(got.y, want.y, 1e-5f);
                NF_CHECK_NEAR(got.z, want.z, 1e-5f);
            }
        }
    }
}

NF_TEST(post_stack_disabled_equals_zero_strength) {
    // "enabled = false" and "enabled = true with a zero strength" must be the
    // same image. They are separate fields, so nothing but a test keeps them
    // from drifting into two different definitions of off.
    const Vec3 hdr{1.2f, 0.4f, 0.05f};
    const Vec3 blurred{0.9f, 0.3f, 0.02f};
    const Vec3 bloom{0.5f, 0.5f, 0.5f};

    PostFxParams off{};
    off.bloom.enabled = false;
    off.sharpen.enabled = false;

    PostFxParams zero{};
    zero.bloom.enabled = true;
    zero.bloom.intensity = 0.0f;
    zero.sharpen.enabled = true;
    zero.sharpen.amount = 0.0f;

    const Vec3 a = apply_post_chain(hdr, blurred, bloom, Vec2{0.5f, 0.5f}, 1.0f,
                                    TonemapMode::Exponential, off);
    const Vec3 b = apply_post_chain(hdr, blurred, bloom, Vec2{0.5f, 0.5f}, 1.0f,
                                    TonemapMode::Exponential, zero);
    NF_CHECK_NEAR(a.x, b.x, 1e-6f);
    NF_CHECK_NEAR(a.y, b.y, 1e-6f);
    NF_CHECK_NEAR(a.z, b.z, 1e-6f);
}

NF_TEST(post_stack_exposure_is_applied_exactly_once) {
    // `tonemap()` folds exposure in for its own callers; the chain applies it
    // separately so grading can sit between. Doing both would square it, which
    // at exposure 2 is a 4x error that still looks like "a bit bright".
    PostFxParams p{};
    const Vec3 hdr{0.5f, 0.5f, 0.5f};
    const Vec3 at1 = apply_post_chain(hdr, {}, {}, Vec2{0.5f, 0.5f}, 1.0f,
                                      TonemapMode::Linear, p);
    const Vec3 at2 = apply_post_chain(hdr, {}, {}, Vec2{0.5f, 0.5f}, 2.0f,
                                      TonemapMode::Linear, p);
    // Linear mode leaves the HDR value alone, so the only thing between the
    // input and the output is exposure and gamma.
    const float expected1 = std::pow(0.5f, 1.0f / 2.2f);
    const float expected2 = std::pow(1.0f, 1.0f / 2.2f);
    NF_CHECK_NEAR(at1.x, expected1, 1e-5f);
    NF_CHECK_NEAR(at2.x, expected2, 1e-5f);
}

// ---------------------------------------------------------------------------
// Bloom: prefilter
// ---------------------------------------------------------------------------

NF_TEST(post_stack_prefilter_rejects_below_the_knee) {
    // threshold 1, knee 0.5 -> anything at or below 0.5 contributes nothing.
    const Vec3 out = bloom_prefilter(Vec3{0.4f, 0.4f, 0.4f}, 1.0f, 0.5f);
    NF_CHECK_NEAR(out.x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(out.y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(out.z, 0.0f, 1e-6f);
}

NF_TEST(post_stack_prefilter_passes_above_the_threshold) {
    // Past threshold + knee the contribution is exactly (lum - threshold) / lum,
    // i.e. the pixel minus the threshold, scaled back by luminance.
    const Vec3 c{2.0f, 1.0f, 0.5f};
    const Vec3 out = bloom_prefilter(c, 1.0f, 0.5f);
    const float lum = 2.0f; // max channel, not luma
    const float expected = (lum - 1.0f) / lum;
    NF_CHECK_NEAR(out.x, c.x * expected, 1e-5f);
    NF_CHECK_NEAR(out.y, c.y * expected, 1e-5f);
    NF_CHECK_NEAR(out.z, c.z * expected, 1e-5f);
}

NF_TEST(post_stack_prefilter_uses_max_channel_not_luma) {
    // A saturated red highlight has max-channel 2.0 and Rec.709 luma 0.425.
    // A luma threshold would refuse to bloom it — the exact light source bloom
    // exists for — so the two must disagree here.
    const Vec3 red{2.0f, 0.0f, 0.0f};
    NF_CHECK(bloom_prefilter(red, 1.0f, 0.0f).x > 0.5f);
    const float luma = 2.0f * 0.2126f;
    NF_CHECK(luma < 1.0f); // would have been rejected by a luma threshold
}

NF_TEST(post_stack_prefilter_knee_is_continuous_and_monotone) {
    // Sweeping luminance across the knee must not jump: a hard cut here is the
    // "bloom pops on as the light crosses the threshold" artefact.
    const float threshold = 1.0f;
    const float knee = 0.5f;
    float prev = 0.0f;
    float max_step = 0.0f;
    for (float lum = 0.0f; lum <= 3.0f; lum += 0.01f) {
        const Vec3 out = bloom_prefilter(Vec3{lum, lum, lum}, threshold, knee);
        NF_CHECK(out.x >= prev - 1e-6f); // monotone non-decreasing
        max_step = std::max(max_step, out.x - prev);
        prev = out.x;
    }
    // One 0.01 step of luminance cannot move the output by more than the step
    // itself unless there is a discontinuity.
    NF_CHECK(max_step < 0.02f);
    // And it is continuous across the knee's ends.
    const Vec3 at_knee = bloom_prefilter(Vec3{threshold + knee, 0, 0}, threshold, knee);
    const Vec3 past_knee = bloom_prefilter(Vec3{threshold + knee + 0.001f, 0, 0}, threshold, knee);
    NF_CHECK_NEAR(at_knee.x, past_knee.x, 0.01f);
}

NF_TEST(post_stack_prefilter_knee_zero_is_a_hard_cut) {
    const Vec3 below = bloom_prefilter(Vec3{0.999f, 0.0f, 0.0f}, 1.0f, 0.0f);
    NF_CHECK_NEAR(below.x, 0.0f, 1e-6f);
    const Vec3 above = bloom_prefilter(Vec3{2.0f, 0.0f, 0.0f}, 1.0f, 0.0f);
    NF_CHECK(above.x > 0.0f);
    // At exactly the threshold the contribution is zero either way.
    const Vec3 at = bloom_prefilter(Vec3{1.0f, 0.0f, 0.0f}, 1.0f, 0.0f);
    NF_CHECK_NEAR(at.x, 0.0f, 1e-6f);
}

NF_TEST(post_stack_prefilter_preserves_colour_ratios) {
    // The prefilter scales the whole pixel by one scalar, so a surviving
    // highlight keeps its hue. A per-channel threshold would tint the glow.
    const Vec3 c{3.0f, 1.5f, 0.75f};
    const Vec3 out = bloom_prefilter(c, 1.0f, 0.0f);
    NF_CHECK(out.x > 0.0f);
    NF_CHECK_NEAR(out.y / out.x, c.y / c.x, 1e-5f);
    NF_CHECK_NEAR(out.z / out.x, c.z / c.x, 1e-5f);
}

// ---------------------------------------------------------------------------
// Bloom: downsample kernel
// ---------------------------------------------------------------------------

NF_TEST(post_stack_downsample_of_a_flat_image_is_that_image) {
    // The weights must sum to exactly 1. If they do not, the sum the tonemap
    // reads is the image scaled by the level count and the glow's brightness
    // depends on how many levels the chain happens to have.
    constexpr u32 kSize = 16;
    std::vector<Vec3> src(kSize * kSize, Vec3{0.25f, 0.5f, 0.75f});
    const Vec3 out = bloom_downsample(src, kSize, kSize, Vec2{0.5f, 0.5f}, 1.0f);
    NF_CHECK_NEAR(out.x, 0.25f, 1e-5f);
    NF_CHECK_NEAR(out.y, 0.50f, 1e-5f);
    NF_CHECK_NEAR(out.z, 0.75f, 1e-5f);
}

NF_TEST(post_stack_downsample_matches_the_13_tap_weights) {
    // Sample from the centre of texel (4,4) of an 8x8 image, so every tap lands
    // on a texel centre and the nearest-neighbour CPU readback is exact. Which
    // taps land on which texel is derived here rather than asserted as a
    // remembered constant: a tap at offset (ox, oy) reads texel
    // floor(4.5 + ox, 4.5 + oy).
    constexpr u32 kSize = 8;
    const auto lit = [&](u32 x, u32 y) {
        std::vector<Vec3> src(kSize * kSize, Vec3{0.0f, 0.0f, 0.0f});
        src[usize(y) * kSize + x] = Vec3{1.0f, 1.0f, 1.0f};
        return src;
    };
    const Vec2 centre_uv{(4.0f + 0.5f) / float(kSize), (4.0f + 0.5f) / float(kSize)};

    // A lit texel AT the sample point is reached by the centre tap only -> 0.125.
    NF_CHECK_NEAR(bloom_downsample(lit(4, 4), kSize, kSize, centre_uv, 1.0f).x, 0.125f, 1e-5f);

    // Two to the right: offset (+2, 0) is the only tap that lands on (6,4) —
    // the (+2,-2) and (+2,+2) corners land on (6,2) and (6,6) -> edge weight.
    NF_CHECK_NEAR(bloom_downsample(lit(6, 4), kSize, kSize, centre_uv, 1.0f).x, 0.0625f, 1e-5f);

    // One right and one down: offset (+1, +1) is the only tap on (5,5), and it
    // is a diagonal -> 0.125.
    NF_CHECK_NEAR(bloom_downsample(lit(5, 5), kSize, kSize, centre_uv, 1.0f).x, 0.125f, 1e-5f);

    // Two right and two down: offset (+2, +2) is a corner -> 0.03125.
    NF_CHECK_NEAR(bloom_downsample(lit(6, 6), kSize, kSize, centre_uv, 1.0f).x, 0.03125f, 1e-5f);

    // A texel outside the kernel's reach contributes nothing at all.
    NF_CHECK_NEAR(bloom_downsample(lit(7, 7), kSize, kSize, centre_uv, 1.0f).x, 0.0f, 1e-6f);
}

NF_TEST(post_stack_downsample_radius_scales_the_taps) {
    // Radius 2 moves the +2 taps out to +4, so a texel 2 away no longer
    // contributes while one 4 away now does.
    constexpr u32 kSize = 16;
    std::vector<Vec3> near_only(kSize * kSize, Vec3{0.0f, 0.0f, 0.0f});
    near_only[8 * kSize + 10] = Vec3{1.0f, 1.0f, 1.0f}; // (10,8), 2 right of centre
    const float u = 8.5f / float(kSize);
    const float v = 8.5f / float(kSize);
    const Vec3 tight = bloom_downsample(near_only, kSize, kSize, Vec2{u, v}, 1.0f);
    const Vec3 wide = bloom_downsample(near_only, kSize, kSize, Vec2{u, v}, 2.0f);
    NF_CHECK(tight.x > 0.0f);
    NF_CHECK_NEAR(wide.x, 0.0f, 1e-6f);
}

NF_TEST(post_stack_downsample_of_an_empty_image_is_black) {
    const Vec3 out = bloom_downsample({}, 0, 0, Vec2{0.5f, 0.5f}, 1.0f);
    NF_CHECK_NEAR(out.x, 0.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Sharpening
// ---------------------------------------------------------------------------

NF_TEST(post_stack_unsharp_leaves_a_flat_region_alone) {
    // In a flat region centre == blurred, so the difference is exactly zero and
    // sharpening cannot add noise to a sky.
    const Vec3 flat{0.4f, 0.4f, 0.4f};
    const Vec3 out = unsharp_hdr(flat, flat, 1.5f);
    NF_CHECK_NEAR(out.x, flat.x, 1e-6f);
    NF_CHECK_NEAR(out.y, flat.y, 1e-6f);
    NF_CHECK_NEAR(out.z, flat.z, 1e-6f);
}

NF_TEST(post_stack_unsharp_amplifies_an_edge) {
    // Centre above its neighbourhood -> pushed further up; below -> pushed down.
    const Vec3 bright = unsharp_hdr(Vec3{1.0f, 1.0f, 1.0f}, Vec3{0.5f, 0.5f, 0.5f}, 1.0f);
    NF_CHECK_NEAR(bright.x, 1.5f, 1e-6f);
    const Vec3 dark = unsharp_hdr(Vec3{0.25f, 0.25f, 0.25f}, Vec3{0.5f, 0.5f, 0.5f}, 1.0f);
    NF_CHECK_NEAR(dark.x, 0.0f, 1e-6f);
    // Zero amount is identity regardless of the neighbourhood.
    const Vec3 off = unsharp_hdr(Vec3{1.0f, 1.0f, 1.0f}, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    NF_CHECK_NEAR(off.x, 1.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Colour grading
// ---------------------------------------------------------------------------

NF_TEST(post_stack_grade_neutral_is_identity) {
    const ColorGradeParams p{};
    const Vec3 c{0.4f, 0.7f, 1.3f};
    const Vec3 out = apply_color_grade(c, p);
    NF_CHECK_NEAR(out.x, c.x, 1e-6f);
    NF_CHECK_NEAR(out.y, c.y, 1e-6f);
    NF_CHECK_NEAR(out.z, c.z, 1e-6f);
}

NF_TEST(post_stack_grade_contrast_pivots_about_the_pivot) {
    ColorGradeParams p{};
    p.contrast = 2.0f;
    p.pivot = 1.0f;
    // The pivot is a fixed point.
    const Vec3 at_pivot = apply_color_grade(Vec3{1.0f, 1.0f, 1.0f}, p);
    NF_CHECK_NEAR(at_pivot.x, 1.0f, 1e-5f);
    // Above it moves up, below it moves down, by the contrast factor.
    const Vec3 above = apply_color_grade(Vec3{1.5f, 1.5f, 1.5f}, p);
    NF_CHECK_NEAR(above.x, 2.0f, 1e-5f);
    const Vec3 below = apply_color_grade(Vec3{0.5f, 0.5f, 0.5f}, p);
    NF_CHECK_NEAR(below.x, 0.0f, 1e-5f);
}

NF_TEST(post_stack_grade_temperature_moves_red_up_and_blue_down) {
    ColorGradeParams p{};
    p.temperature = 1.0f;
    const Vec3 warm = apply_color_grade(Vec3{1.0f, 1.0f, 1.0f}, p);
    NF_CHECK_NEAR(warm.x, 1.25f, 1e-5f);
    NF_CHECK_NEAR(warm.y, 1.00f, 1e-5f);
    NF_CHECK_NEAR(warm.z, 0.75f, 1e-5f);
    p.temperature = -1.0f;
    const Vec3 cool = apply_color_grade(Vec3{1.0f, 1.0f, 1.0f}, p);
    NF_CHECK_NEAR(cool.x, 0.75f, 1e-5f);
    NF_CHECK_NEAR(cool.z, 1.25f, 1e-5f);
}

NF_TEST(post_stack_grade_tint_moves_green_only) {
    // A tint that also scaled red and blue would be a second exposure control,
    // which is the failure mode this pins.
    ColorGradeParams p{};
    p.tint = 1.0f; // magenta
    const Vec3 out = apply_color_grade(Vec3{1.0f, 1.0f, 1.0f}, p);
    NF_CHECK_NEAR(out.x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(out.y, 0.75f, 1e-5f);
    NF_CHECK_NEAR(out.z, 1.0f, 1e-6f);
}

NF_TEST(post_stack_grade_gamma_is_a_per_channel_power) {
    ColorGradeParams p{};
    p.gamma = 2.0f;
    const Vec3 out = apply_color_grade(Vec3{0.25f, 0.5f, 1.0f}, p);
    NF_CHECK_NEAR(out.x, 0.5f, 1e-5f);
    NF_CHECK_NEAR(out.y, std::sqrt(0.5f), 1e-5f);
    NF_CHECK_NEAR(out.z, 1.0f, 1e-5f);
}

NF_TEST(post_stack_grade_never_produces_a_negative_channel) {
    // A pivot below the black point drives channels negative; pow() of a
    // negative base is undefined in GLSL, so both sides clamp at zero.
    ColorGradeParams p{};
    p.contrast = 4.0f;
    p.pivot = 0.0f;
    p.gamma = 2.0f;
    const Vec3 out = apply_color_grade(Vec3{-0.5f, 0.1f, 0.1f}, p);
    NF_CHECK(out.x >= 0.0f);
    NF_CHECK(std::isfinite(out.x));
    NF_CHECK(std::isfinite(out.y));
    NF_CHECK(std::isfinite(out.z));
}

// ---------------------------------------------------------------------------
// Stage order — the property no per-stage test can see
// ---------------------------------------------------------------------------

NF_TEST(post_stack_grade_runs_before_the_tonemap) {
    // Grading in HDR and grading the tonemapped LDR result are different
    // pictures, and so are "before the operator" and "after the operator but
    // before gamma". Both wrong orders are computed here and both must differ
    // from what the chain produces — a version of this test that only compared
    // against the second one passed with the grade moved to the first.
    PostFxParams p{};
    p.grade.enabled = true;
    p.grade.contrast = 2.5f;
    p.grade.pivot = 1.0f;
    const Vec3 hdr{1.4f, 0.6f, 0.2f};
    const float exposure = 1.0f;
    const float inv_gamma = 1.0f / 2.2f;
    const auto gamma = [inv_gamma](Vec3 c) {
        return Vec3{std::pow(c.x, inv_gamma), std::pow(c.y, inv_gamma),
                    std::pow(c.z, inv_gamma)};
    };

    const Vec3 got = apply_post_chain(hdr, {}, {}, Vec2{0.5f, 0.5f}, exposure,
                                      TonemapMode::Exponential, p);

    // Wrong order A: grade immediately after the operator, before gamma.
    const Vec3 wrong_a =
        gamma(apply_color_grade(tonemap(hdr, exposure, TonemapMode::Exponential), p.grade));
    // Wrong order B: grade the finished LDR image.
    const Vec3 wrong_b = apply_color_grade(gamma(tonemap(hdr, exposure, TonemapMode::Exponential)),
                                           p.grade);

    NF_CHECK(std::abs(got.x - wrong_a.x) > 1e-3f);
    NF_CHECK(std::abs(got.x - wrong_b.x) > 1e-3f);
}

NF_TEST(post_stack_bloom_is_added_after_sharpening) {
    // The order inside the HDR block matters too: sharpen first, then the
    // bloom add, so the glow is not itself sharpened into a hard ring.
    PostFxParams p{};
    p.sharpen.enabled = true;
    p.sharpen.amount = 1.0f;
    p.bloom.enabled = true;
    p.bloom.intensity = 1.0f;
    const Vec3 hdr{0.5f, 0.5f, 0.5f};
    const Vec3 blurred{0.2f, 0.2f, 0.2f}; // sharpen pushes the centre up
    const Vec3 bloom{0.3f, 0.3f, 0.3f};

    const Vec3 got = apply_post_chain(hdr, blurred, bloom, Vec2{0.5f, 0.5f}, 1.0f,
                                      TonemapMode::Linear, p);
    // Linear mode + gamma, no saturation/vignette: (0.5 + 0.3) + 0.3 = 1.1 HDR.
    const float expected_hdr = unsharp_hdr(hdr, blurred, 1.0f).x + bloom.x;
    const float expected = std::pow(expected_hdr, 1.0f / 2.2f);
    NF_CHECK_NEAR(got.x, expected, 1e-5f);
}

NF_TEST(post_stack_bloom_intensity_zero_ignores_the_bloom_image) {
    PostFxParams p{};
    p.bloom.enabled = true;
    p.bloom.intensity = 0.0f;
    const Vec3 a = apply_post_chain(Vec3{0.5f, 0.5f, 0.5f}, {}, Vec3{0, 0, 0},
                                    Vec2{0.5f, 0.5f}, 1.0f, TonemapMode::Linear, p);
    const Vec3 b = apply_post_chain(Vec3{0.5f, 0.5f, 0.5f}, {}, Vec3{50, 50, 50},
                                    Vec2{0.5f, 0.5f}, 1.0f, TonemapMode::Linear, p);
    NF_CHECK_NEAR(a.x, b.x, 1e-6f);
}

// ---------------------------------------------------------------------------
// Setter
// ---------------------------------------------------------------------------

NF_TEST(post_stack_setter_roundtrips_the_whole_block) {
    Renderer3D renderer;
    PostFxParams p{};
    p.bloom.enabled = true;
    p.bloom.threshold = 1.75f;
    p.bloom.knee = 0.25f;
    p.bloom.intensity = 2.5f;
    p.bloom.radius = 1.5f;
    p.grade.enabled = true;
    p.grade.contrast = 1.2f;
    p.grade.pivot = 0.8f;
    p.grade.temperature = -0.4f;
    p.grade.tint = 0.3f;
    p.grade.gamma = 1.1f;
    p.sharpen.enabled = true;
    p.sharpen.amount = 0.6f;
    p.sharpen.radius = 2.0f;
    renderer.set_postfx(p);

    const PostFxParams& got = renderer.postfx();
    NF_CHECK(got.bloom.enabled);
    NF_CHECK_NEAR(got.bloom.threshold, 1.75f, 1e-6f);
    NF_CHECK_NEAR(got.bloom.knee, 0.25f, 1e-6f);
    NF_CHECK_NEAR(got.bloom.intensity, 2.5f, 1e-6f);
    NF_CHECK_NEAR(got.bloom.radius, 1.5f, 1e-6f);
    NF_CHECK(got.grade.enabled);
    NF_CHECK_NEAR(got.grade.contrast, 1.2f, 1e-6f);
    NF_CHECK_NEAR(got.grade.pivot, 0.8f, 1e-6f);
    NF_CHECK_NEAR(got.grade.temperature, -0.4f, 1e-6f);
    NF_CHECK_NEAR(got.grade.tint, 0.3f, 1e-6f);
    NF_CHECK_NEAR(got.grade.gamma, 1.1f, 1e-6f);
    NF_CHECK(got.sharpen.enabled);
    NF_CHECK_NEAR(got.sharpen.amount, 0.6f, 1e-6f);
    NF_CHECK_NEAR(got.sharpen.radius, 2.0f, 1e-6f);
}

NF_TEST(post_stack_bloom_active_requires_the_stage_and_the_pipeline) {
    // `enabled` alone is a request, not a fact: bloom_active() also requires
    // the pipeline to have come up, which is what lets a shader directory
    // without bloom_*.spv degrade to "no glow" instead of "no frame".
    Renderer3D renderer;
    NF_CHECK(!renderer.bloom_active()); // no device, no pipeline
    PostFxParams p{};
    p.bloom.enabled = true;
    renderer.set_postfx(p);
    NF_CHECK(!renderer.bloom_active());
}

// ---------------------------------------------------------------------------
// GPU: the chain is recorded, and its result reaches the pixels
// ---------------------------------------------------------------------------

NF_TEST(post_stack_renderer_records_no_bloom_passes_when_off) {
    const GpuFixture& f = require_gpu();
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    if (!renderer.init(*f.device, basic3d_shader_dir(), 64, 64)) {
        NF_CHECK(false);
        return;
    }
    std::vector<Pixel> pixels;
    PostFxParams p{}; // everything off
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, p, pixels));
    NF_CHECK_EQ(renderer.last_stats().bloom_levels_recorded, 0u);
    renderer.shutdown();
}

NF_TEST(post_stack_renderer_records_the_whole_chain_when_enabled) {
    const GpuFixture& f = require_gpu();
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    if (!renderer.init(*f.device, basic3d_shader_dir(), 64, 64)) {
        NF_CHECK(false);
        return;
    }
    std::vector<Pixel> pixels;
    PostFxParams p{};
    p.bloom.enabled = true;
    // A shader directory without bloom_*.spv is a legitimate configuration in
    // which the stage degrades to "no glow" — but a PASS here would claim the
    // chain was verified when nothing ran, so it is reported as a skip.
    renderer.set_postfx(p);
    if (!renderer.bloom_active()) {
        renderer.shutdown();
        NF_SKIP("bloom shaders unavailable in this shader directory");
    }
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, p, pixels));
    // One prefilter plus one downsample per remaining level.
    NF_CHECK_EQ(renderer.last_stats().bloom_levels_recorded, Renderer3D::kBloomLevels);
    renderer.shutdown();
}

NF_TEST(post_stack_bloom_changes_the_rendered_image) {
    // The end-to-end claim: not "the pass list has bloom in it" but "the glow
    // is in the pixels". The scene is empty, so the sky is the only source of
    // light — with a threshold of 0 everything above black survives into the
    // chain and the summed levels visibly lift the image.
    const GpuFixture& f = require_gpu();
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    if (!renderer.init(*f.device, basic3d_shader_dir(), 64, 64)) {
        NF_CHECK(false);
        return;
    }

    std::vector<Pixel> without;
    PostFxParams off{};
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, off, without));

    std::vector<Pixel> with;
    PostFxParams on{};
    on.bloom.enabled = true;
    on.bloom.threshold = 0.0f;
    on.bloom.knee = 0.0f;
    on.bloom.intensity = 1.5f;
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, on, with));

    if (!renderer.bloom_active()) {
        renderer.shutdown();
        NF_SKIP("bloom shaders unavailable in this shader directory");
    }

    NF_CHECK_EQ(without.size(), with.size());
    const u32 different = count_different_pixels(without, with, 4);
    // A meaningful fraction, not one stray pixel: the sky fills the frame, so
    // the glow must move a large part of it.
    NF_CHECK(different > without.size() / 4);

    // And it must brighten, not darken: bloom is an ADD. An implementation that
    // assigned instead of adding would also change every pixel.
    u64 sum_without = 0, sum_with = 0;
    for (usize i = 0; i < without.size(); ++i) {
        sum_without += u64(without[i].r) + without[i].g + without[i].b;
        sum_with += u64(with[i].r) + with[i].g + with[i].b;
    }
    NF_CHECK(sum_with > sum_without);
    renderer.shutdown();
}
