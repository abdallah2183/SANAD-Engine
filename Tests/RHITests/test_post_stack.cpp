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
#include <NF/ECS/ECS.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/Scene/Transform.hpp>

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

/// One cube at the origin, so the depth buffer has a real surface to reproject.
///
/// `render_empty_scene` cannot show motion blur: with no geometry every depth is
/// the far plane, the reprojection then reports zero velocity BY CONSTRUCTION,
/// and a test built on it would pass with the whole stage deleted.
bool render_cube_scene(rhi::IGraphicsDevice& dev, Renderer3D& renderer,
                       u32 width, u32 height, const PostFxParams& params,
                       const Camera& camera, std::vector<Pixel>& out_pixels) {
    renderer.set_postfx(params);

    MeshLibrary meshes;
    auto cube = StaticMesh::create_cube(1.0f);
    if (!cube || !cube->upload(dev)) return false;
    const StaticMeshHandle handle = meshes.add(std::move(cube));
    renderer.set_mesh_library(&meshes);

    ecs::World world;
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    world.add<MeshComponent>(e, MeshComponent{handle, kInvalidMaterialHandle, true});
    scene::propagate_transforms(world);

    RenderWorld render_world;
    nf::runtime::extract_render_objects(world, meshes, render_world);

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
    if (!renderer.render(*cmd, render_world, camera, *target)) return false;
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
    // The library is about to die, so leave the renderer with no stale pointer.
    renderer.set_mesh_library(nullptr);
    return true;
}

/// Uploads a `kLutSize^3` strip and hands back its view, so a test can bind a
/// REAL LUT rather than only exercising the "no LUT bound" path.
std::unique_ptr<rhi::Texture> upload_lut(rhi::IGraphicsDevice& dev, const std::vector<u8>& rgba,
                                        std::unique_ptr<rhi::TextureView>& out_view) {
    rhi::TextureDesc td{};
    td.width = rendering::kLutSize * rendering::kLutSize;
    td.height = rendering::kLutSize;
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
    auto tex = dev.create_texture(td);
    if (!tex) return nullptr;
    rhi::BufferDesc bd{};
    bd.size = rgba.size();
    bd.usage = rhi::BufferUsage::TransferSrc;
    bd.memory = rhi::MemoryUsage::CPUToGPU;
    auto staging = dev.create_buffer(bd);
    if (!staging) return nullptr;
    staging->update(rgba.data(), 0, rgba.size());
    if (auto upload = dev.create_upload_context()) {
        upload->copy_buffer_to_texture(*staging, *tex, 0, 0, 0, td.width, td.height);
        if (auto fence = upload->submit()) {
            fence->wait();
        }
    }
    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    if (!cmd || !fence) return nullptr;
    cmd->begin();
    cmd->transition_texture_for_sampling(*tex);
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(kGpuTimeoutNs)) return nullptr;
    rhi::TextureViewDesc vd{};
    vd.texture = tex.get();
    out_view = dev.create_texture_view(vd);
    if (!out_view) return nullptr;
    return tex;
}

/// A `kLutSize^3` strip where every texel is the same colour.
std::vector<u8> constant_lut(u8 r, u8 g, u8 b) {
    std::vector<u8> rgba(static_cast<usize>(rendering::kLutSize) * rendering::kLutSize *
                             rendering::kLutSize * 4u,
                         0);
    for (usize i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = 255;
    }
    return rgba;
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
    NF_CHECK(!p.lens.enabled);
    NF_CHECK_NEAR(p.lens.distortion, 0.0f, 1e-6f);
    NF_CHECK_NEAR(p.lens.chromatic_aberration, 0.0f, 1e-6f);
    NF_CHECK(!p.dof.enabled);
    NF_CHECK_NEAR(p.dof.max_radius, 6.0f, 1e-6f);
    NF_CHECK(p.dof.focus_range > 0.0f);
    NF_CHECK(!p.motion.enabled);
    NF_CHECK_NEAR(p.motion.intensity, 1.0f, 1e-6f);
    NF_CHECK(p.motion.max_length > 0.0f);
    NF_CHECK_NEAR(p.lut_strength, 0.0f, 1e-6f);
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

NF_TEST(post_stack_lens_neutral_is_identity) {
    // The golden-pixel contract for this stage: with the defaults, or with the
    // box unchecked and the sliders moved, every channel must read the SAME
    // coordinate. Anything else moves the whole image for a scene that never
    // asked for a lens.
    const LensParams off{};
    for (Vec2 uv : {Vec2{0.0f, 0.0f}, Vec2{0.5f, 0.5f}, Vec2{1.0f, 1.0f}, Vec2{0.13f, 0.87f}}) {
        for (int ch = 0; ch < 3; ++ch) {
            const Vec2 got = lens_sample_uv(uv, off, ch);
            NF_CHECK_NEAR(got.x, uv.x, 1e-7f);
            NF_CHECK_NEAR(got.y, uv.y, 1e-7f);
        }
    }
    LensParams unchecked{};
    unchecked.enabled = false;
    unchecked.distortion = 0.4f;
    unchecked.chromatic_aberration = 0.05f;
    for (int ch = 0; ch < 3; ++ch) {
        const Vec2 got = lens_sample_uv(Vec2{0.9f, 0.2f}, unchecked, ch);
        NF_CHECK_NEAR(got.x, 0.9f, 1e-7f);
        NF_CHECK_NEAR(got.y, 0.2f, 1e-7f);
    }
}

NF_TEST(post_stack_lens_distortion_moves_the_edges_out) {
    LensParams p{};
    p.enabled = true;
    p.distortion = 0.5f;

    // The centre is the fixed point of a radial warp: it must not move, or the
    // whole image would shift as the slider moves.
    const Vec2 centre = lens_sample_uv(Vec2{0.5f, 0.5f}, p, 1);
    NF_CHECK_NEAR(centre.x, 0.5f, 1e-7f);
    NF_CHECK_NEAR(centre.y, 0.5f, 1e-7f);

    // A positive coefficient reads FURTHER out at the edges, which is what
    // makes the image bow (barrel). Pincushion is the same test with the sign
    // flipped, so both directions are pinned.
    const Vec2 edge = lens_sample_uv(Vec2{1.0f, 0.5f}, p, 1);
    NF_CHECK(edge.x > 1.0f);
    NF_CHECK_NEAR(edge.y, 0.5f, 1e-7f);

    p.distortion = -0.5f;
    const Vec2 pinched = lens_sample_uv(Vec2{1.0f, 0.5f}, p, 1);
    NF_CHECK(pinched.x < 1.0f);

    // A zero coefficient is identity even with the stage enabled.
    LensParams zero = p;
    zero.distortion = 0.0f;
    const Vec2 same = lens_sample_uv(Vec2{1.0f, 0.25f}, zero, 1);
    NF_CHECK_NEAR(same.x, 1.0f, 1e-7f);
    NF_CHECK_NEAR(same.y, 0.25f, 1e-7f);
}

NF_TEST(post_stack_lens_chroma_splits_red_out_and_blue_in) {
    LensParams p{};
    p.enabled = true;
    p.chromatic_aberration = 0.02f;

    // Red outward, blue inward, green untouched. Green is the anchor: a
    // neutral grey pixel must stay neutral, so an implementation that moved all
    // three channels would fringe every grey surface in the scene.
    const Vec2 r = lens_sample_uv(Vec2{1.0f, 0.5f}, p, 0);
    const Vec2 g = lens_sample_uv(Vec2{1.0f, 0.5f}, p, 1);
    const Vec2 b = lens_sample_uv(Vec2{1.0f, 0.5f}, p, 2);
    NF_CHECK(r.x > 1.0f);
    NF_CHECK_NEAR(g.x, 1.0f, 1e-7f);
    NF_CHECK(b.x < 1.0f);

    // At the centre every channel agrees, so the fringing is zero there —
    // which is what makes it read as a lens rather than as a colour shift.
    for (int ch = 0; ch < 3; ++ch) {
        const Vec2 c = lens_sample_uv(Vec2{0.5f, 0.5f}, p, ch);
        NF_CHECK_NEAR(c.x, 0.5f, 1e-7f);
        NF_CHECK_NEAR(c.y, 0.5f, 1e-7f);
    }
}

NF_TEST(post_stack_lens_applies_distortion_before_chroma) {
    // The order the two compose in is observable, so it is pinned: the chroma
    // split is measured on the DISTORTED coordinate. Doing it the other way
    // round gives a different number, and this test would accept either if it
    // only checked "the value moved".
    LensParams p{};
    p.enabled = true;
    p.distortion = 0.1f;
    p.chromatic_aberration = 0.02f;

    const Vec2 got = lens_sample_uv(Vec2{1.0f, 0.5f}, p, 0); // red
    // distortion: c=(0.5,0), r2=0.25, scale=1+0.1*0.25=1.025 -> x=1.0125
    // then chroma on that: c=(0.5125,0), scale=1.02 -> x=0.5+0.5125*1.02
    const float want = 0.5f + 0.5125f * 1.02f;
    NF_CHECK_NEAR(got.x, want, 1e-5f);

    // The other order: chroma first (x=1.01), then distortion on that.
    const float other = 0.5f + (1.01f - 0.5f) * (1.0f + 0.1f * ((1.01f - 0.5f) * (1.01f - 0.5f)));
    NF_CHECK(std::fabs(got.x - other) > 1e-5f);
}

NF_TEST(post_stack_dof_coc_is_zero_inside_the_focus_band) {
    DofParams p{};
    p.enabled = true;
    p.focus_distance = 10.0f;
    p.focus_range = 2.0f;
    p.max_radius = 8.0f;

    // Exactly at the focus distance nothing is confused, and the ramp is
    // SYMMETRIC: a surface nearer than the focus plane blurs exactly as much as
    // one further away, which is what makes the focus plane a plane.
    NF_CHECK_NEAR(dof_coc(10.0f, p), 0.0f, 1e-6f);
    NF_CHECK_NEAR(dof_coc(9.0f, p), dof_coc(11.0f, p), 1e-6f);
    NF_CHECK_NEAR(dof_coc(6.0f, p), dof_coc(14.0f, p), 1e-6f);

    // The confusion rises linearly with distance from the focus plane: half way
    // to the full ramp is half the radius.
    NF_CHECK_NEAR(dof_coc(11.0f, p), 4.0f, 1e-5f);
    // At `focus_distance +/- focus_range` the ramp is SATURATED, and beyond it
    // it clamps — a blur that kept growing would spend samples on a radius the
    // frame cannot resolve. `focus_range` is therefore "the distance at which
    // the blur reaches its maximum", not the half-width of a sharp band.
    NF_CHECK_NEAR(dof_coc(12.0f, p), 8.0f, 1e-5f);
    NF_CHECK_NEAR(dof_coc(8.0f, p), 8.0f, 1e-5f);
    NF_CHECK_NEAR(dof_coc(1000.0f, p), 8.0f, 1e-5f);
}

NF_TEST(post_stack_dof_off_is_exactly_zero_confusion) {
    // The golden-pixel contract. Disabled, or enabled with a zero radius, every
    // depth must give a zero circle of confusion — which is what makes the
    // gather return its own centre and the stage free when unused.
    DofParams off{};
    for (float d : {0.0f, 1.0f, 10.0f, 1.0e30f}) {
        NF_CHECK_NEAR(dof_coc(d, off), 0.0f, 1e-6f);
    }
    DofParams zero_radius{};
    zero_radius.enabled = true;
    zero_radius.max_radius = 0.0f;
    for (float d : {0.0f, 1.0f, 10.0f, 1.0e30f}) {
        NF_CHECK_NEAR(dof_coc(d, zero_radius), 0.0f, 1e-6f);
    }
    // A zero focus range would divide by zero. It is refused rather than read as
    // "everything is sharp": a band of no width has no focus plane in it, and
    // treating that as a no-op would hide the author's mistake.
    DofParams zero_range{};
    zero_range.enabled = true;
    zero_range.focus_range = 0.0f;
    NF_CHECK_NEAR(dof_coc(10.0f, zero_range), 0.0f, 1e-6f);
    NF_CHECK_NEAR(dof_coc(1000.0f, zero_range), 0.0f, 1e-6f);
}

NF_TEST(post_stack_motion_smear_is_zero_for_a_still_camera) {
    // The golden contract, and the reason the stage costs nothing in a still
    // frame: reprojecting with the SAME view-projection lands on the same pixel.
    const Mat4 vp = make_camera(3.0f, 1.0f).view_projection;
    const Mat4 inv = vp.inverse();
    MotionBlurParams p{};
    p.enabled = true;
    p.intensity = 1.0f;
    p.max_length = 0.05f;
    for (float depth : {0.1f, 0.5f, 0.9f, 1.0f}) {
        for (Vec2 uv : {Vec2{0.5f, 0.5f}, Vec2{0.1f, 0.8f}}) {
            const Vec2 s = motion_smear(uv, depth, inv, vp, p);
            NF_CHECK_NEAR(s.x, 0.0f, 1e-5f);
            NF_CHECK_NEAR(s.y, 0.0f, 1e-5f);
        }
    }
    // Disabled is zero no matter how far the camera travelled.
    MotionBlurParams off{};
    const Mat4 moved = make_camera(9.0f, 1.0f).view_projection;
    const Vec2 s = motion_smear(Vec2{0.5f, 0.5f}, 0.5f, inv, moved, off);
    NF_CHECK_NEAR(s.x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(s.y, 0.0f, 1e-6f);
}

NF_TEST(post_stack_motion_smear_follows_the_reprojection) {
    const Camera a = make_camera(3.0f, 1.0f);
    const Camera b = make_camera(6.0f, 1.0f);
    MotionBlurParams p{};
    p.enabled = true;
    p.intensity = 1.0f;
    p.max_length = 10.0f; // effectively no cap, so the raw reprojection shows
    const Vec2 uv{0.9f, 0.5f};
    const Vec2 prev = motion_prev_uv(uv, 0.5f, a.view_projection.inverse(), b.view_projection);
    const Vec2 s = motion_smear(uv, 0.5f, a.view_projection.inverse(), b.view_projection, p);

    // The smear IS the reprojected difference: same direction, same magnitude
    // when the intensity is 1 and the cap does not bite.
    NF_CHECK(std::sqrt((prev.x - uv.x) * (prev.x - uv.x) + (prev.y - uv.y) * (prev.y - uv.y)) > 1e-4f);
    NF_CHECK_NEAR(s.x, uv.x - prev.x, 1e-6f);
    NF_CHECK_NEAR(s.y, uv.y - prev.y, 1e-6f);
    // The camera came CLOSER between the two frames — b (z=6) is the previous
    // one, a (z=3) the current — so a point off-centre moved OUTWARD, away from
    // the centre of the frame, and the smear points that way. Pinning the sign
    // is what catches an inverted reprojection, which would smear the frame the
    // wrong way while still "doing something".
    NF_CHECK(prev.x < uv.x);
    NF_CHECK(s.x > 0.0f);
    NF_CHECK_NEAR(s.y, 0.0f, 1e-4f); // the move is along the view axis
}

NF_TEST(post_stack_motion_smear_is_capped) {
    // A camera that teleports must smear by a bounded amount instead of
    // stretching one frame across the screen.
    const Camera a = make_camera(1.0f, 1.0f);
    const Camera b = make_camera(500.0f, 1.0f);
    MotionBlurParams p{};
    p.enabled = true;
    p.intensity = 4.0f;
    p.max_length = 0.02f;
    const Vec2 s = motion_smear(Vec2{0.95f, 0.5f}, 0.2f, a.view_projection.inverse(),
                                b.view_projection, p);
    const float len = std::sqrt(s.x * s.x + s.y * s.y);
    NF_CHECK(len <= 0.02f + 1e-6f);
    NF_CHECK(len > 0.0f);
}

NF_TEST(post_stack_lut_zero_strength_is_identity) {
    // The golden contract: at strength 0 the LUT is not read at all, whatever
    // is in the buffer — including a buffer of the wrong size.
    const std::vector<u8> lut = constant_lut(255, 0, 0);
    for (Vec3 c : {Vec3{0.2f, 0.5f, 0.8f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}}) {
        const Vec3 got = apply_color_lut(c, lut, 0.0f);
        NF_CHECK_NEAR(got.x, c.x, 1e-6f);
        NF_CHECK_NEAR(got.y, c.y, 1e-6f);
        NF_CHECK_NEAR(got.z, c.z, 1e-6f);
    }
    // A LUT of the wrong size is ignored rather than read out of bounds.
    const std::vector<u8> tiny{1, 2, 3, 4};
    const Vec3 got = apply_color_lut(Vec3{0.3f, 0.4f, 0.5f}, tiny, 1.0f);
    NF_CHECK_NEAR(got.x, 0.3f, 1e-6f);
    NF_CHECK_NEAR(got.y, 0.4f, 1e-6f);
    NF_CHECK_NEAR(got.z, 0.5f, 1e-6f);
}

NF_TEST(post_stack_lut_constant_lut_replaces_the_colour) {
    // A constant LUT is the case where the CPU's nearest-texel approximation and
    // the shader's bilinear fetch agree EXACTLY, so it is the one a mirror test
    // can assert equality on.
    const std::vector<u8> lut = constant_lut(255, 128, 0);
    for (Vec3 c : {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.4f, 0.6f, 0.9f}, Vec3{1.0f, 1.0f, 1.0f}}) {
        const Vec3 got = apply_color_lut(c, lut, 1.0f);
        NF_CHECK_NEAR(got.x, 1.0f, 2e-3f);
        NF_CHECK_NEAR(got.y, 128.0f / 255.0f, 2e-3f);
        NF_CHECK_NEAR(got.z, 0.0f, 2e-3f);
    }
    // The strength is a blend, so half strength lands half way.
    const Vec3 half = apply_color_lut(Vec3{0.2f, 0.2f, 0.2f}, lut, 0.5f);
    NF_CHECK_NEAR(half.x, 0.6f, 5e-3f); // 0.2 -> 1.0, half way
    NF_CHECK_NEAR(half.z, 0.1f, 5e-3f); // 0.2 -> 0.0, half way
}

NF_TEST(post_stack_lut_clamps_its_input) {
    // The cube only covers [0, 1], and an HDR pixel is routinely outside it, so
    // the lookup clamps rather than reading a texel that does not exist.
    const std::vector<u8> lut = constant_lut(10, 20, 30);
    const Vec3 got = apply_color_lut(Vec3{9.0f, -3.0f, 4.0f}, lut, 1.0f);
    NF_CHECK_NEAR(got.x, 10.0f / 255.0f, 2e-3f);
    NF_CHECK_NEAR(got.y, 20.0f / 255.0f, 2e-3f);
    NF_CHECK_NEAR(got.z, 30.0f / 255.0f, 2e-3f);
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

NF_TEST(post_stack_lens_changes_the_rendered_image) {
    // The end-to-end claim: not "the push block has two more floats" but "the
    // warp is in the pixels". The scene is empty and the sky fills the frame
    // with a vertical gradient, so a radial warp has to move a large part of
    // it.
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

    // Enabled with both coefficients at zero must be pixel-IDENTICAL. This is
    // the "an unchecked box and a slider at zero agree" contract, and it is the
    // one an implementation that always took the three-tap fetch path — or that
    // applied a warp unconditionally — would break.
    std::vector<Pixel> enabled_neutral;
    PostFxParams neutral{};
    neutral.lens.enabled = true;
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, neutral, enabled_neutral));
    NF_CHECK_EQ(without.size(), enabled_neutral.size());
    NF_CHECK(count_different_pixels(without, enabled_neutral, 0) == 0);

    // Distortion only, so the centre's invariance is not muddied by the chroma
    // split (which touches even a near-centre texel).
    std::vector<Pixel> warped;
    PostFxParams on{};
    on.lens.enabled = true;
    on.lens.distortion = 0.35f;
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, on, warped));

    NF_CHECK_EQ(without.size(), warped.size());
    const u32 different = count_different_pixels(without, warped, 4);
    NF_CHECK(different > without.size() / 8);

    // The centre is the warp's fixed point: it must survive. A centre that
    // moved would mean the image SHIFTED rather than bowed.
    const auto at = [](const std::vector<Pixel>& v, u32 w, u32 x, u32 y) -> const Pixel& {
        return v[static_cast<usize>(y) * w + x];
    };
    const Pixel& c0 = at(without, 64, 32, 32);
    const Pixel& c1 = at(warped, 64, 32, 32);
    NF_CHECK(std::abs(int(c0.r) - int(c1.r)) <= 2);
    NF_CHECK(std::abs(int(c0.g) - int(c1.g)) <= 2);
    NF_CHECK(std::abs(int(c0.b) - int(c1.b)) <= 2);

    // And the corner must move, or nothing was warped at all.
    const Pixel& e0 = at(without, 64, 1, 1);
    const Pixel& e1 = at(warped, 64, 1, 1);
    NF_CHECK(std::abs(int(e0.r) - int(e1.r)) > 2 || std::abs(int(e0.g) - int(e1.g)) > 2 ||
             std::abs(int(e0.b) - int(e1.b)) > 2);

    renderer.shutdown();
}

NF_TEST(post_stack_dof_changes_the_rendered_image) {
    // The end-to-end claim: not "the layout has two more bindings" but "the
    // blur is in the pixels, and it is driven by DEPTH".
    //
    // The scene is empty, so the depth buffer is the clear value everywhere —
    // which the shader reads as "the sky, very far away". That gives two
    // deterministic cases without needing geometry: focus the camera on that
    // distance and nothing is confused; focus it near and the whole frame is.
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

    // Enabled with a zero radius must be pixel-IDENTICAL: that is the stage's
    // own off switch, and the reason the renderer can disable it by folding the
    // flag into one value.
    std::vector<Pixel> enabled_neutral;
    PostFxParams neutral{};
    neutral.dof.enabled = true;
    neutral.dof.max_radius = 0.0f;
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, neutral, enabled_neutral));
    NF_CHECK_EQ(without.size(), enabled_neutral.size());
    NF_CHECK(count_different_pixels(without, enabled_neutral, 0) == 0);

    // Focused ON the far plane: every circle of confusion is zero, so the frame
    // must be identical again. This is the half that proves the blur is depth
    // driven rather than unconditional — an implementation that always blurred
    // would fail here while passing the "enabled with radius 0" case.
    std::vector<Pixel> focused_far;
    PostFxParams far_focus{};
    far_focus.dof.enabled = true;
    far_focus.dof.focus_distance = 1.0e30f; // the sky's reported distance
    far_focus.dof.focus_range = 1.0f;
    far_focus.dof.max_radius = 8.0f;
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, far_focus, focused_far));
    NF_CHECK_EQ(without.size(), focused_far.size());
    NF_CHECK(count_different_pixels(without, focused_far, 0) == 0);

    // Focused NEAR: the whole frame is at full confusion, so the sky gradient
    // and its horizon must visibly change.
    std::vector<Pixel> blurred;
    PostFxParams near_focus{};
    near_focus.dof.enabled = true;
    near_focus.dof.focus_distance = 10.0f;
    near_focus.dof.focus_range = 2.0f;
    near_focus.dof.max_radius = 12.0f;
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, near_focus, blurred));

    NF_CHECK_EQ(without.size(), blurred.size());
    const u32 different = count_different_pixels(without, blurred, 4);
    NF_CHECK(different > 0);

    renderer.shutdown();
}

NF_TEST(post_stack_motion_blur_changes_the_rendered_image) {
    // The end-to-end claim, and the one that needs GEOMETRY: with an empty scene
    // every depth is the far plane, the reprojection reports zero velocity by
    // construction, and a test built on it would pass with the stage deleted.
    const GpuFixture& f = require_gpu();
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    if (!renderer.init(*f.device, basic3d_shader_dir(), 64, 64)) {
        NF_CHECK(false);
        return;
    }

    const Camera near_cam = make_camera(3.0f, 1.0f);
    const Camera far_cam = make_camera(5.0f, 1.0f);

    PostFxParams off{};
    PostFxParams on{};
    on.motion.enabled = true;
    on.motion.intensity = 1.0f;
    on.motion.max_length = 0.05f;

    // A STILL camera must be pixel-identical with the stage enabled: the
    // reprojection lands on the same pixel, so there is nothing to smear.
    std::vector<Pixel> warm, still_a, still_b;
    NF_CHECK(render_cube_scene(*f.device, renderer, 64, 64, off, near_cam, warm));
    NF_CHECK(render_cube_scene(*f.device, renderer, 64, 64, off, near_cam, still_a));
    NF_CHECK(render_cube_scene(*f.device, renderer, 64, 64, on, near_cam, still_b));
    NF_CHECK_EQ(still_a.size(), still_b.size());
    NF_CHECK(count_different_pixels(still_a, still_b, 0) == 0);

    // A camera that MOVED. The baseline and the blurred frame must share the
    // same history, so the camera is returned to `near_cam` before each
    // `far_cam` render — otherwise the second one would see a still camera and
    // correctly do nothing.
    std::vector<Pixel> baseline, blurred;
    NF_CHECK(render_cube_scene(*f.device, renderer, 64, 64, off, near_cam, warm));
    NF_CHECK(render_cube_scene(*f.device, renderer, 64, 64, off, far_cam, baseline));
    NF_CHECK(render_cube_scene(*f.device, renderer, 64, 64, off, near_cam, warm));
    NF_CHECK(render_cube_scene(*f.device, renderer, 64, 64, on, far_cam, blurred));
    NF_CHECK_EQ(baseline.size(), blurred.size());
    NF_CHECK(count_different_pixels(baseline, blurred, 4) > 0);

    renderer.shutdown();
}

NF_TEST(post_stack_color_lut_changes_the_rendered_image) {
    // The end-to-end claim for the last §206 stage, and the one that has to
    // BIND A REAL TEXTURE: the "no LUT bound" path is already identity by
    // construction, so a test that only exercised it would pass with the whole
    // stage deleted.
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

    // An authored strength with NO LUT bound must be pixel-IDENTICAL. The slot
    // holds the renderer's 1x1 white texture, so without the push folding the
    // strength to 0 this would grade the whole frame toward white.
    std::vector<Pixel> strength_only;
    PostFxParams on{};
    on.lut_strength = 1.0f;
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, on, strength_only));
    NF_CHECK_EQ(without.size(), strength_only.size());
    NF_CHECK(count_different_pixels(without, strength_only, 0) == 0);

    // Bind a real LUT: constant red, so every input lands on red.
    std::unique_ptr<rhi::TextureView> lut_view;
    auto lut_tex = upload_lut(*f.device, constant_lut(255, 0, 0), lut_view);
    NF_CHECK(lut_tex != nullptr);
    NF_CHECK(lut_view != nullptr);
    if (!lut_tex || !lut_view) {
        renderer.shutdown();
        return;
    }
    renderer.set_color_lut(lut_view.get());
    NF_CHECK(renderer.has_color_lut());

    // Bound but at strength 0: still exactly identity.
    std::vector<Pixel> bound_off;
    PostFxParams bound_off_params{};
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, bound_off_params, bound_off));
    NF_CHECK(count_different_pixels(without, bound_off, 0) == 0);

    // Bound AND applied: the frame is graded toward the LUT.
    std::vector<Pixel> graded;
    NF_CHECK(render_empty_scene(*f.device, renderer, 64, 64, on, graded));
    NF_CHECK_EQ(without.size(), graded.size());
    NF_CHECK(count_different_pixels(without, graded, 4) > without.size() / 2);

    // And it is RED-dominant now, which a stage that merely "changed something"
    // would not be.
    u64 r_sum = 0, g_sum = 0;
    for (const Pixel& p : graded) {
        r_sum += p.r;
        g_sum += p.g;
    }
    NF_CHECK(r_sum > g_sum);

    renderer.clear_color_lut();
    NF_CHECK(!renderer.has_color_lut());
    renderer.shutdown();
}
