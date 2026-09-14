// Tests/RHITests/test_rhi_triangle.cpp
//
// Proves the RHI actually rasterizes, not merely that it does not crash.
//
// The strategy: render the sample's triangle into an offscreen color
// attachment, copy that attachment into a host-visible buffer, read the pixels
// back on the CPU and assert they contain the triangle's vertex colors. A test
// that only checks "no validation errors" would still pass if the pipeline
// silently drew nothing.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>

#include <NF/Core/Logger.hpp>
#include <NF/RHI/RHI.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace {

using namespace nf;
using namespace nf::test;

/// Directory holding the compiled SPIR-V, injected by CMake.
#ifndef NF_RHI_TEST_SHADER_DIR
    #define NF_RHI_TEST_SHADER_DIR ""
#endif

} // namespace

NF_TEST(rhi_offscreen_triangle_writes_colored_pixels) {
    // require_gpu() marks the test SKIPPED when no device exists. A bare
    // `gpu()` + `return` would be recorded as a PASS, so a GPU-less runner
    // would report the whole suite green while verifying nothing.
    const GpuFixture& fixture = require_gpu();

    auto& device = *fixture.device;

    // --- Shaders ----------------------------------------------------------
    const std::filesystem::path shader_dir = NF_RHI_TEST_SHADER_DIR;
    NF_CHECK(!shader_dir.empty());

    const auto vert_code = load_spirv(shader_dir / "triangle_vert.spv");
    const auto frag_code = load_spirv(shader_dir / "triangle_frag.spv");
    NF_CHECK(!vert_code.empty());
    NF_CHECK(!frag_code.empty());

    rhi::ShaderModuleDesc vert_desc{ vert_code, rhi::ShaderStage::Vertex };
    rhi::ShaderModuleDesc frag_desc{ frag_code, rhi::ShaderStage::Fragment };

    auto vs = device.create_shader_module(vert_desc);
    auto fs = device.create_shader_module(frag_desc);
    NF_CHECK(vs != nullptr);
    NF_CHECK(fs != nullptr);

    // --- Offscreen color target ------------------------------------------
    constexpr u32 kWidth = 64;
    constexpr u32 kHeight = 64;

    rhi::TextureDesc target_desc;
    target_desc.width = kWidth;
    target_desc.height = kHeight;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;

    auto target = device.create_texture(target_desc);
    NF_CHECK(target != nullptr);
    NF_CHECK_EQ(target->width(), kWidth);
    NF_CHECK_EQ(target->height(), kHeight);

    // --- Render pass ------------------------------------------------------
    rhi::ColorAttachment color_attachment;
    color_attachment.format = target->format();
    color_attachment.blend_enabled = false;

    const std::array<rhi::ColorAttachment, 1> attachments{ color_attachment };

    rhi::RenderPassDesc rp_desc;
    rp_desc.color_attachments = std::span<const rhi::ColorAttachment>(attachments);
    rp_desc.has_depth = false;
    rp_desc.present_source = false; // offscreen: leave the image renderable

    auto render_pass = device.create_render_pass(rp_desc);
    NF_CHECK(render_pass != nullptr);
    NF_CHECK_EQ(render_pass->color_attachment_count(), 1u);
    NF_CHECK(!render_pass->has_depth());

    // --- Pipeline ---------------------------------------------------------
    rhi::PipelineDesc pipeline_desc;
    pipeline_desc.vs = vs.get();
    pipeline_desc.fs = fs.get();
    pipeline_desc.topology = rhi::PrimitiveTopology::TriangleList;
    pipeline_desc.rasterizer.cull_mode = rhi::CullMode::None;
    pipeline_desc.depth.test_enabled = false;
    pipeline_desc.depth.write_enabled = false;
    pipeline_desc.render_pass = render_pass.get();
    pipeline_desc.vertex_layout.stride = 0; // vertices come from gl_VertexIndex

    auto pipeline = device.create_pipeline(pipeline_desc);
    NF_CHECK(pipeline != nullptr);

    // --- Framebuffer -------------------------------------------------------
    const std::array<rhi::Texture*, 1> fb_attachments{ target.get() };

    auto framebuffer = device.create_framebuffer(
        *render_pass, std::span<rhi::Texture* const>(fb_attachments), nullptr);
    NF_CHECK(framebuffer != nullptr);

    // --- Readback buffer ---------------------------------------------------
    constexpr usize kPixelBytes = kWidth * kHeight * 4;

    rhi::BufferDesc readback_desc;
    readback_desc.size = kPixelBytes;
    readback_desc.usage = rhi::BufferUsage::TransferDst;
    readback_desc.memory = rhi::MemoryUsage::GPUToCPU;

    auto readback = device.create_buffer(readback_desc);
    NF_CHECK(readback != nullptr);

    // --- Record: draw, then copy the result back ---------------------------
    auto cmd = device.create_command_buffer();
    NF_CHECK(cmd != nullptr);

    auto fence = device.create_fence(false);
    NF_CHECK(fence != nullptr);

    // Distinct from every vertex color so a missing triangle is unambiguous.
    const std::array<rhi::ClearValue, 1> clear_values{
        rhi::ClearValue{ 0.0f, 0.0f, 0.0f, 1.0f }
    };

    cmd->begin();
    cmd->begin_render_pass(*render_pass, *framebuffer,
                           std::span<const rhi::ClearValue>(clear_values));
    cmd->bind_pipeline(*pipeline);
    cmd->set_viewport(0, 0, kWidth, kHeight);
    cmd->set_scissor(0, 0, kWidth, kHeight);
    cmd->draw(3);
    cmd->end_render_pass();

    cmd->copy_texture_to_buffer(*target, *readback, 0, 0, kWidth, kHeight, 0);
    cmd->end();

    device.submit(*cmd, rhi::SubmitInfo{ .signal_fence = fence.get() });
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    device.wait_idle();

    // --- Verify the pixels -------------------------------------------------
    Pixel* pixels = static_cast<Pixel*>(readback->map());
    NF_CHECK(pixels != nullptr);

    u32 non_black = 0;
    u32 red_dominant = 0;
    u32 green_dominant = 0;
    u32 blue_dominant = 0;

    for (usize i = 0; i < kWidth * kHeight; ++i) {
        const Pixel p = pixels[i];
        if (p.r == 0 && p.g == 0 && p.b == 0) continue;
        ++non_black;

        if (p.r > p.g && p.r > p.b) ++red_dominant;
        else if (p.g > p.r && p.g > p.b) ++green_dominant;
        else if (p.b > p.r && p.b > p.g) ++blue_dominant;
    }

    readback->unmap();

    // The triangle covers roughly a quarter of a 64x64 target, so anything
    // near zero means the draw produced nothing.
    NF_CHECK(non_black > 200);

    // Each of the three vertex colors must survive rasterization and
    // interpolation somewhere in the image. This is what distinguishes
    // "a triangle was drawn" from "something was scribbled on the target".
    NF_CHECK(red_dominant > 20);
    NF_CHECK(green_dominant > 20);
    NF_CHECK(blue_dominant > 20);

    NF_LOG_INFO(LogCategory::RHI,
        "Offscreen triangle verified: {} lit pixels (R:{} G:{} B:{})",
        non_black, red_dominant, green_dominant, blue_dominant);
}

NF_TEST(rhi_clear_fills_entire_attachment) {
    const GpuFixture& fixture = require_gpu();

    auto& device = *fixture.device;

    constexpr u32 kWidth = 32;
    constexpr u32 kHeight = 32;

    rhi::TextureDesc target_desc;
    target_desc.width = kWidth;
    target_desc.height = kHeight;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;

    auto target = device.create_texture(target_desc);
    NF_CHECK(target != nullptr);

    rhi::ColorAttachment color_attachment;
    color_attachment.format = target->format();

    const std::array<rhi::ColorAttachment, 1> attachments{ color_attachment };

    rhi::RenderPassDesc rp_desc;
    rp_desc.color_attachments = std::span<const rhi::ColorAttachment>(attachments);
    rp_desc.present_source = false;

    auto render_pass = device.create_render_pass(rp_desc);
    NF_CHECK(render_pass != nullptr);

    const std::array<rhi::Texture*, 1> fb_attachments{ target.get() };
    auto framebuffer = device.create_framebuffer(
        *render_pass, std::span<rhi::Texture* const>(fb_attachments), nullptr);
    NF_CHECK(framebuffer != nullptr);

    rhi::BufferDesc readback_desc;
    readback_desc.size = static_cast<usize>(kWidth) * kHeight * 4;
    readback_desc.usage = rhi::BufferUsage::TransferDst;
    readback_desc.memory = rhi::MemoryUsage::GPUToCPU;

    auto readback = device.create_buffer(readback_desc);
    NF_CHECK(readback != nullptr);

    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    NF_CHECK(cmd != nullptr);
    NF_CHECK(fence != nullptr);

    // A clear value that is trivially distinguishable from "untouched".
    const std::array<rhi::ClearValue, 1> clear_values{
        rhi::ClearValue{ 0.25f, 0.5f, 0.75f, 1.0f }
    };

    cmd->begin();
    cmd->begin_render_pass(*render_pass, *framebuffer,
                           std::span<const rhi::ClearValue>(clear_values));
    cmd->end_render_pass();
    cmd->copy_texture_to_buffer(*target, *readback, 0, 0, kWidth, kHeight, 0);
    cmd->end();

    device.submit(*cmd, rhi::SubmitInfo{ .signal_fence = fence.get() });
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    device.wait_idle();

    auto* pixels = static_cast<Pixel*>(readback->map());
    NF_CHECK(pixels != nullptr);

    // Every pixel must match the clear color — including the corners, which is
    // what catches a scissor or render-area that does not cover the target.
    for (usize i = 0; i < static_cast<usize>(kWidth) * kHeight; ++i) {
        const Pixel p = pixels[i];
        // float->unorm8 rounding differs by one LSB between drivers
        // (0.5*255 = 127.5 truncates to 127 on some, rounds to 128 on
        // others), so compare with a one-LSB tolerance.
        NF_CHECK(std::abs(int(p.r) - 64) <= 1);  // 0.25 * 255 = 63.75
        NF_CHECK(std::abs(int(p.g) - 128) <= 1); // 0.50 * 255 = 127.5
        NF_CHECK(std::abs(int(p.b) - 191) <= 1); // 0.75 * 255 = 191.25
        NF_CHECK_EQ(p.a, 255);
    }

    readback->unmap();
}

NF_TEST(rhi_rejects_framebuffer_that_does_not_match_render_pass) {
    const GpuFixture& fixture = require_gpu();

    auto& device = *fixture.device;

    // Two attachments in the pass, one in the framebuffer — the backend must
    // refuse rather than build an incompatible framebuffer that blows up later
    // at submit time.
    rhi::TextureDesc desc;
    desc.width = 16;
    desc.height = 16;
    desc.format = rhi::Format::R8G8B8A8_UNorm;
    desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;

    auto a = device.create_texture(desc);
    auto b = device.create_texture(desc);
    NF_CHECK(a != nullptr);
    NF_CHECK(b != nullptr);

    rhi::ColorAttachment attachment;
    attachment.format = desc.format;

    const std::array<rhi::ColorAttachment, 2> two{ attachment, attachment };

    rhi::RenderPassDesc rp_desc;
    rp_desc.color_attachments = std::span<const rhi::ColorAttachment>(two);
    rp_desc.present_source = false;

    auto two_attachment_pass = device.create_render_pass(rp_desc);
    NF_CHECK(two_attachment_pass != nullptr);

    const std::array<rhi::Texture*, 1> one{ a.get() };
    auto mismatched = device.create_framebuffer(
        *two_attachment_pass, std::span<rhi::Texture* const>(one), nullptr);

    NF_CHECK(mismatched == nullptr);
}
