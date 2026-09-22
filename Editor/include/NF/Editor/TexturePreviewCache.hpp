#pragma once

// NF/Editor/TexturePreviewCache.hpp — GPU thumbnails for the editor panels.
//
// Complements PreviewPixels: that module shrinks pixels on the CPU, this one
// owns the RHI objects for the result and hands ImGui a stable texture id per
// logical path. Lives in the NOVAForgeEditor shell only — NFEditorCore stays
// device-free, so the panels reach the cache through the EditorApp
// `preview_texture` hook and get 0 (→ placeholder) when previews are off.
//
// Lifetime: one upload per path, kept until shutdown. Hot reload of a source
// file invalidates an entry by path, and the next `acquire` re-decodes. The
// ids handed out are stable for a live entry, so a panel that holds one across
// frames keeps sampling the same view.
//
// The ids start well above UiRenderer's reserved constants (font/viewport) so
// the shell's reserved slots and the cache's never collide.

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::editor {

class UiRenderer;

class TexturePreviewCache {
public:
    // 0 is the panel placeholder; UiRenderer reserves 1 (font) and 2 (viewport).
    static constexpr uintptr_t kInvalidId = 0;
    static constexpr uintptr_t kFirstId = 1000;
    // Previews are UI-sized: a 4K source has no business on the GPU twice.
    static constexpr int kMaxDim = 128;

    TexturePreviewCache() = default;
    ~TexturePreviewCache();

    TexturePreviewCache(const TexturePreviewCache&) = delete;
    TexturePreviewCache& operator=(const TexturePreviewCache&) = delete;

    bool init(rhi::IGraphicsDevice& device);
    void shutdown();
    [[nodiscard]] bool valid() const { return m_device != nullptr; }

    /// Returns the ImGui texture id for `logical_path`, uploading on first
    /// request. Returns kInvalidId when the path cannot be resolved, decoded,
    /// or uploaded — the panel draws a placeholder and the failure is already
    /// in the log, so the caller does not need a second error channel.
    uintptr_t acquire(assets::VirtualFileSystem& vfs, const std::string& logical_path);

    /// Drops one entry (used on hot reload so the next acquire re-decodes).
    void invalidate(const std::string& logical_path);

    /// One binding per preview actually requested since the last drain, for
    /// UiRenderer::set_content_textures. Draining (not clearing) keeps entries
    /// alive: a panel that skips a frame should not force a re-decode.
    struct Binding {
        uintptr_t id = kInvalidId;
        const rhi::TextureView* view = nullptr;
        const rhi::Sampler* sampler = nullptr;
    };
    std::vector<Binding> drain_used();

private:
    struct Entry {
        uintptr_t id = kInvalidId;
        std::unique_ptr<rhi::Texture> texture;
        std::unique_ptr<rhi::TextureView> view;
        bool used = false;
    };

    rhi::IGraphicsDevice* m_device = nullptr;
    std::unique_ptr<rhi::Sampler> m_sampler;
    std::unordered_map<std::string, Entry> m_entries;
    uintptr_t m_next_id = kFirstId;
};

} // namespace nf::editor
