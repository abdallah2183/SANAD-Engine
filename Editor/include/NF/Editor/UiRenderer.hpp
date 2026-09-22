#pragma once

// NF/Editor/UiRenderer.hpp — Dear ImGui draw submission through the RHI.
//
// This is the Phase 5 replacement for the stock ImGui_ImplVulkan backend: it
// records an ImGui frame's ImDrawData with only backend-neutral RHI calls
// (pipeline, vertex/index buffers, one combined image sampler, push
// constants, scissors), so validation and lifetime behave exactly like every
// other tested renderer in the tree. No raw Vulkan leaks out of this class.
//
// Texture model: ImGui most often samples the font atlas; the editor viewport
// panel additionally samples the offscreen viewport target, and the P3 asset
// previews sample small per-path thumbnails. All three are plain ImTextureID
// constants resolved to descriptor sets built per frame — ImGui never sees RHI
// pointers. Only the ids actually referenced by the recorded draw data
// allocate a set, so a content tree with hundreds of textures costs nothing
// until a panel shows one.
//
// Contracts (mirroring Renderer3D):
//   - render() may only be called when the previous frame's GPU work has
//     completed (fence waited): it resets the per-frame descriptor allocator
//     and may grow the vertex/index buffers, destroying the old ones.
//   - The caller owns the render pass/framebuffer (a color Load pass over the
//     already-rendered scene) and the command buffer recording state.
//   - Viewport view/sampler passed to set_viewport_texture() must outlive
//     every render() that references them (owned by ViewportResources).

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

struct ImDrawData;

namespace nf::editor {

class UiRenderer {
public:
    // ImTextureID values (cast to void* at the call site).
    static constexpr uintptr_t kFontTextureId = 1;
    static constexpr uintptr_t kViewportTextureId = 2;

    UiRenderer() = default;
    ~UiRenderer();

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    // Loads imgui_vert/frag.spv from shader_dir, uploads the font atlas.
    bool init(rhi::IGraphicsDevice& device, const std::filesystem::path& shader_dir);
    void shutdown();
    bool valid() const;

    void set_viewport_texture(const rhi::TextureView* view, const rhi::Sampler* sampler);

    // Content textures (asset previews) for the frame about to be recorded.
    // `id` must be a TexturePreviewCache id (above kFirstId) and the view/
    // sampler must outlive the following render(). Replaces the previous
    // frame's table; pass nullptr/0 to clear.
    struct TextureBinding {
        uintptr_t id = 0;
        const rhi::TextureView* view = nullptr;
        const rhi::Sampler* sampler = nullptr;
    };
    void set_content_textures(const TextureBinding* bindings, size_t count);

    // Records draw_data into cmd (inside an active render pass built for a
    // pipeline from pipeline_for()). Returns false on descriptor/pipeline
    // failure. A null/empty draw_data is a successful no-op.
    bool render(rhi::CommandBuffer& cmd, const rhi::RenderPass& pass, const ImDrawData* draw_data,
                uint32_t fb_width, uint32_t fb_height);

    // Pipeline for a concrete pass (cached per pass pointer).
    rhi::Pipeline* pipeline_for(const rhi::RenderPass& pass);

private:
    bool ensure_buffers(size_t vtx_count, size_t idx_count);

    rhi::IGraphicsDevice* m_device = nullptr;
    std::unique_ptr<rhi::ShaderModule> m_vs;
    std::unique_ptr<rhi::ShaderModule> m_fs;
    std::unique_ptr<rhi::DescriptorSetLayout> m_layout;
    std::unique_ptr<rhi::Sampler> m_sampler;
    std::unique_ptr<rhi::Texture> m_font_texture;
    std::unique_ptr<rhi::TextureView> m_font_view;
    std::unique_ptr<rhi::DescriptorAllocator> m_allocator;
    std::unique_ptr<rhi::Buffer> m_vb;
    std::unique_ptr<rhi::Buffer> m_ib;
    size_t m_vb_cap = 0;
    size_t m_ib_cap = 0; // in u32 indices
    std::unordered_map<const rhi::RenderPass*, std::unique_ptr<rhi::Pipeline>> m_pipelines;

    const rhi::TextureView* m_viewport_view = nullptr;
    const rhi::Sampler* m_viewport_sampler = nullptr;

    std::vector<TextureBinding> m_content_bindings;
};

} // namespace nf::editor
