// Tests/RHITests/test_rhi_quad.cpp
//
// Proves the resource-binding path actually works end to end:
// CPU data → staging buffer → GPU vertex/index buffers, CPU pixels → staging
// buffer → GPU texture, both bound through a descriptor set, drawn with
// draw_indexed, and read back off the GPU to verify the sampled texels.
//
// The texture is a 4-quadrant checker with a distinct solid color in each
// quadrant (red / green / blue / yellow). Sampling it onto a quad that covers
// the whole target means each screen quadrant must come back matching its
// texture quadrant. That is what makes this a real test: if the upload lands
// in the wrong place, the UVs are flipped, the sampler is broken, or the
// descriptor set never got bound, the quadrant colors come back wrong or the
// attachment stays at the clear color.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>

#include <NF/Core/Logger.hpp>
#include <NF/RHI/RHI.hpp>

#include <array>
#include <cstddef>   // offsetof
#include <cstring>   // std::memcpy / std::memcmp
#include <filesystem>
#include <span>
#include <vector>

namespace {

using namespace nf;
using namespace nf::test;

/// Directory holding the compiled textured-quad SPIR-V, injected by CMake.
#ifndef NF_RHI_TEST_QUAD_SHADER_DIR
    #define NF_RHI_TEST_QUAD_SHADER_DIR ""
#endif

// --- Geometry -------------------------------------------------------------
//
// A quad covering most of the target, with UVs in the normal Vulkan
// orientation: (0,0) top-left, (1,1) bottom-right.

struct Vertex {
    float x, y;
    float u, v;
};

constexpr float kQuadExtent = 0.8f;

/// Four corners, CCW. UVs match so the texture is drawn right-side up.
constexpr std::array<Vertex, 4> kVertices{{
    { -kQuadExtent, -kQuadExtent, 0.0f, 0.0f }, // 0: bottom-left
    {  kQuadExtent, -kQuadExtent, 1.0f, 0.0f }, // 1: bottom-right
    {  kQuadExtent,  kQuadExtent, 1.0f, 1.0f }, // 2: top-right
    { -kQuadExtent,  kQuadExtent, 0.0f, 1.0f }, // 3: top-left
}};

/// Two triangles forming the quad, wound CCW to match the rasterizer.
constexpr std::array<u32, 6> kIndices{ 0, 1, 2, 2, 3, 0 };

// --- Texture --------------------------------------------------------------
//
// 64x64, four solid quadrants. Nearest filtering is used so each sampled texel
// is exactly one of these colors — no interpolation to reason about.

constexpr u32 kTexSize = 64;

constexpr std::array<Pixel, 4> kQuadrantColors{{
    { 255,   0,   0, 255 }, // top-left     — red
    {   0, 255,   0, 255 }, // top-right    — green
    {   0,   0, 255, 255 }, // bottom-left  — blue
    { 255, 255,   0, 255 }, // bottom-right — yellow
}};

std::vector<u8> make_checker_texture() {
    std::vector<u8> data(kTexSize * kTexSize * 4);

    for (u32 y = 0; y < kTexSize; ++y) {
        for (u32 x = 0; x < kTexSize; ++x) {
            const bool left  = x < (kTexSize / 2);
            const bool top   = y < (kTexSize / 2);

            // Vulkan texel row 0 is the top row, so `top` (y < half) is the
            // visually upper half and `left` (x < half) the left columns.
            usize quadrant = 0;
            if (top && left)    quadrant = 0; // top-left
            else if (top)       quadrant = 1; // top-right
            else if (left)      quadrant = 2; // bottom-left
            else                quadrant = 3; // bottom-right

            const Pixel c = kQuadrantColors[quadrant];
            const usize offset = (static_cast<usize>(y) * kTexSize + x) * 4;
            data[offset + 0] = c.r;
            data[offset + 1] = c.g;
            data[offset + 2] = c.b;
            data[offset + 3] = c.a;
        }
    }

    return data;
}

/// Returns true when two pixels are close enough to count as the same color.
/// Nearest sampling of an unorm8 texture should be exact, but a small
/// tolerance keeps the test robust against format conversion rounding.
bool pixel_near(const Pixel& a, const Pixel& b, u8 tolerance = 8) {
    const auto near = [tolerance](u8 x, u8 y) {
        const int d = static_cast<int>(x) - static_cast<int>(y);
        return d >= -static_cast<int>(tolerance) && d <= static_cast<int>(tolerance);
    };
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b);
}

/// Builds the render pass + pipeline + framebuffer for a plain offscreen
/// RGBA8 target. Shared by the quad tests so the only thing that varies is
/// what gets drawn.
struct OffscreenTarget {
    static constexpr u32 kWidth = 128;
    static constexpr u32 kHeight = 128;

    std::unique_ptr<rhi::Texture> texture;
    std::unique_ptr<rhi::RenderPass> render_pass;
    std::unique_ptr<rhi::Framebuffer> framebuffer;

    static std::unique_ptr<rhi::RenderPass> make_pass(rhi::IGraphicsDevice& device,
                                                      rhi::Format format) {
        rhi::ColorAttachment attachment;
        attachment.format = format;
        attachment.blend_enabled = false;

        const std::array<rhi::ColorAttachment, 1> attachments{ attachment };

        rhi::RenderPassDesc desc;
        desc.color_attachments = std::span<const rhi::ColorAttachment>(attachments);
        desc.has_depth = false;
        desc.present_source = false; // offscreen: leave the image readable

        return device.create_render_pass(desc);
    }
};

} // namespace

NF_TEST(rhi_textured_quad_samples_uploaded_texture) {
    // require_gpu() marks the test SKIPPED when no device exists; a bare
    // `gpu()` + `return` would be recorded as a PASS.
    const GpuFixture& fixture = require_gpu();

    auto& device = *fixture.device;

    // --- Shaders ----------------------------------------------------------
    const std::filesystem::path shader_dir = NF_RHI_TEST_QUAD_SHADER_DIR;
    NF_CHECK(!shader_dir.empty());

    const auto vert_code = load_spirv(shader_dir / "textured_quad_vert.spv");
    const auto frag_code = load_spirv(shader_dir / "textured_quad_frag.spv");
    NF_CHECK(!vert_code.empty());
    NF_CHECK(!frag_code.empty());

    auto vs = device.create_shader_module(
        rhi::ShaderModuleDesc{ vert_code, rhi::ShaderStage::Vertex });
    auto fs = device.create_shader_module(
        rhi::ShaderModuleDesc{ frag_code, rhi::ShaderStage::Fragment });
    NF_CHECK(vs != nullptr);
    NF_CHECK(fs != nullptr);

    // --- Vertex buffer: CPU → staging → GPU ------------------------------
    constexpr usize kVertexBytes = sizeof(Vertex) * kVertices.size();

    rhi::BufferDesc vertex_desc;
    vertex_desc.size = kVertexBytes;
    vertex_desc.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
    vertex_desc.memory = rhi::MemoryUsage::GPUOnly; // device-local, copied into

    auto vertex_buffer = device.create_buffer(vertex_desc);
    NF_CHECK(vertex_buffer != nullptr);
    NF_CHECK_EQ(vertex_buffer->size(), kVertexBytes);

    // The RHI's update() on a GPU-only buffer stages the upload internally.
    vertex_buffer->update(kVertices.data(), 0, kVertexBytes);

    // --- Index buffer -----------------------------------------------------
    constexpr usize kIndexBytes = sizeof(u32) * kIndices.size();

    rhi::BufferDesc index_desc;
    index_desc.size = kIndexBytes;
    index_desc.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
    index_desc.memory = rhi::MemoryUsage::GPUOnly;

    auto index_buffer = device.create_buffer(index_desc);
    NF_CHECK(index_buffer != nullptr);
    index_buffer->update(kIndices.data(), 0, kIndexBytes);

    // --- Texture: CPU pixels → staging → GPU image ------------------------
    const std::vector<u8> texel_data = make_checker_texture();

    rhi::TextureDesc tex_desc;
    tex_desc.width = kTexSize;
    tex_desc.height = kTexSize;
    tex_desc.format = rhi::Format::R8G8B8A8_UNorm;
    tex_desc.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;

    auto texture = device.create_texture(tex_desc);
    NF_CHECK(texture != nullptr);
    NF_CHECK_EQ(texture->width(), kTexSize);
    NF_CHECK_EQ(texture->height(), kTexSize);

    // Upload through an explicit staging buffer so the test exercises the
    // same path the asset pipeline will use.
    rhi::BufferDesc staging_desc;
    staging_desc.size = texel_data.size();
    staging_desc.usage = rhi::BufferUsage::TransferSrc;
    staging_desc.memory = rhi::MemoryUsage::CPUToGPU;

    auto staging = device.create_buffer(staging_desc);
    NF_CHECK(staging != nullptr);

    void* mapped = staging->map();
    NF_CHECK(mapped != nullptr);
    std::memcpy(mapped, texel_data.data(), texel_data.size());
    staging->unmap();

    // --- Sampler + descriptor set ----------------------------------------
    rhi::SamplerDesc sampler_desc;
    sampler_desc.mag = rhi::Filter::Nearest;
    sampler_desc.min = rhi::Filter::Nearest;
    sampler_desc.mip = rhi::MipMapMode::None;
    sampler_desc.address_u = rhi::AddressMode::ClampToEdge;
    sampler_desc.address_v = rhi::AddressMode::ClampToEdge;

    auto sampler = device.create_sampler(sampler_desc);
    NF_CHECK(sampler != nullptr);

    auto texture_view = device.create_texture_view(*texture);
    NF_CHECK(texture_view != nullptr);
    NF_CHECK_EQ(texture_view->width(), kTexSize);
    NF_CHECK_EQ(texture_view->height(), kTexSize);

    // One combined image+sampler binding at (set 0, binding 0), read by the
    // fragment shader.
    const std::array<rhi::DescriptorBinding, 1> bindings{{
        rhi::DescriptorBinding{
            .binding = 0,
            .type = rhi::DescriptorType::SampledImage,
            .stages = rhi::ShaderStage::Fragment,
            .count = 1,
        },
    }};

    rhi::DescriptorSetLayoutDesc layout_desc;
    layout_desc.bindings = std::span<const rhi::DescriptorBinding>(bindings);

    auto set_layout = device.create_descriptor_set_layout(layout_desc);
    NF_CHECK(set_layout != nullptr);

    auto descriptor_set = device.create_descriptor_set(*set_layout);
    NF_CHECK(descriptor_set != nullptr);

    const std::array<rhi::DescriptorWrite, 1> writes{{
        rhi::DescriptorWrite{
            .binding = 0,
            .type = rhi::DescriptorType::SampledImage,
            .texture_view = texture_view.get(),
            .sampler = sampler.get(),
        },
    }};

    device.update_descriptor_set(*descriptor_set,
                                 std::span<const rhi::DescriptorWrite>(writes));

    // --- Render target ----------------------------------------------------
    constexpr u32 kWidth = OffscreenTarget::kWidth;
    constexpr u32 kHeight = OffscreenTarget::kHeight;

    rhi::TextureDesc target_desc;
    target_desc.width = kWidth;
    target_desc.height = kHeight;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;

    auto target = device.create_texture(target_desc);
    NF_CHECK(target != nullptr);

    auto render_pass = OffscreenTarget::make_pass(device, target->format());
    NF_CHECK(render_pass != nullptr);

    const std::array<rhi::Texture*, 1> fb_attachments{ target.get() };
    auto framebuffer = device.create_framebuffer(
        *render_pass, std::span<rhi::Texture* const>(fb_attachments), nullptr);
    NF_CHECK(framebuffer != nullptr);

    // --- Pipeline with vertex layout --------------------------------------
    // Two attributes: position (vec2) at offset 0, uv (vec2) at offset 8.
    const std::array<rhi::VertexAttrib, 2> attribs{{
        rhi::VertexAttrib{ .location = 0, .offset = offsetof(Vertex, x),
                           .format = rhi::Format::R32G32_SFloat },
        rhi::VertexAttrib{ .location = 1, .offset = offsetof(Vertex, u),
                           .format = rhi::Format::R32G32_SFloat },
    }};

    rhi::PipelineDesc pipeline_desc;
    pipeline_desc.vs = vs.get();
    pipeline_desc.fs = fs.get();
    pipeline_desc.topology = rhi::PrimitiveTopology::TriangleList;
    pipeline_desc.rasterizer.cull_mode = rhi::CullMode::None; // no winding surprises
    pipeline_desc.depth.test_enabled = false;
    pipeline_desc.depth.write_enabled = false;
    pipeline_desc.render_pass = render_pass.get();
    pipeline_desc.descriptor_set_layout = set_layout.get();
    pipeline_desc.vertex_layout.binding = 0;
    pipeline_desc.vertex_layout.stride = sizeof(Vertex);
    pipeline_desc.vertex_layout.attributes = std::span<const rhi::VertexAttrib>(attribs);

    auto pipeline = device.create_pipeline(pipeline_desc);
    NF_CHECK(pipeline != nullptr);

    // --- Readback buffer ---------------------------------------------------
    constexpr usize kPixelBytes = kWidth * kHeight * 4;

    rhi::BufferDesc readback_desc;
    readback_desc.size = kPixelBytes;
    readback_desc.usage = rhi::BufferUsage::TransferDst;
    readback_desc.memory = rhi::MemoryUsage::GPUToCPU;

    auto readback = device.create_buffer(readback_desc);
    NF_CHECK(readback != nullptr);

    // --- Record: upload the texture, then draw ----------------------------
    //
    // The texture upload and the draw go in one command buffer. The copy
    // transitions the image UNDEFINED → TRANSFER_DST, and afterwards we
    // transition TRANSFER_DST → SHADER_READ before the draw samples it.
    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    NF_CHECK(cmd != nullptr);
    NF_CHECK(fence != nullptr);

    cmd->begin();

    // Upload: buffer → image (image arrives in TRANSFER_DST_OPTIMAL).
    cmd->copy_buffer_to_texture(*staging, *texture, 0, 0, 0, kTexSize, kTexSize);

    // Let the shader read it. This is the second half of the
    // Undefined → TransferDst → ShaderRead chain the milestone calls for.
    cmd->transition_texture_for_sampling(*texture);

    // Draw the textured quad.
    const std::array<rhi::ClearValue, 1> clear_values{
        rhi::ClearValue{ 0.0f, 0.0f, 0.0f, 1.0f }
    };

    cmd->begin_render_pass(*render_pass, *framebuffer,
                           std::span<const rhi::ClearValue>(clear_values));
    cmd->bind_pipeline(*pipeline);

    const std::array<const rhi::Buffer*, 1> vertex_buffers{ vertex_buffer.get() };
    cmd->bind_vertex_buffers(std::span<const rhi::Buffer* const>(vertex_buffers));
    cmd->bind_index_buffer(*index_buffer, 0);

    const std::array<const rhi::DescriptorSet*, 1> sets{ descriptor_set.get() };
    cmd->bind_descriptor_sets(*set_layout,
                              std::span<const rhi::DescriptorSet* const>(sets), 0);

    cmd->set_viewport(0, 0, kWidth, kHeight);
    cmd->set_scissor(0, 0, kWidth, kHeight);

    cmd->draw_indexed(static_cast<u32>(kIndices.size()));
    cmd->end_render_pass();

    cmd->copy_texture_to_buffer(*target, *readback, 0, 0, kWidth, kHeight, 0);
    cmd->end();

    device.submit(*cmd, rhi::SubmitInfo{ .signal_fence = fence.get() });
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    device.wait_idle();

    // --- Verify the sampled texels ----------------------------------------
    Pixel* pixels = static_cast<Pixel*>(readback->map());
    NF_CHECK(pixels != nullptr);

    const auto at = [&](u32 x, u32 y) -> Pixel {
        return pixels[static_cast<usize>(y) * kWidth + x];
    };

    // The quad covers 80% of the target centered, so it spans x,y in
    // [12.8, 115.2] on a 128x128 target. Sample well inside each quadrant of
    // the quad so we never land on an edge texel.
    //
    // Screen-space layout (y grows downward in the readback):
    //   top-left     = texture top-left     = red
    //   top-right    = texture top-right    = green
    //   bottom-left  = texture bottom-left  = blue
    //   bottom-right = texture bottom-right = yellow
    const Pixel top_left     = at(40, 30);
    const Pixel top_right    = at(88, 30);
    const Pixel bottom_left  = at(40, 98);
    const Pixel bottom_right = at(88, 98);

    // Outside the quad the attachment must still hold the clear color, which
    // proves the scissor/viewport did not stretch the quad over everything.
    const Pixel outside = at(2, 2);

    readback->unmap();

    NF_CHECK_EQ(outside.r, 0);
    NF_CHECK_EQ(outside.g, 0);
    NF_CHECK_EQ(outside.b, 0);

    // Each quadrant must match its texture color. This is the assertion that
    // fails if the upload, the UVs, the sampler, or the descriptor binding is
    // wrong in any way.
    NF_LOG_WARN(LogCategory::RHI,
        "quad pixels: TL({}, {}, {}) TR({}, {}, {}) BL({}, {}, {}) BR({}, {}, {}) OUT({}, {}, {})",
        top_left.r, top_left.g, top_left.b,
        top_right.r, top_right.g, top_right.b,
        bottom_left.r, bottom_left.g, bottom_left.b,
        bottom_right.r, bottom_right.g, bottom_right.b,
        outside.r, outside.g, outside.b);
    NF_CHECK(pixel_near(top_left,     kQuadrantColors[0]));
    NF_CHECK(pixel_near(top_right,    kQuadrantColors[1]));
    NF_CHECK(pixel_near(bottom_left,  kQuadrantColors[2]));
    NF_CHECK(pixel_near(bottom_right, kQuadrantColors[3]));

    NF_LOG_INFO(LogCategory::RHI,
        "Textured quad verified: TL({}, {}, {}) TR({}, {}, {}) BL({}, {}, {}) BR({}, {}, {})",
        top_left.r, top_left.g, top_left.b,
        top_right.r, top_right.g, top_right.b,
        bottom_left.r, bottom_left.g, bottom_left.b,
        bottom_right.r, bottom_right.g, bottom_right.b);
}

NF_TEST(rhi_staging_buffer_roundtrips_vertex_data) {
    const GpuFixture& fixture = require_gpu();

    auto& device = *fixture.device;

    // A device-local vertex buffer must survive an upload and give the bytes
    // back. Reading it requires a second staging buffer in the other
    // direction, which also proves GPUToCPU memory works for geometry.
    constexpr usize kBytes = sizeof(Vertex) * kVertices.size();

    rhi::BufferDesc gpu_desc;
    gpu_desc.size = kBytes;
    gpu_desc.usage = rhi::BufferUsage::Vertex |
                     rhi::BufferUsage::TransferDst |
                     rhi::BufferUsage::TransferSrc;
    gpu_desc.memory = rhi::MemoryUsage::GPUOnly;

    auto gpu_buffer = device.create_buffer(gpu_desc);
    NF_CHECK(gpu_buffer != nullptr);
    gpu_buffer->update(kVertices.data(), 0, kBytes);

    rhi::BufferDesc readback_desc;
    readback_desc.size = kBytes;
    readback_desc.usage = rhi::BufferUsage::TransferDst;
    readback_desc.memory = rhi::MemoryUsage::GPUToCPU;

    auto readback = device.create_buffer(readback_desc);
    NF_CHECK(readback != nullptr);

    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    NF_CHECK(cmd != nullptr);
    NF_CHECK(fence != nullptr);

    cmd->begin();
    cmd->copy_buffer(*gpu_buffer, *readback, 0, 0, kBytes);
    cmd->end();

    device.submit(*cmd, rhi::SubmitInfo{ .signal_fence = fence.get() });
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    device.wait_idle();

    const auto* got = static_cast<const Vertex*>(readback->map());
    NF_CHECK(got != nullptr);

    bool identical = true;
    for (usize i = 0; i < kVertices.size(); ++i) {
        if (std::memcmp(&got[i], &kVertices[i], sizeof(Vertex)) != 0) {
            identical = false;
            break;
        }
    }

    readback->unmap();
    NF_CHECK(identical);
}

NF_TEST(rhi_descriptor_set_rejects_empty_layout) {
    const GpuFixture& fixture = require_gpu();

    auto& device = *fixture.device;

    // A layout with no bindings cannot describe any resource, so the backend
    // must refuse it rather than create a set that silently binds nothing.
    rhi::DescriptorSetLayoutDesc empty_desc;
    empty_desc.bindings = std::span<const rhi::DescriptorBinding>{};

    auto bad_layout = device.create_descriptor_set_layout(empty_desc);
    NF_CHECK(bad_layout == nullptr);
}
