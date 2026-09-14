#pragma once

// NF/Rendering/Renderer3D.hpp — the integrated Basic 3D pipeline
//
//   RenderWorld → Frustum Culling → Depth Prepass → GBuffer → PBR Lighting
//                                                    → HDR → Exposure → Tonemap → RHI
//
// Everything below runs through the RenderGraph with generic, backend-neutral
// resource states; the Vulkan backend alone turns transitions into layouts/
// stages/access masks. The renderer owns its pass resources (depth target,
// gbuffer attachments, HDR target are RenderGraph-owned textures) and rebuilds
// the per-frame pass list on every render() call.
//
// Contract:
//   - render() must only be called when the previous frame's GPU work has
//     completed (fence wait) — it resets its per-frame descriptor allocator.
//   - After render() the command buffer is ended but NOT submitted; the
//     caller submits (and presents, when rendering to a swapchain image).
//   - Lights/exposure are renderer state; meshes/materials come from the
//     libraries; the game world is consumed only through RenderWorld.

#include <NF/Core/Types.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Rendering/Material.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/PipelineCache.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/RenderWorld.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace nf::rendering {

// --- Light types (renderer-side for this milestone; ECS extraction later) ---

struct DirectionalLight {
    Vec3 direction{0.0f, -1.0f, 0.0f}; // direction the light travels
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool enabled = true;
};

struct PointLight {
    Vec3 position{0.0f, 2.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 10.0f;
};

struct SpotLight {
    Vec3 position{0.0f, 3.0f, 0.0f};
    Vec3 direction{0.0f, -1.0f, 0.0f}; // direction the light travels
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 2.0f;
    float inner_angle_rad = 0.35f;
    float outer_angle_rad = 0.6f;
};

class Renderer3D {
public:
    static constexpr u32 kMaxPointLights = 8;  // must match lighting.frag
    static constexpr u32 kMaxSpotLights = 8;   // must match lighting.frag

    struct Stats {
        u32 extracted = 0;   // objects received in the RenderWorld
        u32 visible = 0;     // after frustum culling
        u32 draw_calls = 0;
        f64 cull_us = 0;         // CPU time spent culling
        f64 draw_prep_us = 0;    // CPU time composing draws
        // Material descriptor sets built this frame. Material instances are
        // shared and their sets are cached, so in steady state this stays at 0
        // regardless of object count — that is the point of the cache, and the
        // number exists so a test can assert it.
        u32 material_sets_built = 0;
    };

    Renderer3D() = default;
    ~Renderer3D();

    Renderer3D(const Renderer3D&) = delete;
    Renderer3D& operator=(const Renderer3D&) = delete;

    /// Loads shaders, builds render passes/pipelines and allocates the
    /// graph-owned targets (Depth, GBuffer0-2, HDR) at the given resolution.
    bool init(rhi::IGraphicsDevice& device, const std::filesystem::path& shader_dir,
              u32 width, u32 height);
    void shutdown();

    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    void resize(u32 width, u32 height);

    /// The renderer resolves RenderObject mesh handles against this library.
    /// Must be set before render(); ownership stays with the caller.
    void set_mesh_library(MeshLibrary* library) { m_mesh_library = library; }

    // --- Materials ---
    MaterialLibrary& materials() { return *m_material_library; }
    Material* gbuffer_material() { return m_gbuffer_material.get(); }

    // --- Lights ---
    void set_directional_light(const DirectionalLight& light) { m_directional = light; }
    const DirectionalLight& directional_light() const { return m_directional; }
    void set_ambient(float ambient) { m_ambient = ambient; }

    u32 add_point_light(const PointLight& light);
    u32 add_spot_light(const SpotLight& light);
    void clear_point_lights() { m_point_lights.clear(); }
    void clear_spot_lights() { m_spot_lights.clear(); }
    PointLight& point_light(u32 index) { return m_point_lights[index]; }
    SpotLight& spot_light(u32 index) { return m_spot_lights[index]; }
    u32 point_light_count() const { return static_cast<u32>(m_point_lights.size()); }
    u32 spot_light_count() const { return static_cast<u32>(m_spot_lights.size()); }

    void set_exposure(float exposure) { m_exposure = exposure; }
    float exposure() const { return m_exposure; }

    /// Records one full frame into `cmd` (which must be in recording state):
    ///   Frustum Culling → DepthPrepass → GBuffer → Lighting → Tonemap.
    /// The tonemapped result lands in `out_target`. When the target is a
    /// swapchain image, pass out_is_present_source=true so the final layout
    /// is presentation-ready.
    bool render(rhi::CommandBuffer& cmd, const RenderWorld& render_world,
                const Camera& camera, rhi::Texture& out_target,
                bool out_is_present_source = false);

    const Stats& last_stats() const { return m_stats; }


    /// Readback-friendly access to intermediate targets for tests. The depth
    /// target carries TransferSrc usage, so copy_texture_to_buffer works.
    rhi::Texture* depth_target() { return m_graph->get_texture(m_depth_handle); }
    rhi::Texture* gbuffer_target(u32 index); // 0 = base color, 1 = normal, 2 = surface
    rhi::Texture* hdr_target() { return m_graph->get_texture(m_hdr_handle); }

private:
    bool create_resolution_dependent(u32 width, u32 height);
    void destroy_resolution_dependent();

    // Frame uniform block — must stay in sync with lighting.frag (std140).
    struct FrameUniforms {
        float inv_view_proj[16];
        float cam_pos_ambient[4];   // xyz camera, w ambient
        float dir_dir_enable[4];    // xyz direction, w enabled
        float dir_color_int[4];     // rgb color, a intensity
        i32 counts[4];              // x points, y spots
        struct PointGPU {
            float pos_radius[4];
            float color_int[4];
            float pad[4];
        };
        struct SpotGPU {
            float pos[4];
            float dir_inner[4];
            float color_int[4];
            float outer_pad[4];
        };
        PointGPU points[kMaxPointLights];
        SpotGPU spots[kMaxSpotLights];
    };
    static_assert(sizeof(FrameUniforms) == 64 + 16 + 16 + 16 + 16 +
                                        kMaxPointLights * 48 + kMaxSpotLights * 64,
                  "FrameUniforms must match the shader's std140 layout");

    rhi::IGraphicsDevice* m_device = nullptr;

    // Resolution-dependent pass resources
    u32 m_width = 0;
    u32 m_height = 0;
    std::unique_ptr<RenderGraph> m_graph; // owns Depth/GBuffer/HDR targets

    // --- shaders / pipelines (owned) ---
    std::unique_ptr<rhi::ShaderModule> m_depth_vs, m_depth_fs;
    std::unique_ptr<rhi::ShaderModule> m_gbuffer_vs, m_gbuffer_fs;
    std::unique_ptr<rhi::ShaderModule> m_lighting_vs, m_lighting_fs;
    std::unique_ptr<rhi::ShaderModule> m_tonemap_vs, m_tonemap_fs;
    PipelineCache* m_pipeline_cache = nullptr;
    std::unique_ptr<Material> m_gbuffer_material;
    rhi::Pipeline* m_depth_pipeline = nullptr;    // owned by m_pipeline_cache
    rhi::Pipeline* m_lighting_pipeline = nullptr; // owned by m_pipeline_cache
    rhi::Pipeline* m_tonemap_pipeline_off = nullptr;
    rhi::Pipeline* m_tonemap_pipeline_present = nullptr;

    // --- descriptor layouts ---
    std::unique_ptr<rhi::DescriptorSetLayout> m_material_layout;   // set 0: UBO + albedo
    std::unique_ptr<rhi::DescriptorSetLayout> m_lighting_layout;   // set 0: 4 textures + UBO
    std::unique_ptr<rhi::DescriptorSetLayout> m_tonemap_layout;    // set 0: HDR texture

    // --- render passes (owned) ---
    std::unique_ptr<rhi::RenderPass> m_depth_rp;
    std::unique_ptr<rhi::RenderPass> m_gbuffer_rp;
    std::unique_ptr<rhi::RenderPass> m_lighting_rp;
    std::unique_ptr<rhi::RenderPass> m_tonemap_rp_off;
    // The present-source variant is created lazily on first use: it needs the
    // swapchain extension, which a headless/CI device does not have — creating
    // it eagerly would fail vkCreateRenderPass there.
    std::unique_ptr<rhi::RenderPass> m_tonemap_rp_present;
    bool m_tonemap_present_ready = false;

    // --- graph-owned targets ---
    RGTextureHandle m_depth_handle = kInvalidRGHandle;
    RGTextureHandle m_gbuffer0_handle = kInvalidRGHandle;
    RGTextureHandle m_gbuffer1_handle = kInvalidRGHandle;
    RGTextureHandle m_gbuffer2_handle = kInvalidRGHandle;
    RGTextureHandle m_hdr_handle = kInvalidRGHandle;

    std::unique_ptr<rhi::TextureView> m_gbuffer0_view, m_gbuffer1_view;
    std::unique_ptr<rhi::TextureView> m_gbuffer2_view, m_gbuffer_depth_view;
    std::unique_ptr<rhi::TextureView> m_hdr_view;
    std::unique_ptr<rhi::Sampler> m_sampler;

    // Framebuffers: depth-only, gbuffer, lighting, and one per output target
    std::unique_ptr<rhi::Framebuffer> m_depth_fb;
    std::unique_ptr<rhi::Framebuffer> m_gbuffer_fb;
    std::unique_ptr<rhi::Framebuffer> m_lighting_fb;
    // Keyed by creation serial, NEVER by raw pointer: heap address reuse
    // across create/destroy cycles used to resurrect framebuffers whose image
    // views were long destroyed (invalid framebuffer + potential GPU hang).
    // Serial 0 (backends without the concept) falls back to pointer identity.
    struct TonemapFBKey {
        u64 tex_serial = 0;
        const rhi::Texture* texture = nullptr;
        bool present_source = false;
        bool operator==(const TonemapFBKey& o) const {
            return tex_serial == o.tex_serial && texture == o.texture &&
                   present_source == o.present_source;
        }
    };
    struct TonemapFBKeyHash {
        size_t operator()(const TonemapFBKey& k) const noexcept {
            size_t h = std::hash<u64>{}(k.tex_serial);
            h ^= reinterpret_cast<size_t>(k.texture) + 0x9E3779B9u + (h << 6) + (h >> 2);
            return h ^ (k.present_source ? 0x9E3779B9u : 0u);
        }
    };
    // Upper bound on cached per-target framebuffers.
    //
    // Serial keys fix invalidation-by-reuse, but they introduce the mirror
    // problem: an entry can never be recognised as dead, because nothing tells
    // the renderer that its texture was destroyed. A process that cycles
    // through many targets at one resolution (tests, thumbnails, tooling)
    // would accumulate framebuffers for textures that no longer exist. A
    // renderer realistically keeps one or two live output targets, so dropping
    // the whole cache on overflow costs one framebuffer rebuild and bounds the
    // growth.
    static constexpr usize kMaxTonemapFramebuffers = 8;
    std::unordered_map<TonemapFBKey, std::unique_ptr<rhi::Framebuffer>, TonemapFBKeyHash> m_tonemap_fbs;

    // Per-frame data
    std::unique_ptr<rhi::DescriptorAllocator> m_descriptor_allocator;
    std::unique_ptr<MaterialLibrary> m_material_library; // created in init()
    std::unique_ptr<rhi::Buffer> m_frame_uniforms; // persistent, updated per frame
    std::vector<u32> m_visible;                    // culled indices into the render world

    // State
    MeshLibrary* m_mesh_library = nullptr;
    DirectionalLight m_directional{};
    std::vector<PointLight> m_point_lights;
    std::vector<SpotLight> m_spot_lights;
    float m_ambient = 0.03f;
    float m_exposure = 1.0f;
    Stats m_stats;

    // White 1x1 fallback texture so scalar-only materials still have a valid
    // albedo binding (the shader multiplies by it, i.e. ignores it).
    std::unique_ptr<rhi::Texture> m_white_texture;
    std::unique_ptr<rhi::TextureView> m_white_view;
};

} // namespace nf::rendering
