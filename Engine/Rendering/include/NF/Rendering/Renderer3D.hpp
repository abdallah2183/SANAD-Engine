#pragma once

// NF/Rendering/Renderer3D.hpp — the integrated Basic 3D pipeline
//
//   RenderWorld → Frustum Culling → Depth Prepass → GBuffer → PBR Lighting
//                                                    → HDR → Exposure → Tonemap → RHI
//
// A purely deferred pipeline holds ONE surface per pixel, so it cannot draw a
// transparent surface: blending into the gbuffer would average the material
// parameters of two surfaces and the lighting pass would light a surface that
// never existed. Surfaces with a base colour alpha below 1 are therefore drawn
// in a forward Transparency pass after Lighting, over the lit HDR image:
//
//   ... → GBuffer → Lighting → Transparency → Tonemap → ...
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
#include <NF/Rendering/LocalShadows.hpp>
#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/ShadowCascades.hpp>
#include <NF/Rendering/Sky.hpp>

#include <cstddef>
#include <array>
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
    // Shadow mapping (Phase 13): depth-tested PCF against a shadow atlas.
    // Strength scales the occlusion 0..1 (1 = full dark).
    bool shadows_enabled = true;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0005f;
    // Cascades fitted around the camera frustum, clamped to
    // [1, kMaxShadowCascades]. 1 is the degenerate single fitted map and is
    // what a scene gets when it wants the cheapest shadows; the default spends
    // the same atlas memory on four ranges that follow the viewer. See
    // ShadowCascades.hpp for why the box is no longer pinned to the origin.
    u32 shadow_cascades = kMaxShadowCascades;
    // Farthest view-space distance shadows are cast to. 0 means the camera's
    // far plane. Capping below the far plane concentrates the atlas on the
    // range that is actually readable — the usual open-world knob.
    float shadow_distance = 0.0f;
};

struct PointLight {
    Vec3 position{0.0f, 2.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 10.0f;
    // Phase 21: shadows from this light. Off by default — a point light costs
    // SIX depth renders, so the game opts in per light rather than paying for
    // every torch in the scene. Lights past kMaxShadowPointLights are lit but
    // unshadowed whatever this says, and shadow_distance caps the projector's
    // far plane (0 = the light's radius).
    bool shadows_enabled = false;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0005f;
    float shadow_distance = 0.0f;
};

struct SpotLight {
    Vec3 position{0.0f, 3.0f, 0.0f};
    Vec3 direction{0.0f, -1.0f, 0.0f}; // direction the light travels
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 2.0f;
    float inner_angle_rad = 0.35f;
    float outer_angle_rad = 0.6f;
    // The cone's reach. This was a hard-coded 25.0 in lighting.frag with a
    // matching kLocalShadowSpotDefaultFar on the CPU — two literals that had to
    // agree and nothing tied together — so a spot could not be given a short
    // reach even in a closet-sized room, and its shadow projector spent depth
    // precision out to 25 units regardless. The default IS that old 25 so every
    // existing scene lights and shadows bit for bit as before; the shader reads
    // it back through SpotGPU::outer_range.y and the shadow projector through
    // the same fallback below.
    float range = kLocalShadowSpotDefaultFar;
    // Phase 21: one depth render per light, so this is cheap to leave on.
    // shadow_distance 0 means `range`, i.e. as far as this light can brighten
    // anything; spending depth precision further out only blurs the shadow.
    bool shadows_enabled = false;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0005f;
    float shadow_distance = 0.0f;
};

// The tone-mapping operator applied to HDR linear colour *before* gamma. It is
// a sibling of exposure, not part of PostFxParams: those run after gamma, this
// one does not. The whole block still reaches tonemap.frag as one float[4]
// push constant (exposure, vignette, saturation, mode), so the mode rides the
// same channel; the shader compares it by range, not equality.
enum class TonemapMode : int {
    // 1 - exp(-x). The shipped default and the *only* mode the golden pixels
    // were authored against; keep it at 0 so an unset block reproduces the old
    // frame bit for bit.
    Exponential = 0,
    ACES       = 1, // Narkowicz's fitted filmic curve.
    Reinhard   = 2, // x / (1 + x), the classic photographic operator.
    Linear     = 3, // exposure only; for diagnostics and HDR capture.
};

// Post-processing applied in the tonemap pass (after gamma). Neutral values
// (saturation 1, vignette 0) are exactly identity, so existing content and
// golden pixels are unaffected until a game opts in.
struct PostFxParams {
    float saturation = 1.0f; // 0 = grayscale, 1 = neutral, >1 = vivid
    float vignette = 0.0f;   // 0 = off .. 1 = strong corner darkening
};

/// CPU mirror of the tonemap operator (pre-gamma). Matches tonemap.frag
/// exactly, including the published ACES constants — do not simplify those,
/// the shader spells the same polynomial.
Vec3 tonemap(Vec3 hdr, float exposure, TonemapMode mode);

/// CPU mirror of the tonemap tail (saturation + vignette on LDR color).
/// Matches tonemap.frag exactly; uv is the fullscreen uv in [0, 1].
Vec3 apply_postfx(Vec3 color, Vec2 uv, const PostFxParams& params);

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
        // Objects routed to the forward Transparency pass (base colour alpha <
        // 1). Zero means the pass recorded no draw and the frame is pixel-
        // identical to one this renderer produced before the pass existed.
        u32 transparent_objects = 0;
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

    // --- Sky (Phase 13): procedural gradient + sun disk, painted by the
    // lighting pass where depth reads far. Pure state: no device needed.
    void set_sky(const SkyParams& sky) { m_sky = sky; }
    const SkyParams& sky() const { return m_sky; }

    u32 add_point_light(const PointLight& light);
    u32 add_spot_light(const SpotLight& light);
    void clear_point_lights() { m_point_lights.clear(); }
    void clear_spot_lights() { m_spot_lights.clear(); }
    PointLight& point_light(u32 index) { return m_point_lights[index]; }
    SpotLight& spot_light(u32 index) { return m_spot_lights[index]; }
    u32 point_light_count() const { return static_cast<u32>(m_point_lights.size()); }
    u32 spot_light_count() const { return static_cast<u32>(m_spot_lights.size()); }

    // --- Distance fog (applied in the lighting and transparency passes) ---
    // Linear fade of a surface toward a haze colour over world-space distance
    // from the camera. Off by default, and the default band sits past every
    // test scene, so a renderer that never calls this renders as before — the
    // same "unset is a no-op" contract as the tonemap mode.
    //
    // Deliberately NOT a SkyParams field: SkyParams is the sky's own state and
    // lives in Sky.hpp, and fog is a surface effect the lighting pass owns.
    // A scene that wants the terrain to fade INTO its horizon should set
    // `color` to the sky's horizon colour itself; the coupling is the caller's,
    // because two scenes disagree about whether haze should match the sky.
    struct FogParams {
        bool enabled = false;
        Vec3 color{0.62f, 0.66f, 0.70f}; // haze tint, linear
        float start = 60.0f;             // distance the fade begins, world units
        float end = 120.0f;              // distance the surface is fully haze
    };

    void set_fog(const FogParams& fog) { m_fog = fog; }
    const FogParams& fog() const { return m_fog; }

    void set_exposure(float exposure) { m_exposure = exposure; }
    float exposure() const { return m_exposure; }

    // --- Tone mapping (pre-gamma operator on HDR colour) ---
    // Default is Exponential, which is what every golden pixel was authored
    // against, so a renderer that never calls this renders as before.
    void set_tonemap_mode(TonemapMode mode) { m_tonemap_mode = mode; }
    TonemapMode tonemap_mode() const { return m_tonemap_mode; }

    // --- Post FX (tonemap tail) ---
    void set_postfx(const PostFxParams& params) { m_postfx = params; }
    const PostFxParams& postfx() const { return m_postfx; }

    // --- Terrain layer splat (design 59: splat maps) ---
    // A surface the mesh has classified carries `uv1 = (slot, blend)`; slot 0
    // is the base material itself, so the palette only answers for the layers
    // above it. Slots are 1..kSplatPaletteLayers-1 and default to white, which
    // is why an author who never calls this renders as before. Flat colours
    // for now; the slot/blend plumbing already reaches the shader, so a
    // per-layer texture array is an additive change to this API.
    static constexpr u32 kSplatPaletteLayers = 8; // must match gbuffer.frag

    void set_splat_layer_color(u32 slot, const Vec3& color);
    Vec3 splat_layer_color(u32 slot) const;

    // --- LOD policy ---
    // max_distances[i] is the farthest camera distance still drawn at lod i
    // (see select_lod). Defaults sit beyond any test scene, so content
    // without LODs — or an author who never touches this — renders exactly
    // as before. Empty bands disable selection entirely (always lod 0).
    void set_lod_max_distances(std::vector<float> bands) { m_lod_max_distances = std::move(bands); }
    const std::vector<float>& lod_max_distances() const { return m_lod_max_distances; }

    // --- Shadow quality ---
    // Global, unlike the per-light count/distance above: these describe the
    // atlas itself. Cascade count and distance are art direction and travel on
    // the light (DirectionalLight::shadow_cascades / ::shadow_distance); tile
    // resolution and the split blend are engine settings that must not differ
    // between two lights sharing one atlas.
    //
    // The tile size may be changed at any time, including after init(): the
    // atlas is rebuilt on the next render(). Default 1024 makes the 2x2 atlas
    // 2048x2048 — byte-for-byte the memory the single shadow map used before
    // cascades existed, now spent on four ranges instead of one.
    void set_shadow_tile_size(u32 tile);
    u32 shadow_tile_size() const { return m_shadow_tile_size; }
    u32 shadow_atlas_size() const { return m_shadow_tile_size * kShadowTileGrid; }
    void set_shadow_lambda(float lambda);
    float shadow_lambda() const { return m_cascades.lambda; }
    void set_shadow_fade_range(float fade_range);
    float shadow_fade_range() const { return m_cascades.fade_range; }

    // --- Local shadow quality (Phase 21) ---
    // Same contract as the cascade tile size, for the point/spot atlas: it is
    // a property of the atlas, not of any one light, and applies on the next
    // render(). Default 512 makes the 7x7 atlas 3584 — larger than the
    // directional one per-tile, because a point light's six tiles each cover a
    // much shorter range than a cascade and a coarse texel there shows up as a
    // hard, crawling shadow edge on a light the player stands next to.
    void set_local_shadow_tile_size(u32 tile);
    u32 local_shadow_tile_size() const { return m_local_shadow_tile_size; }
    u32 local_shadow_atlas_size() const {
        return m_local_shadow_tile_size * kLocalShadowTileGrid;
    }
    /// The effective global cascade settings (sanitised).
    CascadeConfig shadow_config() const { return sanitize_cascade_config(m_cascades); }

    /// Records one full frame into `cmd` (which must be in recording state):
    ///   Frustum Culling → DepthPrepass → GBuffer → Lighting → Transparency
    ///     → Tonemap.
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
        // One world -> shadow-clip transform per cascade. A std140 mat4 array is
        // tightly packed (each element already 16-byte aligned), so this is laid
        // out exactly like four consecutive mat4s.
        float light_view_proj[kMaxShadowCascades][16];
        float shadow_params[4];     // x enabled, y strength, z bias, w texel (1/atlas)
        float cascade_splits[4];    // view-space FAR distance covered by cascade i
        float cascade_info[4];      // x count, y tile uv scale, z fade range, w unused
        float cascade_bias[4];      // per-cascade MINIMUM bias, in NDC (added to z above)
        float cam_forward[4];       // xyz camera forward axis (cascade selection)
        float sky_zenith[4];        // rgb zenith, w unused (Phase 13 sky)
        float sky_horizon[4];       // rgb horizon, w unused
        float sky_ground[4];        // rgb below-horizon, w unused
        float sky_params[4];        // x enabled, y sun disk mul, z sun glow mul, w unused
        float sky_clear[4];         // rgb fallback when the sky is disabled
        // z/w are the local shadow atlas (Phase 21): the shader turns a tile
        // index into atlas uv with the grid and sizes one PCF tap with the tile
        // pixel size, so widening the atlas on the CPU needs no shader edit.
        i32 counts[4];              // x points, y spots, z tile grid, w tile pixels
        struct PointGPU {
            float pos_radius[4];
            float color_int[4];
            float pad[4];
        };
        struct SpotGPU {
            float pos[4];
            float dir_inner[4];
            float color_int[4];
            // x = outer cone cos, y = range. The name is not `outer_pad`
            // anymore because two of the four slots carry real data; yzw were
            // genuinely padding before range moved in, so this costs no extra
            // uniform — the struct is still four vec4s either side of the
            // change, and the offsets below are untouched.
            float outer_range[4];
        };
        PointGPU points[kMaxPointLights];
        SpotGPU spots[kMaxSpotLights];
        // Phase 21: point/spot shadow projectors, one per atlas TILE rather
        // than per light — a point light owns six consecutive tiles and a spot
        // one, so indexing by tile gives the six faces of one light a single
        // lookup rule in the shader. std140 mat4 arrays are tightly packed
        // (every element is already 16-byte aligned), so this is exactly
        // kLocalShadowTileCount consecutive mat4s.
        float local_shadow_view_proj[kLocalShadowTileCount][16];
        // x enabled, y strength, z the tile's DERIVED minimum bias, w the
        // light's own bias. The derived term is per-tile because each tile's
        // texel size and depth span are its own; the artist's term is per-light
        // and duplicated across a point light's six faces. The shader adds the
        // two (see lighting.frag) the way it adds cascade_bias on the
        // directional path.
        float local_shadow_params[kLocalShadowTileCount][4];
        // Fog (distance haze). Appended at the END of the block rather than
        // slotted in by the sky fields: everything above is addressed by
        // offset, and appending moves no existing field. x = enabled is a
        // float because std140 has no bools; the shader compares it.
        float fog_color[4];       // rgb haze tint, w unused
        float fog_params[4];      // x enabled, y start, z end, w unused
    };
    // Per-field offsets rather than one hand-summed total: a field inserted in
    // the middle shifts everything after it, and the sum only notices when the
    // shift happens to change the total. The named offsets say exactly which
    // field drifted, and where the shader expects it.
    static_assert(offsetof(FrameUniforms, inv_view_proj) == 0);
    static_assert(offsetof(FrameUniforms, cam_pos_ambient) == 64);
    static_assert(offsetof(FrameUniforms, dir_dir_enable) == 80);
    static_assert(offsetof(FrameUniforms, dir_color_int) == 96);
    static_assert(offsetof(FrameUniforms, light_view_proj) == 112);
    static_assert(offsetof(FrameUniforms, shadow_params) == 368);
    static_assert(offsetof(FrameUniforms, cascade_splits) == 384);
    static_assert(offsetof(FrameUniforms, cascade_info) == 400);
    static_assert(offsetof(FrameUniforms, cascade_bias) == 416);
    static_assert(offsetof(FrameUniforms, cam_forward) == 432);
    static_assert(offsetof(FrameUniforms, sky_zenith) == 448);
    static_assert(offsetof(FrameUniforms, sky_horizon) == 464);
    static_assert(offsetof(FrameUniforms, sky_ground) == 480);
    static_assert(offsetof(FrameUniforms, sky_params) == 496);
    static_assert(offsetof(FrameUniforms, sky_clear) == 512);
    static_assert(offsetof(FrameUniforms, counts) == 528);
    static_assert(offsetof(FrameUniforms, points) == 544);
    static_assert(offsetof(FrameUniforms, spots) ==
                  544 + kMaxPointLights * 48); // both are 16-byte aligned vec4s
    // The two light arrays are the last fields whose size depends on the LIGHT
    // limits; the local shadow block depends on the TILE count instead, so its
    // offsets are written as "after the light arrays" rather than as a number.
    static constexpr u32 kLocalShadowBlockOffset =
        544 + kMaxPointLights * 48 + kMaxSpotLights * 64;
    static_assert(offsetof(FrameUniforms, local_shadow_view_proj) ==
                  kLocalShadowBlockOffset);
    static_assert(offsetof(FrameUniforms, local_shadow_params) ==
                  kLocalShadowBlockOffset + kLocalShadowTileCount * 64);
    // Appended, so only the total and the two new offsets move.
    static constexpr u32 kFogBlockOffset =
        kLocalShadowBlockOffset + kLocalShadowTileCount * 80;
    static_assert(offsetof(FrameUniforms, fog_color) == kFogBlockOffset);
    static_assert(offsetof(FrameUniforms, fog_params) == kFogBlockOffset + 16);
    static_assert(sizeof(FrameUniforms) == kFogBlockOffset + 32,
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
    std::unique_ptr<rhi::ShaderModule> m_forward_vs, m_forward_fs;
    std::unique_ptr<rhi::ShaderModule> m_tonemap_vs, m_tonemap_fs;
    PipelineCache* m_pipeline_cache = nullptr;
    std::unique_ptr<Material> m_gbuffer_material;
    rhi::Pipeline* m_depth_pipeline = nullptr;    // owned by m_pipeline_cache
    rhi::Pipeline* m_lighting_pipeline = nullptr; // owned by m_pipeline_cache
    rhi::Pipeline* m_forward_pipeline = nullptr;  // owned by m_pipeline_cache
    rhi::Pipeline* m_tonemap_pipeline_off = nullptr;
    rhi::Pipeline* m_tonemap_pipeline_present = nullptr;

    // --- descriptor layouts ---
    std::unique_ptr<rhi::DescriptorSetLayout> m_material_layout;   // set 0: UBO + albedo
    std::unique_ptr<rhi::DescriptorSetLayout> m_lighting_layout;   // set 0: 5 textures + UBO
    std::unique_ptr<rhi::DescriptorSetLayout> m_tonemap_layout;    // set 0: HDR texture
    // The transparency pass needs BOTH the frame/shadow bindings the lighting
    // pass uses AND the material bindings the gbuffer pass uses, but the RHI
    // pipeline takes one layout — so this is the union in a single set. The
    // binding numbers match brdf.glsl exactly (frame UBO 4, cascade atlas 5,
    // local atlas 6) which is what lets one shader file serve both paths; the
    // material pair sits at 7-8 where neither lighting.frag nor the shared
    // include looks.
    std::unique_ptr<rhi::DescriptorSetLayout> m_forward_layout;    // set 0: material + frame

    // --- render passes (owned) ---
    std::unique_ptr<rhi::RenderPass> m_depth_rp;
    std::unique_ptr<rhi::RenderPass> m_gbuffer_rp;
    std::unique_ptr<rhi::RenderPass> m_lighting_rp;
    // Blends over the HDR image Lighting produced. color_load = Load keeps
    // that image (a clear here would blank the whole scene) and the attachment
    // carries blend_enabled, which is where the RHI looks for blend state —
    // not on the pipeline. Depth is Loaded too: the prepass owns it, and a
    // transparent surface tests against the opaque geometry behind it without
    // writing, so two panes can overlap and still sort correctly.
    std::unique_ptr<rhi::RenderPass> m_transparency_rp;
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

    // Framebuffers: depth-only, gbuffer, lighting, transparency, and one per
    // output target
    std::unique_ptr<rhi::Framebuffer> m_depth_fb;
    std::unique_ptr<rhi::Framebuffer> m_gbuffer_fb;
    std::unique_ptr<rhi::Framebuffer> m_lighting_fb;
    std::unique_ptr<rhi::Framebuffer> m_transparency_fb;
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
    SkyParams m_sky{};
    std::vector<PointLight> m_point_lights;
    std::vector<SpotLight> m_spot_lights;
    float m_ambient = 0.03f;
    float m_exposure = 1.0f;
    TonemapMode m_tonemap_mode = TonemapMode::Exponential;
    PostFxParams m_postfx{};
    FogParams m_fog{};
    Stats m_stats;
    std::vector<float> m_lod_max_distances{40.0f, 100.0f, 250.0f};

    // White 1x1 fallback texture so scalar-only materials still have a valid
    // albedo binding (the shader multiplies by it, i.e. ignores it).
    std::unique_ptr<rhi::Texture> m_white_texture;
    std::unique_ptr<rhi::TextureView> m_white_view;

    // Terrain splat palette, bound on every gbuffer material set as binding 2.
    // One std140 array of vec4, mirrored CPU-side so a read-back matches what
    // the shader will see.
    struct SplatPaletteGPU {
        std::array<std::array<float, 4>, kSplatPaletteLayers> layers{};
    };
    SplatPaletteGPU m_splat_palette_cpu{};
    std::unique_ptr<rhi::Buffer> m_splat_palette;

    // Directional shadow atlas: resolution-independent (survives resize without
    // recreation, like the white fallback), but NOT tile-independent — changing
    // the tile size rebuilds it via ensure_shadow_atlas().
    static constexpr u32 kShadowTileSize = 1024;
    static constexpr u32 kShadowAtlasSize = kShadowTileSize * kShadowTileGrid;
    CascadeConfig m_cascades{};
    u32 m_shadow_tile_size = kShadowTileSize;
    // Edge length the LIVE atlas was created at. Tracked separately from
    // m_shadow_tile_size because a tile-size request only takes effect once the
    // rebuild succeeds, and because a failed rebuild must not leave the
    // renderer believing it has an atlas it does not.
    u32 m_shadow_atlas_size = 0;
    std::unique_ptr<rhi::Texture> m_shadow_map;
    std::unique_ptr<rhi::TextureView> m_shadow_view;
    std::unique_ptr<rhi::Framebuffer> m_shadow_fb;

    // Local (point/spot) shadow atlas: a separate texture from the cascade
    // atlas because its grid is 7x7 rather than 2x2 — one texture per light
    // family keeps either's resolution knob from moving the other, and a
    // device that cannot spare the memory for one still gets the other.
    static constexpr u32 kLocalShadowTileSize = 512;
    u32 m_local_shadow_tile_size = kLocalShadowTileSize;
    u32 m_local_shadow_atlas_size = 0;
    std::unique_ptr<rhi::Texture> m_local_shadow_map;
    std::unique_ptr<rhi::TextureView> m_local_shadow_view;
    std::unique_ptr<rhi::Framebuffer> m_local_shadow_fb;

    /// Recreates the directional atlas (texture + view + framebuffer) when it
    /// is missing or was built at a different edge length. Returns false only
    /// if the device refused the resource, in which case the previous atlas is
    /// left intact.
    bool ensure_shadow_atlas(u32 tile_size);
    /// Same contract, for the point/spot atlas.
    bool ensure_local_shadow_atlas(u32 tile_size);
};

} // namespace nf::rendering
