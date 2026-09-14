// Editor UI overlay: color-Load preservation, ImGui draws over a scene pass,
// and viewport-texture sampling — all headless, all pixel-proven.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Editor/UiRenderer.hpp>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

using namespace nf;
using namespace nf::test;

#ifndef NF_EDITOR_IMGUI_SHADER_DIR
#define NF_EDITOR_IMGUI_SHADER_DIR ""
#endif

namespace {

std::unique_ptr<rhi::Texture> make_target(rhi::IGraphicsDevice& device, uint32_t w, uint32_t h) {
    rhi::TextureDesc td{};
    td.width = w;
    td.height = h;
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    return device.create_texture(td);
}

std::unique_ptr<rhi::RenderPass> make_pass(rhi::IGraphicsDevice& device, rhi::Format fmt,
                                           rhi::RenderPassDesc::ColorLoad load, bool blend) {
    rhi::ColorAttachment ca{};
    ca.format = fmt;
    ca.blend_enabled = blend;
    ca.src_color = rhi::BlendFactor::SrcAlpha;
    ca.dst_color = rhi::BlendFactor::OneMinusSrcAlpha;
    const std::array<rhi::ColorAttachment, 1> atts{ca};
    rhi::RenderPassDesc rpd{};
    rpd.color_attachments = std::span<const rhi::ColorAttachment>(atts);
    rpd.color_load = load;
    rpd.present_source = false;
    return device.create_render_pass(rpd);
}

uint32_t count_non_blue(const std::vector<uint8_t>& px) {
    uint32_t n = 0;
    const size_t count = px.size() / 4;
    for (size_t i = 0; i < count; ++i) {
        const int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
        // Background is pure blue (0,0,255): anything else is drawn content.
        if (r > 12 || g > 12 || b < 243) {
            ++n;
        }
    }
    return n;
}

} // namespace

NF_TEST(ui_color_load_preserves) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    constexpr uint32_t kW = 64, kH = 64;
    auto target = make_target(device, kW, kH);
    NF_CHECK(target);
    auto clear_pass = make_pass(device, rhi::Format::R8G8B8A8_UNorm,
                                rhi::RenderPassDesc::ColorLoad::Clear, false);
    auto load_pass = make_pass(device, rhi::Format::R8G8B8A8_UNorm,
                               rhi::RenderPassDesc::ColorLoad::Load, false);
    NF_CHECK(clear_pass && load_pass);
    const std::array<rhi::Texture*, 1> cols{target.get()};
    auto fb_clear = device.create_framebuffer(*clear_pass, std::span<rhi::Texture* const>(cols), nullptr);
    auto fb_load = device.create_framebuffer(*load_pass, std::span<rhi::Texture* const>(cols), nullptr);
    NF_CHECK(fb_clear && fb_load);

    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    NF_CHECK(cmd && fence);
    rhi::BufferDesc bd{};
    bd.size = static_cast<usize>(kW) * kH * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = device.create_buffer(bd);
    NF_CHECK(rb);

    cmd->begin();
    const std::array<rhi::ClearValue, 1> red{rhi::ClearValue{1.0f, 0.0f, 0.0f, 1.0f}};
    cmd->begin_render_pass(*clear_pass, *fb_clear, std::span<const rhi::ClearValue>(red));
    cmd->end_render_pass();
    // Same-usage chaining barrier (the scene->UI case in miniature).
    cmd->barrier_texture(*target, rhi::ImageUsage::ColorAtt, rhi::ImageUsage::ColorAtt);
    cmd->begin_render_pass(*load_pass, *fb_load, std::span<const rhi::ClearValue>{});
    cmd->end_render_pass();
    cmd->copy_texture_to_buffer(*target, *rb, 0, 0, kW, kH, 0);
    cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(5000000000ULL));

    const auto* px = static_cast<const uint8_t*>(rb->map());
    NF_CHECK(px);
    // Center must still be the clear red: Load preserved it.
    const size_t ci = (static_cast<size_t>(kH / 2) * kW + (kW / 2)) * 4;
    NF_CHECK(px[ci] > 200 && px[ci + 1] < 60 && px[ci + 2] < 60);
    rb->unmap();

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    rhi::reset_validation_error_count();
}

NF_TEST(ui_imgui_draws_over_cleared_target) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    // ImGui context first: UiRenderer::init reads the font atlas through it.
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    constexpr uint32_t kW = 64, kH = 64;
    io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();

    editor::UiRenderer ui;
    NF_CHECK(ui.init(device, std::filesystem::path(NF_EDITOR_IMGUI_SHADER_DIR)));
    NF_CHECK(ui.valid());

    auto target = make_target(device, kW, kH);
    NF_CHECK(target);
    auto clear_pass = make_pass(device, rhi::Format::R8G8B8A8_UNorm,
                                rhi::RenderPassDesc::ColorLoad::Clear, false);
    auto load_pass = make_pass(device, rhi::Format::R8G8B8A8_UNorm,
                               rhi::RenderPassDesc::ColorLoad::Load, true);
    NF_CHECK(clear_pass && load_pass);
    const std::array<rhi::Texture*, 1> cols{target.get()};
    auto fb_clear = device.create_framebuffer(*clear_pass, std::span<rhi::Texture* const>(cols), nullptr);
    auto fb_load = device.create_framebuffer(*load_pass, std::span<rhi::Texture* const>(cols), nullptr);
    NF_CHECK(fb_clear && fb_load);

    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    rhi::BufferDesc bd{};
    bd.size = static_cast<usize>(kW) * kH * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = device.create_buffer(bd);
    NF_CHECK(cmd && fence && rb);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(48, 40), ImGuiCond_Always);
    ImGui::Begin("T", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::Button("OK", ImVec2(32, 16));
    ImGui::End();
    ImGui::Render();

    cmd->begin();
    const std::array<rhi::ClearValue, 1> blue{rhi::ClearValue{0.0f, 0.0f, 1.0f, 1.0f}};
    cmd->begin_render_pass(*clear_pass, *fb_clear, std::span<const rhi::ClearValue>(blue));
    cmd->end_render_pass();
    cmd->barrier_texture(*target, rhi::ImageUsage::ColorAtt, rhi::ImageUsage::ColorAtt);
    cmd->begin_render_pass(*load_pass, *fb_load, std::span<const rhi::ClearValue>{});
    NF_CHECK(ui.render(*cmd, *load_pass, ImGui::GetDrawData(), kW, kH));
    cmd->end_render_pass();
    cmd->copy_texture_to_buffer(*target, *rb, 0, 0, kW, kH, 0);
    cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(5000000000ULL));

    const auto* px = static_cast<const uint8_t*>(rb->map());
    NF_CHECK(px);
    std::vector<uint8_t> pixels(px, px + static_cast<size_t>(kW) * kH * 4);
    rb->unmap();
    // Window background + white button over pure blue: unmistakable coverage.
    NF_CHECK(count_non_blue(pixels) > 200);

    ImGui::DestroyContext();
    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    ui.shutdown();
    device.wait_idle();
    rhi::reset_validation_error_count();
}

NF_TEST(ui_viewport_image_binding) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    // ImGui context first: UiRenderer::init reads the font atlas through it.
    ImGui::CreateContext();
    ImGuiIO& view_io = ImGui::GetIO();
    view_io.DisplaySize = ImVec2(64, 64);
    view_io.DeltaTime = 1.0f / 60.0f;
    view_io.Fonts->AddFontDefault();
    view_io.Fonts->Build();

    editor::UiRenderer ui;
    NF_CHECK(ui.init(device, std::filesystem::path(NF_EDITOR_IMGUI_SHADER_DIR)));

    // A magenta "viewport" texture, sampled through kViewportTextureId.
    auto mag = make_target(device, 16, 16);
    NF_CHECK(mag);
    auto fill_pass = make_pass(device, rhi::Format::R8G8B8A8_UNorm,
                               rhi::RenderPassDesc::ColorLoad::Clear, false);
    NF_CHECK(fill_pass);
    const std::array<rhi::Texture*, 1> mag_cols{mag.get()};
    auto mag_fb = device.create_framebuffer(*fill_pass, std::span<rhi::Texture* const>(mag_cols), nullptr);
    NF_CHECK(mag_fb);
    rhi::TextureViewDesc vd{};
    vd.texture = mag.get();
    auto mag_view = device.create_texture_view(vd);
    rhi::SamplerDesc sd{};
    sd.address_u = rhi::AddressMode::ClampToEdge;
    sd.address_v = rhi::AddressMode::ClampToEdge;
    auto mag_sampler = device.create_sampler(sd);
    NF_CHECK(mag_view && mag_sampler);
    ui.set_viewport_texture(mag_view.get(), mag_sampler.get());

    constexpr uint32_t kW = 64, kH = 64;
    auto target = make_target(device, kW, kH);
    auto load_pass = make_pass(device, rhi::Format::R8G8B8A8_UNorm,
                               rhi::RenderPassDesc::ColorLoad::Load, true);
    NF_CHECK(target && load_pass);
    const std::array<rhi::Texture*, 1> cols{target.get()};
    auto fb_load = device.create_framebuffer(*load_pass, std::span<rhi::Texture* const>(cols), nullptr);
    NF_CHECK(fb_load);

    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    rhi::BufferDesc bd{};
    bd.size = static_cast<usize>(kW) * kH * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = device.create_buffer(bd);
    NF_CHECK(cmd && fence && rb);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kW), static_cast<float>(kH)),
                             ImGuiCond_Always);
    ImGui::Begin("V", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImGui::Image(static_cast<ImTextureID>(editor::UiRenderer::kViewportTextureId),
                 ImVec2(32, 32));
    ImGui::End();
    ImGui::Render();

    cmd->begin();
    const std::array<rhi::ClearValue, 1> magenta{rhi::ClearValue{1.0f, 0.0f, 1.0f, 1.0f}};
    cmd->begin_render_pass(*fill_pass, *mag_fb, std::span<const rhi::ClearValue>(magenta));
    cmd->end_render_pass();
    // Magenta target is sampled below: transition out of the color layout.
    cmd->barrier_texture(*mag, rhi::ImageUsage::ColorAtt, rhi::ImageUsage::Sampled);
    const std::array<rhi::ClearValue, 1> blue{rhi::ClearValue{0.0f, 0.0f, 1.0f, 1.0f}};
    auto clear2_pass = make_pass(device, rhi::Format::R8G8B8A8_UNorm,
                                 rhi::RenderPassDesc::ColorLoad::Clear, false);
    NF_CHECK(clear2_pass);
    auto fb_clear2 =
        device.create_framebuffer(*clear2_pass, std::span<rhi::Texture* const>(cols), nullptr);
    NF_CHECK(fb_clear2);
    cmd->begin_render_pass(*clear2_pass, *fb_clear2, std::span<const rhi::ClearValue>(blue));
    cmd->end_render_pass();
    cmd->barrier_texture(*target, rhi::ImageUsage::ColorAtt, rhi::ImageUsage::ColorAtt);
    cmd->begin_render_pass(*load_pass, *fb_load, std::span<const rhi::ClearValue>{});
    NF_CHECK(ui.render(*cmd, *load_pass, ImGui::GetDrawData(), kW, kH));
    cmd->end_render_pass();
    cmd->copy_texture_to_buffer(*target, *rb, 0, 0, kW, kH, 0);
    cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(5000000000ULL));

    const auto* px = static_cast<const uint8_t*>(rb->map());
    NF_CHECK(px);
    uint32_t magenta_px = 0;
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i) {
        if (px[i * 4] > 200 && px[i * 4 + 1] < 60 && px[i * 4 + 2] > 200) {
            ++magenta_px;
        }
    }
    rb->unmap();
    // 32x32 sampled quad: the bulk of its 1024 pixels must read magenta.
    NF_CHECK(magenta_px > 500);

    ImGui::DestroyContext();
    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    ui.shutdown();
    device.wait_idle();
    rhi::reset_validation_error_count();
}
