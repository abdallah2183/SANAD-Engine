// Tests/RHITests/test_render_scale.cpp — internal render scale (Phase 37)
//
// The phase has two claims and they are verified separately:
//
//   1. The CPU contract: the scale is clamped (NaN is the identity, not the
//      minimum), the internal size follows the output size and rounds both
//      axes independently, and a zero-sized output stays zero instead of
//      becoming a one-pixel image.
//   2. The GPU claim: a renderer sized 32x32 with scale 2 allocates 64x64
//      scene targets, still reports a 32x32 OUTPUT, and produces a frame in
//      that output target. Scale 0.5 does the same at 16x16. This is the part
//      that catches a renderer that only changes its reported width without
//      changing the textures — the failure that would make supersampling look
//      like it worked while rendering at native resolution.
//
// Tests skip (never silently pass) when the GPU or shaders are unavailable.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/RenderScale.hpp>
#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

namespace {

using nf::u32;
using nf::usize;
using nf::Vec3;
using namespace nf::test;
using namespace nf::rendering;
namespace ecs = nf::ecs;
namespace scene = nf::scene;
namespace rhi = nf::rhi;
namespace runtime = nf::runtime;
namespace fs = std::filesystem;

#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

constexpr u32 kOutputSize = 32;

fs::path basic3d_shader_dir() {
    fs::path p(NF_BASIC3D_SHADER_DIR);
    if (!p.empty() && fs::exists(p)) return p;
    return {};
}

Camera make_camera() {
    Camera cam{};
    cam.position = {0.0f, 0.0f, 3.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.aspect = 1.0f;
    cam.fov_y_rad = 60.0f * 3.14159265358979323846f / 180.0f;
    cam.near_plane = 0.1f;
    cam.far_plane = 100.0f;
    update_camera(cam);
    return cam;
}

/// Reads back an RGBA8 texture of exactly kOutputSize x kOutputSize. The output
/// target is deliberately this size for every scale: if the renderer wrote at
/// its internal size instead, this copy would be invalid or the readback would
/// not describe the caller's image.
bool read_output(rhi::IGraphicsDevice& dev, rhi::Texture& tex, std::vector<Pixel>& out) {
    rhi::BufferDesc rb_desc{};
    rb_desc.size = usize(kOutputSize) * kOutputSize * 4;
    rb_desc.usage = rhi::BufferUsage::TransferDst;
    rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = dev.create_buffer(rb_desc);
    if (!rb) return false;

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    if (!cmd || !fence) return false;
    cmd->begin();
    cmd->copy_texture_to_buffer(tex, *rb, 0, 0, kOutputSize, kOutputSize, 0);
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(kGpuTimeoutNs)) return false;
    dev.wait_idle();

    const auto* px = static_cast<const Pixel*>(rb->map());
    if (px == nullptr) return false;
    out.assign(px, px + usize(kOutputSize) * kOutputSize);
    rb->unmap();
    return true;
}

usize non_black_pixels(const std::vector<Pixel>& px) {
    return static_cast<usize>(std::count_if(px.begin(), px.end(), [](const Pixel& p) {
        return p.r != 0 || p.g != 0 || p.b != 0;
    }));
}

} // namespace

// ---------------------------------------------------------------------------
// CPU contract: sanitizing and size arithmetic
// ---------------------------------------------------------------------------

NF_TEST(render_scale_is_the_identity_at_one_and_inside_the_bounds) {
    NF_CHECK_NEAR(sanitize_render_scale(1.0f), 1.0f, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(0.5f), 0.5f, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(1.5f), 1.5f, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(kMinRenderScale), kMinRenderScale, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(kMaxRenderScale), kMaxRenderScale, 1e-6f);
}

NF_TEST(render_scale_clamps_out_of_range_values) {
    NF_CHECK_NEAR(sanitize_render_scale(0.0f), kMinRenderScale, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(-3.0f), kMinRenderScale, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(0.01f), kMinRenderScale, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(3.0f), kMaxRenderScale, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(100.0f), kMaxRenderScale, 1e-6f);
}

NF_TEST(render_scale_treats_non_finite_values_as_the_identity) {
    // A corrupted float must not turn the whole frame into a 1/16-resolution
    // blur. The minimum is a legal authored value; NaN is not an authored
    // value at all, so the only honest mapping is "don't change anything".
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    NF_CHECK_NEAR(sanitize_render_scale(nan), 1.0f, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(inf), 1.0f, 1e-6f);
    NF_CHECK_NEAR(sanitize_render_scale(-inf), 1.0f, 1e-6f);
}

NF_TEST(scaled_render_size_multiplies_both_axes) {
    const RenderTargetSize full = scaled_render_size(1920, 1080, 1.0f);
    NF_CHECK_EQ(full.width, 1920u);
    NF_CHECK_EQ(full.height, 1080u);

    const RenderTargetSize half = scaled_render_size(1920, 1080, 0.5f);
    NF_CHECK_EQ(half.width, 960u);
    NF_CHECK_EQ(half.height, 540u);

    const RenderTargetSize super = scaled_render_size(1920, 1080, 2.0f);
    NF_CHECK_EQ(super.width, 3840u);
    NF_CHECK_EQ(super.height, 2160u);

    const RenderTargetSize fractional = scaled_render_size(1920, 1080, 1.5f);
    NF_CHECK_EQ(fractional.width, 2880u);
    NF_CHECK_EQ(fractional.height, 1620u);
}

NF_TEST(scaled_render_size_rounds_each_axis_and_never_returns_zero_for_a_live_output) {
    // 50.5 and 49.5 are exact ties; llround's documented half-away-from-zero
    // rule makes the result 51/50 rather than platform-dependent.
    const RenderTargetSize odd = scaled_render_size(101, 99, 0.5f);
    NF_CHECK_EQ(odd.width, 51u);
    NF_CHECK_EQ(odd.height, 50u);

    // A minimum-size output at the minimum scale would round to 0 without the
    // one-texel floor, and a zero-sized attachment is not a valid texture.
    const RenderTargetSize tiny = scaled_render_size(1, 1, kMinRenderScale);
    NF_CHECK_EQ(tiny.width, 1u);
    NF_CHECK_EQ(tiny.height, 1u);

    const RenderTargetSize tiny_super = scaled_render_size(1, 1, kMaxRenderScale);
    NF_CHECK_EQ(tiny_super.width, 2u);
    NF_CHECK_EQ(tiny_super.height, 2u);
}

NF_TEST(scaled_render_size_leaves_an_unsized_output_unsized) {
    // "0" means the viewport has not been laid out yet, not "one pixel".
    const RenderTargetSize no_width = scaled_render_size(0, 720, 2.0f);
    NF_CHECK_EQ(no_width.width, 0u);
    NF_CHECK_EQ(no_width.height, 0u);
    const RenderTargetSize no_height = scaled_render_size(1280, 0, 2.0f);
    NF_CHECK_EQ(no_height.width, 0u);
    NF_CHECK_EQ(no_height.height, 0u);
}

NF_TEST(scaled_render_size_clamps_the_internal_axis) {
    // 16k is the hard ceiling; a larger output plus 2x scale must not ask for
    // an image size the arithmetic (or a device) cannot represent.
    const RenderTargetSize capped = scaled_render_size(20000, 9000, 2.0f);
    NF_CHECK_EQ(capped.width, kMaxRenderTargetDimension);
    NF_CHECK_EQ(capped.height, kMaxRenderTargetDimension);
}

// ---------------------------------------------------------------------------
// GPU: the internal targets change, the output does not
// ---------------------------------------------------------------------------

NF_TEST(render_scale_gpu_changes_targets_but_not_the_output) {
    const auto& fixture = require_gpu();
    rhi::IGraphicsDevice& dev = *fixture.device;
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    if (!renderer.init(dev, basic3d_shader_dir(), kOutputSize, kOutputSize)) {
        NF_SKIP("Renderer3D init failed on this device");
    }
    MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);

    const StaticMeshHandle cube = meshes.add(StaticMesh::create_cube(3.0f));
    if (!meshes.upload_all(dev)) {
        renderer.shutdown();
        NF_SKIP("mesh upload failed on this device");
    }
    PBRMaterialParams params{};
    params.base_color[0] = 0.8f;
    params.base_color[1] = 0.8f;
    params.base_color[2] = 0.8f;
    params.roughness = 0.8f;
    params.ao = 1.0f;
    const MaterialHandle material =
        renderer.materials().create_instance(*renderer.gbuffer_material(), params, "scale-wall");

    ecs::World world;
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    world.add<MeshComponent>(e, MeshComponent{cube, material, true});
    scene::propagate_transforms(world);

    RenderWorld rw;
    runtime::extract_render_objects(world, meshes, rw);

    rhi::TextureDesc target_desc{};
    target_desc.width = kOutputSize;
    target_desc.height = kOutputSize;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled |
                        rhi::ImageUsage::TransferSrc;
    auto target = dev.create_texture(target_desc);
    NF_CHECK(target != nullptr);
    if (!target) {
        renderer.shutdown();
        return;
    }

    auto render_once = [&]() {
        auto cmd = dev.create_command_buffer();
        auto fence = dev.create_fence(false);
        if (!cmd || !fence) return false;
        cmd->begin();
        const bool rendered = renderer.render(*cmd, rw, make_camera(), *target);
        cmd->end();
        if (!rendered) return false;
        dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
        if (!fence->wait(kGpuTimeoutNs)) return false;
        dev.wait_idle();
        return true;
    };

    // Native: output and internal agree.
    NF_CHECK_EQ(renderer.width(), kOutputSize);
    NF_CHECK_EQ(renderer.height(), kOutputSize);
    NF_CHECK_EQ(renderer.render_width(), kOutputSize);
    NF_CHECK_EQ(renderer.render_height(), kOutputSize);
    NF_CHECK_NEAR(renderer.render_scale(), 1.0f, 1e-6f);
    NF_CHECK(render_once());
    std::vector<Pixel> native_pixels;
    NF_CHECK(read_output(dev, *target, native_pixels));
    NF_CHECK(non_black_pixels(native_pixels) > 0u);

    // 2x: the graph targets are twice as large in each axis, while width() and
    // the caller's target stay 32. A renderer that merely reported 64 would
    // fail the gbuffer_target() checks below.
    renderer.set_render_scale(2.0f);
    NF_CHECK_NEAR(renderer.render_scale(), 2.0f, 1e-6f);
    NF_CHECK_EQ(renderer.width(), kOutputSize);
    NF_CHECK_EQ(renderer.height(), kOutputSize);
    NF_CHECK_EQ(renderer.render_width(), 2 * kOutputSize);
    NF_CHECK_EQ(renderer.render_height(), 2 * kOutputSize);
    NF_CHECK(renderer.gbuffer_target(0) != nullptr);
    if (renderer.gbuffer_target(0) != nullptr) {
        NF_CHECK_EQ(renderer.gbuffer_target(0)->width(), 2 * kOutputSize);
        NF_CHECK_EQ(renderer.gbuffer_target(0)->height(), 2 * kOutputSize);
    }
    NF_CHECK(renderer.depth_target() != nullptr);
    if (renderer.depth_target() != nullptr) {
        NF_CHECK_EQ(renderer.depth_target()->width(), 2 * kOutputSize);
    }
    NF_CHECK(render_once());
    std::vector<Pixel> super_pixels;
    NF_CHECK(read_output(dev, *target, super_pixels));
    NF_CHECK(non_black_pixels(super_pixels) > 0u);

    // Half: the same output, a 16x16 internal frame. This is the dynamic-
    // resolution direction, not just the quality direction.
    renderer.set_render_scale(0.5f);
    NF_CHECK_EQ(renderer.render_width(), kOutputSize / 2);
    NF_CHECK_EQ(renderer.render_height(), kOutputSize / 2);
    NF_CHECK(render_once());
    std::vector<Pixel> half_pixels;
    NF_CHECK(read_output(dev, *target, half_pixels));
    NF_CHECK(non_black_pixels(half_pixels) > 0u);

    // NaN is mapped to the identity, so this rebuilds at native size again.
    renderer.set_render_scale(std::numeric_limits<float>::quiet_NaN());
    NF_CHECK_NEAR(renderer.render_scale(), 1.0f, 1e-6f);
    NF_CHECK_EQ(renderer.render_width(), kOutputSize);

    renderer.shutdown();
}
