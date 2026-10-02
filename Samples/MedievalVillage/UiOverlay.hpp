#pragma once

// Samples/MedievalVillage/UiOverlay.hpp — the runtime draw backend for the
// engine's game UI.
//
// NF/UI is deliberately split: Canvas/GameFlow/Hud are retained widgets that
// produce a ui::DisplayList ("rects + lines + text runs, nothing else"), and
// "a draw backend (GPU overlay, software blit, debug console) consumes only
// that" — see NF/UI/Widgets.hpp. The editor has such a backend (its ImGui
// panels render through NF/Editor/UiRenderer), but a *shipped game* had none,
// so nothing could put the engine's HUD on screen.
//
// This class is that backend, for a windowed game:
//
//   ui::GameFlow / Hud / Menu  ->  ui::DisplayList  ->  ImGui draw list  ->  RHI
//
// ImGui is used for what it is good at here — glyph rasterisation and batched
// 2D submission — and not as the UI model. The screens, the menu state machine
// and the HUD values all come from the engine's UI module, so the same
// GameFlow a Lua script or the editor drives is what the player sees.
//
// Arabic: DisplayList text is logical-order UTF-8 and shaping is the backend's
// job, so every text run goes through ui::shape_arabic() before it reaches
// ImGui, exactly as the editor does. The Arabic companion font is merged into
// the same atlas so a mixed Latin/Arabic line draws in one pass.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/UI/Widgets.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawData;

namespace nf::sample::medieval {

class UiOverlay {
public:
    UiOverlay() = default;
    ~UiOverlay();

    UiOverlay(const UiOverlay&) = delete;
    UiOverlay& operator=(const UiOverlay&) = delete;

    /// Creates the ImGui context, attaches the Win32 input backend to `hwnd`,
    /// builds the font atlas (Segoe UI + Amiri for Arabic) and uploads it
    /// through the RHI, loading imgui_vert/frag.spv from `shader_dir`.
    bool init(rhi::IGraphicsDevice& device, void* hwnd, const std::filesystem::path& shader_dir);
    void shutdown();
    bool valid() const;

    /// Creates the "color Load" pass over the swapchain and one framebuffer per
    /// swapchain image. Call once per swapchain; call again after a resize.
    bool attach_swapchain(rhi::Swapchain& swapchain);
    void detach_swapchain();

    /// Opens an ImGui frame. `dt` feeds ImGui's own animation timers.
    void begin_frame(f32 dt);

    /// Draws a ui::DisplayList. Coordinates in the list are pixels for a
    /// width x height viewport (Canvas::snapshot already baked them).
    void draw(const ui::DisplayList& list, f32 width, f32 height);

    /// Convenience: snapshot `canvas` and draw it.
    void draw_canvas(const ui::Canvas& canvas, f32 width, f32 height);

    /// Closes the ImGui frame and keeps the draw data for render().
    void end_frame();

    /// Records the frame's UI into `cmd`, inside a Load pass over `target`.
    /// `target` must be the swapchain image at `image_index`. `frame_slot`
    /// selects this frame's in-flight slot (0..kFramesInFlight-1): the vertex
    /// and index buffers are per-slot because a shared buffer rewritten while
    /// the previous frame still draws from it is a GPU data race that ends in
    /// VK_ERROR_DEVICE_LOST. Returns false on a pipeline or descriptor
    /// failure; an empty frame is a successful no-op.
    bool render(rhi::CommandBuffer& cmd, rhi::Texture& target, u32 image_index, u32 frame_slot,
                u32 width, u32 height);

    /// Win32 message hook for Window::set_message_hook.
    static bool handle_win32_message(void* hwnd, u32 msg, u64 wparam, i64 lparam);

    /// True when the font atlas was built with the Arabic companion font.
    bool arabic_font_loaded() const { return m_arabic_font; }
    const std::string& font_description() const { return m_font_used; }

private:
    /// How many frames may be in flight. Must match the game loop's
    /// kFramesInFlight: the fence the caller waits on before recording a slot
    /// is exactly what makes reusing that slot's buffers safe.
    static constexpr u32 kFramesInFlight = 2;

    bool ensure_buffers(u32 frame_slot, usize vtx_count, usize idx_count);
    rhi::Pipeline* pipeline();

    rhi::IGraphicsDevice* m_device = nullptr;
    rhi::Swapchain* m_swapchain = nullptr;

    std::unique_ptr<rhi::ShaderModule> m_vs;
    std::unique_ptr<rhi::ShaderModule> m_fs;
    std::unique_ptr<rhi::DescriptorSetLayout> m_layout;
    std::unique_ptr<rhi::Sampler> m_sampler;
    std::unique_ptr<rhi::Texture> m_font_texture;
    std::unique_ptr<rhi::TextureView> m_font_view;
    std::unique_ptr<rhi::DescriptorSet> m_font_set;
    std::unique_ptr<rhi::DescriptorAllocator> m_allocator;
    std::unique_ptr<rhi::Pipeline> m_pipeline;

    // One vertex/index pair per in-flight slot. A single shared pair raced
    // with the previous frame's GPU reads once the engine moved to two frames
    // in flight, and the GPU faulted (VK_ERROR_DEVICE_LOST) after a
    // nondeterministic number of frames — sometimes 2 seconds, sometimes 17.
    std::array<std::unique_ptr<rhi::Buffer>, kFramesInFlight> m_vb;
    std::array<std::unique_ptr<rhi::Buffer>, kFramesInFlight> m_ib;
    std::array<usize, kFramesInFlight> m_vb_cap{};
    std::array<usize, kFramesInFlight> m_ib_cap{}; // in u32 indices

    std::unique_ptr<rhi::RenderPass> m_pass;
    std::vector<std::unique_ptr<rhi::Framebuffer>> m_framebuffers;

    const ImDrawData* m_draw_data = nullptr;
    std::string m_font_used;
    bool m_arabic_font = false;
    bool m_context_created = false;
};

} // namespace nf::sample::medieval
