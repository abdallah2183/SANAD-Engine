#pragma once

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/PipelineCache.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::runtime {

// Fallback material assignment when MeshComponent::material is empty.
inline constexpr const char* kDefaultMaterialPath = "content://Materials/Default";

struct RuntimeConfig {
    std::string scene_path = "content://Scenes/Example.nfscene";
    uint32_t max_frames = 0;
    bool validation = false;
    bool headless = false;
    uint32_t width = 1280;
    uint32_t height = 720;
};

class Runtime {
public:
    Runtime(assets::VirtualFileSystem& vfs, assets::AssetRegistry& registry, assets::AssetManager& manager,
            rhi::IGraphicsDevice& device, rhi::Swapchain* swapchain);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Load a scene from a logical path (e.g. content://Scenes/Example.nfscene)
    // Returns true on success (even if some assets are missing, it will log and continue)
    bool load_scene(const std::string& logical_path, std::string& out_error);

    // Per-frame update: handles async asset GPU uploads, transform propagation, culling, etc.
    void update(float dt);

    // Render the current scene to the swapchain image at `image_index`
    // The caller is responsible for acquiring the image and presenting.
    // Records Depth → GBuffer → Lighting → Tonemap via Renderer3D.
    // On failure records a distinct fallback clear (magenta) and logs.
    void render(uint32_t image_index, rhi::CommandBuffer& cmd);

    // For headless/offscreen rendering (tests).
    // Records the full Renderer3D pipeline into `cmd` targeting `target`
    // (no swapchain, no PRESENT layout). On failure records fallback clear.
    void render_offscreen(rhi::Texture& target, rhi::CommandBuffer& cmd);

    // Frees all GPU resources (renderer, mesh/material libraries, fallback
    // passes) after wait_idle. Must be called before device->shutdown().
    // Safe to call multiple times; subsequent renders will re-initialize.
    void shutdown();

    // Updates the swapchain pointer (after recreation on resize) and resizes
    // renderer resources when the resolution changed.
    void on_swapchain_resized(rhi::Swapchain* swapchain);

    bool has_scene() const { return m_scene_data_ptr && m_scene_data_ptr->scene; }
    size_t mesh_count() const { return m_mesh_library ? m_mesh_library->size() : 0; }
    const rendering::Renderer3D* renderer() const { return m_renderer.get(); }

    // --- Shared material assets (Phase 5) ---
    // MeshComponent::material names a .nfmat logical path; every entity with
    // the same path shares one renderer instance (one 48-byte UBO). Edits
    // rewrite that UBO in place — visible next frame, no pipeline touched.
    rendering::MaterialHandle material_for_path(const std::string& logical_path);
    bool set_material_params(const std::string& logical_path,
                             const rendering::PBRMaterialParams& params, std::string& out_error);
    bool material_params(const std::string& logical_path,
                         rendering::PBRMaterialParams& out) const;
    std::vector<std::string> known_material_paths() const;
    // Writes the entry's current params to dst_path (same-path save clears
    // the entry's dirty flag; copies leave it set).
    bool save_material(const std::string& src_path, const std::string& dst_path,
                       std::string& out_error);

    // --- Texture assets for material albedo (Phase 5) ---
    // Decoded once from content://, uploaded, and cached; materials reference
    // textures by path and rebind automatically on reload.
    bool ensure_texture(const std::string& logical_path, std::string& out_error);
    // Re-decodes and re-uploads (failure keeps the old texture live), then
    // rebinds every material using it. Hot Reload's texture path.
    bool reload_texture(const std::string& logical_path, std::string& out_error);
    // Reloads a mesh asset from its registry cooked file into the live mesh
    // library slot (handles stay valid; bounds follow the new geometry).
    // Failure keeps the old GPU copy. Hot Reload's mesh path.
    bool hot_reload_mesh(const assets::AssetId& id, std::string& out_error);
    // Re-reads a .nfmat file into the live instance (no dirty flag: the file
    // is the source of truth). Entries with unsaved UI edits are SKIPPED with
    // a warning — the user's work always wins over the disk.
    bool reload_material_file(const std::string& logical_path, std::string& out_error);
    // Binds (or, when empty, unbinds) a material's albedo. Marks dirty.
    bool set_material_albedo(const std::string& material_path, const std::string& texture_path,
                             std::string& out_error);
    std::string material_albedo(const std::string& material_path) const;
    std::vector<std::string> known_texture_paths() const;
    bool material_dirty(const std::string& logical_path) const;
    bool any_material_dirty() const;

    // --- Editor hooks (the Editor owns no rendering; it edits this scene) ---
    // Mutable access to the loaded scene for outliner/inspector/commands.
    // The pointer stays valid until the next load_scene() or destruction.
    scene::Scene* edit_scene();
    const scene::Scene* scene() const;
    const std::string& loaded_scene_path() const { return m_loaded_scene_path; }
    // Persists the current scene via the standard .nfscene serializer.
    bool save_scene(const std::string& logical_path, std::string& out_error);
    // Notifies the runtime that the scene was edited externally:
    // re-propagates transforms and bumps scene_version() for viewport sync.
    void mark_scene_edited();
    uint64_t scene_version() const { return m_scene_version; }

private:
    struct SceneRuntimeData {
        std::unique_ptr<scene::Scene> scene;
    };
    assets::VirtualFileSystem& m_vfs;
    assets::AssetRegistry& m_registry;
    assets::AssetManager& m_manager;
    rhi::IGraphicsDevice& m_device;
    rhi::Swapchain* m_swapchain = nullptr;

    std::unique_ptr<SceneRuntimeData> m_scene_data_ptr;

    // Renderer3D integration (owns its own PipelineCache, RenderGraph, etc.)
    std::unique_ptr<rendering::Renderer3D> m_renderer;
    std::unique_ptr<rendering::MeshLibrary> m_mesh_library;
    std::unique_ptr<rendering::MaterialLibrary> m_material_library;
    std::unique_ptr<rendering::PipelineCache> m_pipeline_cache;
    bool m_renderer_initialized = false;
    std::unordered_map<assets::AssetId, rendering::StaticMeshHandle> m_mesh_handles;
    rendering::MaterialHandle m_default_material{};
    std::string m_loaded_scene_path;
    uint64_t m_scene_version = 0;

    // Path -> renderer instance (includes gray fallbacks for missing files, so
    // file IO happens at most once per path; Hot Reload drops entries).
    std::unordered_map<std::string, rendering::MaterialHandle> m_material_instances;
    std::unordered_map<std::string, bool> m_material_dirty;
    // Material path -> albedo texture path ("" = scalar only).
    std::unordered_map<std::string, std::string> m_material_albedo;

    // Owned GPU texture + view + sampler. Destruction order matters (view
    // before texture), hence declaration order: reverse destruction frees the
    // sampler, then the view, then the texture. All destroyed after wait_idle.
    struct TextureObjects {
        std::unique_ptr<rhi::Texture> texture;
        std::unique_ptr<rhi::TextureView> view;
        std::unique_ptr<rhi::Sampler> sampler;
    };
    std::unordered_map<std::string, std::unique_ptr<TextureObjects>> m_textures;

    static std::string normalize_material_path(const std::string& p);
    static rendering::PBRMaterialParams default_material_params();
    bool load_material_file(const std::string& path, rendering::PBRMaterialParams& out) const;
    std::unique_ptr<TextureObjects> upload_texture_objects(const std::string& logical_path,
                                                           const rendering::DecodedImage& img,
                                                           std::string& out_error);

    // Fallback clear resources: kept alive until shutdown() so the command
    // buffer that references them stays valid past cmd->end()/submit.
    // Only used when Renderer3D fails; the success path uses no transients.
    std::vector<std::unique_ptr<rhi::RenderPass>> m_fallback_rps;
    std::vector<std::unique_ptr<rhi::Framebuffer>> m_fallback_fbs;

    bool ensure_renderer_initialized();
    bool ensure_renderer_initialized_for(uint32_t width, uint32_t height);
    void sync_meshes_from_assets();
    void ensure_default_material();
    bool extract_camera(uint32_t target_width, uint32_t target_height, rendering::Camera& out);
    void extract_light();
    void build_render_world(rendering::RenderWorld& out);
    void fallback_clear(rhi::Texture& target, rhi::CommandBuffer& cmd, bool present_source);
};

} // namespace nf::runtime
