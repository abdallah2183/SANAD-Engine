#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

namespace nf::rendering {

namespace {

std::vector<u8> load_spirv_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<u8> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (static_cast<usize>(f.gcount()) != size) return {};
    return data;
}

/// Whether a sphere MIGHT fall inside one shadow projector's frustum — a
/// conservative cull for the local shadow pass, where drawing every caster into
/// every tile would mean 28 passes over the scene instead of one per light that
/// can actually see it.
///
/// The perspective divide makes a sphere of radius r subtend r/w in clip units,
/// so r/w is the exact screen-space margin and this never rejects a caster that
/// overlaps the frustum. A sphere at or behind the near plane (w <= 0) is kept:
/// straddling the plane is a "too close to call", not a miss. Returned depth is
/// compared against [0, 1] with the same margin, which on a local projector
/// means "beyond the light's reach" — nothing out there is lit, and anything it
/// could shadow is even further out.
bool projector_might_see(const Mat4& view_proj, const BoundingSphere& sphere) {
    const Vec4 clip = view_proj * Vec4{Vec3{sphere.cx, sphere.cy, sphere.cz}, 1.0f};
    if (clip.w <= 0.0f) return true;
    const float inv_w = 1.0f / clip.w;
    const float margin = sphere.radius * inv_w;
    const float nx = clip.x * inv_w;
    const float ny = clip.y * inv_w;
    const float nz = clip.z * inv_w;
    if (nx < -1.0f - margin || nx > 1.0f + margin) return false;
    if (ny < -1.0f - margin || ny > 1.0f + margin) return false;
    if (nz < -margin || nz > 1.0f + margin) return false;
    return true;
}

} // namespace

Renderer3D::~Renderer3D() { shutdown(); }

u32 Renderer3D::add_point_light(const PointLight& light) {
    if (m_point_lights.size() >= kMaxPointLights) {
        NF_LOG_WARN(LogCategory::RHI, "Renderer3D: point light limit reached ({})", kMaxPointLights);
        return u32_max;
    }
    m_point_lights.push_back(light);
    return static_cast<u32>(m_point_lights.size() - 1);
}

u32 Renderer3D::add_spot_light(const SpotLight& light) {
    if (m_spot_lights.size() >= kMaxSpotLights) {
        NF_LOG_WARN(LogCategory::RHI, "Renderer3D: spot light limit reached ({})", kMaxSpotLights);
        return u32_max;
    }
    m_spot_lights.push_back(light);
    return static_cast<u32>(m_spot_lights.size() - 1);
}

rhi::Texture* Renderer3D::gbuffer_target(u32 index) {
    switch (index) {
        case 0: return m_graph->get_texture(m_gbuffer0_handle);
        case 1: return m_graph->get_texture(m_gbuffer1_handle);
        case 2: return m_graph->get_texture(m_gbuffer2_handle);
        case 3: return m_graph->get_texture(m_gbuffer3_handle);
        default: return nullptr;
    }
}

bool Renderer3D::init(rhi::IGraphicsDevice& device, const std::filesystem::path& shader_dir,
                      u32 width, u32 height) {
    shutdown();
    m_device = &device;

    auto sdir = shader_dir;
    if (sdir.empty() || !std::filesystem::exists(sdir)) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: shader dir not found: '{}'", sdir.string());
        return false;
    }

    auto dvs = load_spirv_file(sdir / "depth_vert.spv");
    auto dfs = load_spirv_file(sdir / "depth_frag.spv");
    auto gvs = load_spirv_file(sdir / "gbuffer_vert.spv");
    auto gfs = load_spirv_file(sdir / "gbuffer_frag.spv");
    auto lvs = load_spirv_file(sdir / "lighting_vert.spv");
    auto lfs = load_spirv_file(sdir / "lighting_frag.spv");
    auto fvs = load_spirv_file(sdir / "forward_vert.spv");
    auto ffs = load_spirv_file(sdir / "forward_frag.spv");
    auto tvs = load_spirv_file(sdir / "tonemap_vert.spv");
    auto tfs = load_spirv_file(sdir / "tonemap_frag.spv");
    // Present passthrough is optional: an older shader directory only loses
    // present_texture(), never the scene path.
    auto prvs = load_spirv_file(sdir / "present_vert.spv");
    auto prfs = load_spirv_file(sdir / "present_frag.spv");
    // Bloom chain, optional on the same terms: without bloom_*.spv the stage
    // stays off (bloom_active() reports false) and every other pass renders.
    auto bvs = load_spirv_file(sdir / "bloom_vert.spv");
    auto bfs = load_spirv_file(sdir / "bloom_frag.spv");
    // SSAO pair, optional on the same terms: without ssao_*.spv the stage
    // stays off and the lighting pass reads scalar 1.0 (no occlusion).
    auto svs = load_spirv_file(sdir / "ssao_vert.spv");
    auto sfs = load_spirv_file(sdir / "ssao_frag.spv");
    auto sbfs = load_spirv_file(sdir / "ssao_blur_frag.spv");
    if (dvs.empty() || gvs.empty() || lvs.empty() || tvs.empty()) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to load shaders from '{}'", sdir.string());
        return false;
    }

    m_depth_vs = device.create_shader_module({dvs, rhi::ShaderStage::Vertex});
    m_depth_fs = device.create_shader_module({dfs, rhi::ShaderStage::Fragment});
    m_gbuffer_vs = device.create_shader_module({gvs, rhi::ShaderStage::Vertex});
    m_gbuffer_fs = device.create_shader_module({gfs, rhi::ShaderStage::Fragment});
    m_lighting_vs = device.create_shader_module({lvs, rhi::ShaderStage::Vertex});
    m_lighting_fs = device.create_shader_module({lfs, rhi::ShaderStage::Fragment});
    m_forward_vs = device.create_shader_module({fvs, rhi::ShaderStage::Vertex});
    m_forward_fs = device.create_shader_module({ffs, rhi::ShaderStage::Fragment});
    m_tonemap_vs = device.create_shader_module({tvs, rhi::ShaderStage::Vertex});
    m_tonemap_fs = device.create_shader_module({tfs, rhi::ShaderStage::Fragment});
    if (!prvs.empty() && !prfs.empty()) {
        m_present_vs = device.create_shader_module({prvs, rhi::ShaderStage::Vertex});
        m_present_fs = device.create_shader_module({prfs, rhi::ShaderStage::Fragment});
    }
    if (!bvs.empty() && !bfs.empty()) {
        m_bloom_vs = device.create_shader_module({bvs, rhi::ShaderStage::Vertex});
        m_bloom_fs = device.create_shader_module({bfs, rhi::ShaderStage::Fragment});
    }
    if (!svs.empty() && !sfs.empty() && !sbfs.empty()) {
        m_ssao_vs = device.create_shader_module({svs, rhi::ShaderStage::Vertex});
        m_ssao_fs = device.create_shader_module({sfs, rhi::ShaderStage::Fragment});
        m_ssao_blur_fs = device.create_shader_module({sbfs, rhi::ShaderStage::Fragment});
    }
    if (!m_depth_vs || !m_gbuffer_vs || !m_lighting_vs || !m_forward_vs || !m_tonemap_vs) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create shader modules");
        return false;
    }

    // Static vertex layout shared by every mesh (see Vertex in StaticMesh.hpp)
    static const std::array<rhi::VertexAttrib, 4> kMeshAttribs{{
        {0, offsetof(Vertex, position), rhi::Format::R32G32B32_SFloat},
        {1, offsetof(Vertex, normal), rhi::Format::R32G32B32_SFloat},
        {2, offsetof(Vertex, uv0), rhi::Format::R32G32_SFloat},
        {3, offsetof(Vertex, uv1), rhi::Format::R32G32_SFloat},
    }};

    // --- descriptor layouts ---
    {
        const std::array<rhi::DescriptorBinding, 7> material_binds{{
            {0, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1},
            {1, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
            {2, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1}, // terrain splat palette
            {3, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // normal map
            {4, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // metallic-roughness map
            {5, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // occlusion map
            {6, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // emissive map
        }};
        rhi::DescriptorSetLayoutDesc ld{};
        ld.bindings = std::span<const rhi::DescriptorBinding>(material_binds);
        m_material_layout = device.create_descriptor_set_layout(ld);

        const std::array<rhi::DescriptorBinding, 10> lighting_binds{{
            {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // base
            {1, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // normal
            {2, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // surface
            {3, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // depth
            {4, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1},// frame uniforms
            {5, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // cascade shadow map
            {6, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // point/spot shadow atlas
            {7, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // emissive radiance
            {8, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // sky env bake (IBL)
            {9, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // blurred SSAO
        }};
        rhi::DescriptorSetLayoutDesc lld{};
        lld.bindings = std::span<const rhi::DescriptorBinding>(lighting_binds);
        m_lighting_layout = device.create_descriptor_set_layout(lld);

        // Binding 0 is the HDR image; 1-4 are the bloom levels. They are in
        // ONE set rather than two because the tonemap pass is where the levels
        // are summed, and two sets would mean the pass could be recorded with
        // the bloom set missing — a state that renders a plausible frame with
        // the glow silently absent.
        //
        // 5 and 6 are the depth-of-field inputs: the frame block (for the
        // inverse view-projection that turns a depth value into a view distance)
        // and the depth target itself. They are in the same set for the same
        // reason, and because the stage reads both or neither.
        //
        // NOTE: the present/passthrough pipeline shares this layout, so BOTH
        // descriptor-set write sites must write all seven bindings, with the
        // right TYPE at 5 (a uniform buffer, not an image).
        const std::array<rhi::DescriptorBinding, 8> tonemap_binds{{
            {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // HDR
            {1, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // bloom level 0
            {2, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // bloom level 1
            {3, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // bloom level 2
            {4, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // bloom level 3
            {5, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1}, // frame block
            {6, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // depth target
            {7, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // colour LUT
        }};
        static_assert(tonemap_binds.size() == Renderer3D::kBloomLevels + 4,
                      "HDR + one per bloom level + frame block + depth + colour LUT");
        rhi::DescriptorSetLayoutDesc tld{};
        tld.bindings = std::span<const rhi::DescriptorBinding>(tonemap_binds);
        m_tonemap_layout = device.create_descriptor_set_layout(tld);

        // The bloom chain's own set: one sampled image, the level being read.
        const std::array<rhi::DescriptorBinding, 1> bloom_binds{{
            {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
        }};
        rhi::DescriptorSetLayoutDesc bld{};
        bld.bindings = std::span<const rhi::DescriptorBinding>(bloom_binds);
        m_bloom_layout = device.create_descriptor_set_layout(bld);

        // The union of the lighting and material layouts in one set. Bindings
        // 4-6 are the frame UBO and the two shadow atlases, at the same indices
        // brdf.glsl declares them, so the shared include compiles unchanged in
        // the forward shader; 7-8 are the material pair, at the indices
        // forward.frag declares them — above the gbuffer textures 0-3 that the
        // forward path does not have, since it reconstructs the surface from
        // vertex attributes instead of reading it back out of a target.
        const std::array<rhi::DescriptorBinding, 11> forward_binds{{
            {4, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1},
            {5, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
            {6, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
            {7, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1},
            {8, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
            // The sky env bake (IBL). Binding 9, not 0-3: the forward set has
            // no gbuffer, and 7-8 are the material pair forward.frag declares.
            {9, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
            // The PBR maps (forward.frag 10-13), mirroring the gbuffer set's
            // 3-6 so the two paths sample the same maps.
            {10, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // normal
            {11, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // metallic-roughness
            {12, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // occlusion
            {13, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // emissive
            // Blurred SSAO (forward.frag binding 14). Same treatment as the
            // deferred path: the pane's ambient is occluded like the wall's.
            {14, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // ssao
        }};
        rhi::DescriptorSetLayoutDesc fld{};
        fld.bindings = std::span<const rhi::DescriptorBinding>(forward_binds);
        m_forward_layout = device.create_descriptor_set_layout(fld);
    }
    if (!m_material_layout || !m_lighting_layout || !m_tonemap_layout || !m_forward_layout) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create descriptor layouts");
        return false;
    }
    // SSAO sets (created with the others so every layout exists before any
    // pipeline is built against it). The raw pass reads depth + normal + the
    // frame block (144-byte prefix: invViewProj for reconstruction); the blur
    // reads the raw result guided by depth + normal. Push blocks carry the
    // rest (viewProj for the raw pass; nothing for the blur).
    {
        const std::array<rhi::DescriptorBinding, 3> ssao_binds{{
            {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // depth
            {1, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // normal
            {2, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1}, // frame block
        }};
        rhi::DescriptorSetLayoutDesc sld{};
        sld.bindings = std::span<const rhi::DescriptorBinding>(ssao_binds);
        m_ssao_layout = device.create_descriptor_set_layout(sld);
        const std::array<rhi::DescriptorBinding, 3> ssao_blur_binds{{
            {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // raw AO
            {1, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // depth
            {2, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // normal
        }};
        rhi::DescriptorSetLayoutDesc sbld{};
        sbld.bindings = std::span<const rhi::DescriptorBinding>(ssao_blur_binds);
        m_ssao_blur_layout = device.create_descriptor_set_layout(sbld);
    }
    if (!m_ssao_layout || !m_ssao_blur_layout) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create SSAO layouts");
        return false;
    }

    // --- render passes ---
    {
        rhi::RenderPassDesc depth_rpd{};
        depth_rpd.has_depth = true;
        depth_rpd.depth_format = rhi::Format::D32_SFloat;
        depth_rpd.present_source = false;
        m_depth_rp = device.create_render_pass(depth_rpd);

        rhi::ColorAttachment gcolor{};
        gcolor.format = rhi::Format::R8G8B8A8_UNorm;
        const std::array<rhi::ColorAttachment, 4> gcolor_atts{gcolor, gcolor, gcolor, gcolor};
        rhi::RenderPassDesc gbuffer_rpd{};
        gbuffer_rpd.color_attachments = std::span<const rhi::ColorAttachment>(gcolor_atts);
        gbuffer_rpd.has_depth = true;
        gbuffer_rpd.depth_format = rhi::Format::D32_SFloat;
        // The depth prepass cleared and filled the depth buffer; the gbuffer
        // pass tests against it (depth writes disabled) and must LOAD it — a
        // clear here would silently wipe the prepass output to 1.0 and the
        // lighting pass would reconstruct every pixel as background.
        gbuffer_rpd.depth_load = rhi::RenderPassDesc::DepthLoad::Load;
        gbuffer_rpd.present_source = false;
        m_gbuffer_rp = device.create_render_pass(gbuffer_rpd);

        rhi::ColorAttachment hdr_att{};
        hdr_att.format = rhi::Format::R16G16B16A16_SFloat;
        const std::array<rhi::ColorAttachment, 1> hdr_atts{hdr_att};
        rhi::RenderPassDesc lighting_rpd{};
        lighting_rpd.color_attachments = std::span<const rhi::ColorAttachment>(hdr_atts);
        lighting_rpd.present_source = false;
        m_lighting_rp = device.create_render_pass(lighting_rpd);

        // Transparency: the same HDR attachment, but LOADED rather than cleared
        // (Lighting already wrote it) and blended. blend_enabled lives on the
        // attachment, which is where Pipeline_Vk reads it — the pipeline itself
        // has no blend field — so this attachment description is the whole
        // difference between "opaque HDR target" and "the target glass draws
        // into". SrcAlpha/OneMinusSrcAlpha are the ColorAttachment defaults.
        rhi::ColorAttachment trans_att{};
        trans_att.format = rhi::Format::R16G16B16A16_SFloat;
        trans_att.blend_enabled = true;
        const std::array<rhi::ColorAttachment, 1> trans_atts{trans_att};
        rhi::RenderPassDesc transparency_rpd{};
        transparency_rpd.color_attachments = std::span<const rhi::ColorAttachment>(trans_atts);
        transparency_rpd.color_load = rhi::RenderPassDesc::ColorLoad::Load;
        transparency_rpd.has_depth = true;
        transparency_rpd.depth_format = rhi::Format::D32_SFloat;
        transparency_rpd.depth_load = rhi::RenderPassDesc::DepthLoad::Load;
        transparency_rpd.present_source = false;
        m_transparency_rp = device.create_render_pass(transparency_rpd);

        rhi::ColorAttachment back_off{};
        back_off.format = rhi::Format::R8G8B8A8_UNorm;
        const std::array<rhi::ColorAttachment, 1> off_atts{back_off};
        rhi::RenderPassDesc tonemap_off_rpd{};
        tonemap_off_rpd.color_attachments = std::span<const rhi::ColorAttachment>(off_atts);
        tonemap_off_rpd.present_source = false;
        m_tonemap_rp_off = device.create_render_pass(tonemap_off_rpd);

        // Bloom levels: same format as HDR, cleared (each level is fully
        // written by its own fullscreen draw, so a clear is only about not
        // depending on the previous frame's contents), no depth.
        rhi::ColorAttachment bloom_att{};
        bloom_att.format = rhi::Format::R16G16B16A16_SFloat;
        const std::array<rhi::ColorAttachment, 1> bloom_atts{bloom_att};
        rhi::RenderPassDesc bloom_rpd{};
        bloom_rpd.color_attachments = std::span<const rhi::ColorAttachment>(bloom_atts);
        bloom_rpd.present_source = false;
        m_bloom_rp = device.create_render_pass(bloom_rpd);

        // SSAO pair: single R8 occlusion, cleared (both stages fully write
        // their target), no depth. One render pass serves both, the way the
        // bloom levels share theirs — only the framebuffers differ.
        rhi::ColorAttachment ssao_att{};
        ssao_att.format = rhi::Format::R8_UNorm;
        const std::array<rhi::ColorAttachment, 1> ssao_atts{ssao_att};
        rhi::RenderPassDesc ssao_rpd{};
        ssao_rpd.color_attachments = std::span<const rhi::ColorAttachment>(ssao_atts);
        ssao_rpd.present_source = false;
        m_ssao_rp = device.create_render_pass(ssao_rpd);

        // The present variant is format-agnostic at creation (the swapchain
        // format is fixed at init time by the caller's swapchain, typically
        // B8G8R8A8); a separate pipeline covers it.
    }
    if (!m_depth_rp || !m_gbuffer_rp || !m_lighting_rp || !m_transparency_rp || !m_tonemap_rp_off) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create render passes");
        return false;
    }

    // --- pipelines ---
    m_pipeline_cache = new PipelineCache(device);
    {
        // Depth prepass: position-only vertex input (the mesh buffer carries
        // more attributes; only location 0 is consumed).
        rhi::VertexLayout depth_vl{};
        depth_vl.binding = 0;
        depth_vl.stride = sizeof(Vertex);
        const std::array<rhi::VertexAttrib, 1> depth_attribs{{
            {0, offsetof(Vertex, position), rhi::Format::R32G32B32_SFloat},
        }};
        depth_vl.attributes = std::span<const rhi::VertexAttrib>(depth_attribs);
        rhi::PipelineDesc dpd{};
        dpd.vs = m_depth_vs.get();
        dpd.fs = m_depth_fs.get();
        dpd.render_pass = m_depth_rp.get();
        dpd.vertex_layout = depth_vl;
        dpd.rasterizer.cull_mode = rhi::CullMode::Back;
        // Pairs the Vulkan Y-flip in Mat4::perspective: mirroring Y reverses
        // triangle winding, so Back culling needs the CW face to keep eating
        // the same (back) faces it always ate.
        dpd.rasterizer.front_face = rhi::FrontFace::CW;
        dpd.depth.test_enabled = true;
        dpd.depth.write_enabled = true;
        dpd.push_constant_size = 128; // view_proj + model
        dpd.push_constant_stages = rhi::ShaderStage::Vertex;
        m_depth_pipeline = m_pipeline_cache->get_or_create(dpd);

        // GBuffer: full material pipeline (layout shared with MaterialLibrary)
        rhi::VertexLayout gvl{};
        gvl.binding = 0;
        gvl.stride = sizeof(Vertex);
        gvl.attributes = std::span<const rhi::VertexAttrib>(kMeshAttribs);
        rhi::PipelineDesc gpd{};
        gpd.vs = m_gbuffer_vs.get();
        gpd.fs = m_gbuffer_fs.get();
        gpd.render_pass = m_gbuffer_rp.get();
        gpd.descriptor_set_layout = m_material_layout.get();
        gpd.vertex_layout = gvl;
        gpd.rasterizer.cull_mode = rhi::CullMode::Back;
        gpd.rasterizer.front_face = rhi::FrontFace::CW; // pairs the Y-flip (see above)
        gpd.depth.test_enabled = true;
        gpd.depth.write_enabled = false; // prepass owns the depth buffer
        gpd.depth.compare = rhi::CompareOp::LessEqual;
        gpd.push_constant_size = 128;    // view_proj + model
        gpd.push_constant_stages = rhi::ShaderStage::Vertex;
        MaterialDesc gmd{};
        gmd.vs = gpd.vs;
        gmd.fs = gpd.fs;
        gmd.render_pass = gpd.render_pass;
        gmd.descriptor_set_layout = gpd.descriptor_set_layout;
        gmd.vertex_layout = gpd.vertex_layout;
        gmd.topology = gpd.topology;
        gmd.rasterizer = gpd.rasterizer;
        gmd.depth = gpd.depth;
        gmd.push_constant_size = gpd.push_constant_size;
        gmd.push_constant_stages = gpd.push_constant_stages;
        m_gbuffer_material = std::make_unique<Material>(device, *m_pipeline_cache, gmd);

        // Lighting: fullscreen, no vertex input
        rhi::PipelineDesc lpd{};
        lpd.vs = m_lighting_vs.get();
        lpd.fs = m_lighting_fs.get();
        lpd.render_pass = m_lighting_rp.get();
        lpd.descriptor_set_layout = m_lighting_layout.get();
        lpd.rasterizer.cull_mode = rhi::CullMode::None;
        lpd.depth.test_enabled = false;
        lpd.depth.write_enabled = false;
        m_lighting_pipeline = m_pipeline_cache->get_or_create(lpd);

        // Transparency: forward PBR over the lit HDR image. Same vertex layout
        // and same push constants as the gbuffer pipeline, so a surface lands on
        // the same pixel either way. Depth is TESTED against what the prepass
        // wrote but never written — a pane must not occlude the opaque geometry
        // behind it, and leaving the depth alone is also what lets the back-to-
        // front sort be authoritative rather than the depth buffer.
        rhi::VertexLayout fvl{};
        fvl.binding = 0;
        fvl.stride = sizeof(Vertex);
        // forward.vert reads position/normal/uv0 only, so the splat attribute
        // stops here — declaring an attribute no stage consumes is a validation
        // warning (and would make a transparency surface's uv1 count for
        // nothing: the forward pass has no palette to blend).
        const std::array<rhi::VertexAttrib, 3> forward_attribs{{
            {0, offsetof(Vertex, position), rhi::Format::R32G32B32_SFloat},
            {1, offsetof(Vertex, normal),   rhi::Format::R32G32B32_SFloat},
            {2, offsetof(Vertex, uv0),      rhi::Format::R32G32_SFloat},
        }};
        fvl.attributes = std::span<const rhi::VertexAttrib>(forward_attribs);
        rhi::PipelineDesc fpd{};
        fpd.vs = m_forward_vs.get();
        fpd.fs = m_forward_fs.get();
        fpd.render_pass = m_transparency_rp.get();
        fpd.descriptor_set_layout = m_forward_layout.get();
        fpd.vertex_layout = fvl;
        fpd.rasterizer.cull_mode = rhi::CullMode::Back;
        fpd.rasterizer.front_face = rhi::FrontFace::CW; // pairs the Y-flip (see above)
        fpd.depth.test_enabled = true;
        fpd.depth.write_enabled = false;
        fpd.depth.compare = rhi::CompareOp::LessEqual;
        fpd.push_constant_size = 128;    // view_proj + model
        fpd.push_constant_stages = rhi::ShaderStage::Vertex;
        m_forward_pipeline = m_pipeline_cache->get_or_create(fpd);

        // Tonemap: one pipeline per render pass flavour
        rhi::PipelineDesc tpd{};
        tpd.vs = m_tonemap_vs.get();
        tpd.fs = m_tonemap_fs.get();
        tpd.render_pass = m_tonemap_rp_off.get();
        tpd.descriptor_set_layout = m_tonemap_layout.get();
        tpd.rasterizer.cull_mode = rhi::CullMode::None;
        tpd.depth.test_enabled = false;
        tpd.depth.write_enabled = false;
        tpd.push_constant_size = 96; // 24 floats, see the tonemap push block
        tpd.push_constant_stages = rhi::ShaderStage::Fragment;
        m_tonemap_pipeline_off = m_pipeline_cache->get_or_create(tpd);

        // Bloom chain (optional): one pipeline for both stages — prefilter and
        // downsample are the same fullscreen draw and differ only in the push
        // constant's mode flag (see bloom.frag).
        if (m_bloom_vs && m_bloom_fs && m_bloom_layout && m_bloom_rp) {
            rhi::PipelineDesc bpd{};
            bpd.vs = m_bloom_vs.get();
            bpd.fs = m_bloom_fs.get();
            bpd.render_pass = m_bloom_rp.get();
            bpd.descriptor_set_layout = m_bloom_layout.get();
            bpd.rasterizer.cull_mode = rhi::CullMode::None;
            bpd.depth.test_enabled = false;
            bpd.depth.write_enabled = false;
            bpd.push_constant_size = 32; // 8 floats, see BloomPush
            bpd.push_constant_stages = rhi::ShaderStage::Fragment;
            m_bloom_pipeline = m_pipeline_cache->get_or_create(bpd);
            if (!m_bloom_pipeline) {
                // Not fatal: the stage reports inactive and the frame renders
                // without a glow. Failing init here would take the whole
                // renderer down over one optional post stage.
                NF_LOG_WARN(LogCategory::RHI, "Renderer3D: bloom pipeline creation failed; "
                                              "the bloom stage is disabled");
            }
        }

        // Present passthrough (optional): same fullscreen shape, layout and
        // push-block as tonemap, but the fragment is a straight copy. Lives on
        // the same render passes tonemap uses, so only the fragment differs.
        if (m_present_vs && m_present_fs) {
            rhi::PipelineDesc ppd{};
            ppd.vs = m_present_vs.get();
            ppd.fs = m_present_fs.get();
            ppd.render_pass = m_tonemap_rp_off.get();
            ppd.descriptor_set_layout = m_tonemap_layout.get();
            ppd.rasterizer.cull_mode = rhi::CullMode::None;
            ppd.depth.test_enabled = false;
            ppd.depth.write_enabled = false;
            ppd.push_constant_size = 16; // kept for layout parity with tonemap
            ppd.push_constant_stages = rhi::ShaderStage::Fragment;
            m_present_pipeline_off = m_pipeline_cache->get_or_create(ppd);
        }

        // SSAO pair (optional, like bloom): the raw pass and the blur share
        // the vertex shader and the render pass; only the fragment differs.
        // A missing stage degrades to "no occlusion" via the lighting pass's
        // white fallback, never to a failed init.
        if (m_ssao_vs && m_ssao_fs && m_ssao_layout && m_ssao_rp) {
            rhi::PipelineDesc spd{};
            spd.vs = m_ssao_vs.get();
            spd.fs = m_ssao_fs.get();
            spd.render_pass = m_ssao_rp.get();
            spd.descriptor_set_layout = m_ssao_layout.get();
            spd.rasterizer.cull_mode = rhi::CullMode::None;
            spd.depth.test_enabled = false;
            spd.depth.write_enabled = false;
            spd.push_constant_size = 80; // viewProj mat4 + radius/bias/intensity
            spd.push_constant_stages = rhi::ShaderStage::Fragment;
            m_ssao_pipeline = m_pipeline_cache->get_or_create(spd);
            if (!m_ssao_pipeline) {
                NF_LOG_WARN(LogCategory::RHI, "Renderer3D: SSAO pipeline creation failed; "
                                              "the SSAO stage is disabled");
            }
        }
        if (m_ssao_vs && m_ssao_blur_fs && m_ssao_blur_layout && m_ssao_rp) {
            rhi::PipelineDesc sbpd{};
            sbpd.vs = m_ssao_vs.get();
            sbpd.fs = m_ssao_blur_fs.get();
            sbpd.render_pass = m_ssao_rp.get();
            sbpd.descriptor_set_layout = m_ssao_blur_layout.get();
            sbpd.rasterizer.cull_mode = rhi::CullMode::None;
            sbpd.depth.test_enabled = false;
            sbpd.depth.write_enabled = false;
            sbpd.push_constant_size = 0; // tunables are constants; texel via textureSize()
            sbpd.push_constant_stages = rhi::ShaderStage::Fragment;
            m_ssao_blur_pipeline = m_pipeline_cache->get_or_create(sbpd);
            if (!m_ssao_blur_pipeline) {
                NF_LOG_WARN(LogCategory::RHI, "Renderer3D: SSAO blur pipeline creation failed; "
                                              "the SSAO stage is disabled");
            }
        }

    }
    if (!m_depth_pipeline || !m_gbuffer_material || !m_gbuffer_material->valid() ||
        !m_lighting_pipeline || !m_forward_pipeline || !m_tonemap_pipeline_off) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create pipelines");
        return false;
    }

    // --- per-frame helpers ---
    m_material_library = std::make_unique<MaterialLibrary>(device);
    // One allocator per in-flight slot (see the member comment for why two
    // domains exist); each grows its pools on demand and recycles on reset().
    for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
        m_descriptor_allocators[slot] = device.create_descriptor_allocator(32);
        m_present_allocators[slot] = device.create_descriptor_allocator(8);
    }

    for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
        rhi::BufferDesc frame_ubo_desc{};
        frame_ubo_desc.size = sizeof(FrameUniforms);
        frame_ubo_desc.usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst;
        frame_ubo_desc.memory = rhi::MemoryUsage::CPUToGPU;
        m_frame_uniforms[slot] = device.create_buffer(frame_ubo_desc);
        if (!m_frame_uniforms[slot]) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create frame uniform buffer");
            return false;
        }
    }

    // White fallback texture for scalar-only materials
    rhi::TextureDesc white_desc{};
    white_desc.width = 1;
    white_desc.height = 1;
    white_desc.format = rhi::Format::R8G8B8A8_UNorm;
    white_desc.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
    m_white_texture = device.create_texture(white_desc);
    if (m_white_texture) {
        const u8 white[4] = {255, 255, 255, 255};
        rhi::BufferDesc staging_desc{};
        staging_desc.size = 4;
        staging_desc.usage = rhi::BufferUsage::TransferSrc;
        staging_desc.memory = rhi::MemoryUsage::CPUToGPU;
        auto staging = device.create_buffer(staging_desc);
        if (staging) {
            staging->update(white, 0, 4);
            auto upload = device.create_upload_context();
            if (upload) {
                upload->copy_buffer_to_texture(*staging, *m_white_texture, 0, 0, 0, 1, 1);
                auto fence = upload->submit();
                if (fence) fence->wait();
            }
            // The copy leaves the image in TRANSFER_DST; shaders may only
            // sample it in SHADER_READ.
            auto trans_cmd = device.create_command_buffer();
            auto trans_fence = device.create_fence(false);
            if (trans_cmd && trans_fence) {
                trans_cmd->begin();
                trans_cmd->transition_texture_for_sampling(*m_white_texture);
                trans_cmd->end();
                device.submit(*trans_cmd, rhi::SubmitInfo{.signal_fence = trans_fence.get()});
                trans_fence->wait();
            }
        }
        rhi::TextureViewDesc vd{};
        vd.texture = m_white_texture.get();
        m_white_view = device.create_texture_view(vd);
    }
    rhi::SamplerDesc samp_desc{};
    m_sampler = device.create_sampler(samp_desc);
    if (!m_white_texture || !m_white_view || !m_sampler) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create fallback texture/sampler");
        return false;
    }

    // Sky environment bake target (IBL). Not fatal when the device refuses:
    // the lighting pass falls back to the scalar ambient, so a missing bake
    // dims nothing to black — it just renders the old look.
    if (!ensure_env_map()) {
        NF_LOG_WARN(LogCategory::RHI, "Renderer3D: env map unavailable; IBL falls back to scalar ambient");
    }

    // Terrain splat palette (binding 2 of the material set). One std140 array
    // of vec4 — no staging buffer, no sampler, no layout transition, just a
    // CPU-side mirror plus an update, exactly like the frame uniforms above.
    // Every layer defaults to white so an author who never classifies a
    // surface sees no change at all.
    for (u32 i = 0; i < kSplatPaletteLayers; ++i) {
        m_splat_palette_cpu.layers[i] = {1.0f, 1.0f, 1.0f, 1.0f};
    }
    rhi::BufferDesc splat_desc{};
    splat_desc.size = sizeof(SplatPaletteGPU);
    splat_desc.usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst;
    splat_desc.memory = rhi::MemoryUsage::CPUToGPU;
    m_splat_palette = device.create_buffer(splat_desc);
    if (m_splat_palette) {
        m_splat_palette->update(&m_splat_palette_cpu, 0, sizeof(m_splat_palette_cpu));
    }
    if (!m_splat_palette) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create splat palette buffer");
        return false;
    }

    // Directional shadow atlas (cascades): depth-only target, sampled by the
    // lighting pass. The depth-only render pass already exists above.
    if (!ensure_shadow_atlas(m_shadow_tile_size)) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create shadow atlas");
        return false;
    }

    // --- graph-owned targets ---
    m_graph = std::make_unique<RenderGraph>(device);
    rhi::TextureDesc color_desc{};
    color_desc.width = width;
    color_desc.height = height;
    color_desc.format = rhi::Format::R8G8B8A8_UNorm;
    color_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    m_gbuffer0_handle = m_graph->create_texture("GBuffer0_BaseColor", color_desc);
    m_gbuffer1_handle = m_graph->create_texture("GBuffer1_Normal", color_desc);
    m_gbuffer2_handle = m_graph->create_texture("GBuffer2_Surface", color_desc);
    // Emissive radiance. Its own attachment rather than a channel stolen from
    // another: base colour, normal and surface are all full, and the lighting
    // pass has no per-object data to fall back on — which is why the emission
    // colour was unreachable before this existed.
    m_gbuffer3_handle = m_graph->create_texture("GBuffer3_Emissive", color_desc);

    rhi::TextureDesc hdr_desc = color_desc;
    hdr_desc.format = rhi::Format::R16G16B16A16_SFloat;
    m_hdr_handle = m_graph->create_texture("HDR", hdr_desc);
    create_bloom_textures(width, height);
    create_ssao_textures(width, height);

    rhi::TextureDesc depth_desc{};
    depth_desc.width = width;
    depth_desc.height = height;
    depth_desc.format = rhi::Format::D32_SFloat;
    // TransferSrc so tests can copy the depth aspect back to the CPU.
    depth_desc.usage = rhi::ImageUsage::DepthAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    m_depth_handle = m_graph->create_texture("Depth", depth_desc);

    if (!m_graph->compile()) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to materialize graph targets");
        return false;
    }

    if (!create_resolution_dependent(width, height)) return false;

    NF_LOG_INFO(LogCategory::RHI, "Renderer3D initialized ({}x{})", width, height);
    return true;
}

void Renderer3D::create_ssao_textures(u32 width, u32 height) {
    // Half resolution: occlusion is low frequency once blurred, and quarter
    // pixels means quarter taps. Same R8 single channel the shaders write.
    rhi::TextureDesc desc{};
    desc.width = std::max(1u, width >> 1);
    desc.height = std::max(1u, height >> 1);
    desc.format = rhi::Format::R8_UNorm;
    // TransferSrc for the same reason the bloom levels have it: a test that
    // wants to check the stage's output rather than only its pass list has to
    // be able to copy it back.
    desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    m_ssao_raw_handle = m_graph->create_texture("SSAORaw", desc);
    m_ssao_blur_handle = m_graph->create_texture("SSAOBlur", desc);
}

void Renderer3D::create_bloom_textures(u32 width, u32 height) {
    // Level i is half of level i-1, so level 0 is half the render target and
    // the smallest level is 1/(2^kBloomLevels) of each axis. Clamped to 1 so a
    // 4x4 render target — which the RHI tests really do use — gets four valid
    // 1x1 levels instead of zero-sized textures.
    rhi::TextureDesc desc{};
    desc.format = rhi::Format::R16G16B16A16_SFloat;
    // TransferSrc for the same reason the depth target has it: a test that
    // wants to check the chain's output rather than only its pass list has to
    // be able to copy it back.
    desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    for (u32 level = 0; level < kBloomLevels; ++level) {
        const u32 shift = level + 1;
        desc.width = std::max(1u, width >> shift);
        desc.height = std::max(1u, height >> shift);
        m_bloom_handles[level] =
            m_graph->create_texture("Bloom" + std::to_string(level), desc);
    }
}

bool Renderer3D::create_resolution_dependent(u32 width, u32 height) {
    m_width = width;
    m_height = height;

    rhi::Texture* depth = m_graph->get_texture(m_depth_handle);
    rhi::Texture* gb0 = m_graph->get_texture(m_gbuffer0_handle);
    rhi::Texture* gb1 = m_graph->get_texture(m_gbuffer1_handle);
    rhi::Texture* gb2 = m_graph->get_texture(m_gbuffer2_handle);
    rhi::Texture* gb3 = m_graph->get_texture(m_gbuffer3_handle);
    rhi::Texture* hdr = m_graph->get_texture(m_hdr_handle);
    if (!depth || !gb0 || !gb1 || !gb2 || !gb3 || !hdr) return false;

    rhi::TextureViewDesc vd{};
    vd.dimension = rhi::ViewDimension::View2D;
    vd.aspect = rhi::ImageAspect::Color;
    vd.base_mip = 0;
    vd.mip_count = 1;
    vd.base_layer = 0;
    vd.layer_count = 1;
    vd.texture = gb0;
    m_gbuffer0_view = m_device->create_texture_view(vd);
    vd.texture = gb1;
    m_gbuffer1_view = m_device->create_texture_view(vd);
    vd.texture = gb2;
    m_gbuffer2_view = m_device->create_texture_view(vd);
    vd.texture = gb3;
    m_gbuffer3_view = m_device->create_texture_view(vd);
    vd.texture = hdr;
    m_hdr_view = m_device->create_texture_view(vd);
    vd.texture = depth;
    vd.aspect = rhi::ImageAspect::Depth;
    m_gbuffer_depth_view = m_device->create_texture_view(vd);
    if (!m_gbuffer0_view || !m_gbuffer1_view || !m_gbuffer2_view || !m_gbuffer3_view ||
        !m_hdr_view || !m_gbuffer_depth_view) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create target views");
        return false;
    }

    // Bloom levels: a view each (the chain samples them and the tonemap binds
    // them) and a framebuffer each (they share one render pass, so the
    // framebuffer is the only per-level object).
    for (u32 level = 0; level < kBloomLevels; ++level) {
        rhi::Texture* tex = m_graph->get_texture(m_bloom_handles[level]);
        if (!tex) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: bloom level {} has no texture", level);
            return false;
        }
        rhi::TextureViewDesc bvd{};
        bvd.dimension = rhi::ViewDimension::View2D;
        bvd.aspect = rhi::ImageAspect::Color;
        bvd.base_mip = 0;
        bvd.mip_count = 1;
        bvd.base_layer = 0;
        bvd.layer_count = 1;
        bvd.texture = tex;
        m_bloom_views[level] = m_device->create_texture_view(bvd);
        const std::array<rhi::Texture*, 1> level_colors{tex};
        m_bloom_fbs[level] = m_device->create_framebuffer(
            *m_bloom_rp, std::span<rhi::Texture* const>(level_colors), nullptr);
        if (!m_bloom_views[level] || !m_bloom_fbs[level]) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create bloom level {}", level);
            return false;
        }
    }

    // SSAO pair: a view each (the blur samples the raw, the lighting samples
    // the blur) and a framebuffer each (they share one render pass, like the
    // bloom levels, so the framebuffer is the only per-target object).
    {
        rhi::Texture* raw = m_graph->get_texture(m_ssao_raw_handle);
        rhi::Texture* blur = m_graph->get_texture(m_ssao_blur_handle);
        if (!raw || !blur) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: SSAO targets missing");
            return false;
        }
        auto make_ssao_view = [&](rhi::Texture* tex) {
            rhi::TextureViewDesc svd{};
            svd.dimension = rhi::ViewDimension::View2D;
            svd.aspect = rhi::ImageAspect::Color;
            svd.base_mip = 0;
            svd.mip_count = 1;
            svd.base_layer = 0;
            svd.layer_count = 1;
            svd.texture = tex;
            return m_device->create_texture_view(svd);
        };
        m_ssao_raw_view = make_ssao_view(raw);
        m_ssao_blur_view = make_ssao_view(blur);
        const std::array<rhi::Texture*, 1> raw_colors{raw};
        m_ssao_raw_fb = m_device->create_framebuffer(
            *m_ssao_rp, std::span<rhi::Texture* const>(raw_colors), nullptr);
        const std::array<rhi::Texture*, 1> blur_colors{blur};
        m_ssao_blur_fb = m_device->create_framebuffer(
            *m_ssao_rp, std::span<rhi::Texture* const>(blur_colors), nullptr);
        if (!m_ssao_raw_view || !m_ssao_blur_view || !m_ssao_raw_fb || !m_ssao_blur_fb) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create SSAO views/framebuffers");
            return false;
        }
    }

    const std::array<rhi::Texture*, 0> no_colors{};
    m_depth_fb = m_device->create_framebuffer(*m_depth_rp,
                                              std::span<rhi::Texture* const>(no_colors), depth);
    const std::array<rhi::Texture*, 4> gbuffer_colors{gb0, gb1, gb2, gb3};
    m_gbuffer_fb = m_device->create_framebuffer(*m_gbuffer_rp,
                                                std::span<rhi::Texture* const>(gbuffer_colors), depth);
    const std::array<rhi::Texture*, 1> hdr_colors{hdr};
    m_lighting_fb = m_device->create_framebuffer(*m_lighting_rp,
                                                 std::span<rhi::Texture* const>(hdr_colors), nullptr);
    // The transparency framebuffer sees the SAME HDR image as a colour
    // attachment (the render pass Loads it) plus the prepass depth; the lighting
    // framebuffer has no depth at all because a fullscreen quad does not test
    // one.
    m_transparency_fb = m_device->create_framebuffer(*m_transparency_rp,
                                                     std::span<rhi::Texture* const>(hdr_colors), depth);
    if (!m_depth_fb || !m_gbuffer_fb || !m_lighting_fb || !m_transparency_fb) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create framebuffers");
        return false;
    }
    m_tonemap_fbs.clear();
    return true;
}

void Renderer3D::destroy_resolution_dependent() {
    m_depth_fb.reset();
    m_gbuffer_fb.reset();
    m_lighting_fb.reset();
    m_transparency_fb.reset();
    m_tonemap_fbs.clear();
    m_gbuffer0_view.reset();
    m_gbuffer1_view.reset();
    m_gbuffer2_view.reset();
    m_gbuffer3_view.reset();
    m_gbuffer_depth_view.reset();
    m_hdr_view.reset();
    for (u32 level = 0; level < kBloomLevels; ++level) {
        m_bloom_fbs[level].reset();
        m_bloom_views[level].reset();
    }
    m_ssao_raw_fb.reset();
    m_ssao_blur_fb.reset();
    m_ssao_raw_view.reset();
    m_ssao_blur_view.reset();
}

void Renderer3D::resize(u32 width, u32 height) {
    if (!m_graph || !m_device) return;
    if (width == m_width && height == m_height) return;
    destroy_resolution_dependent();
    m_graph->reset(false);
    m_graph.reset();

    m_graph = std::make_unique<RenderGraph>(*m_device);
    rhi::TextureDesc color_desc{};
    color_desc.width = width;
    color_desc.height = height;
    color_desc.format = rhi::Format::R8G8B8A8_UNorm;
    color_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    m_gbuffer0_handle = m_graph->create_texture("GBuffer0_BaseColor", color_desc);
    m_gbuffer1_handle = m_graph->create_texture("GBuffer1_Normal", color_desc);
    m_gbuffer2_handle = m_graph->create_texture("GBuffer2_Surface", color_desc);
    // Emissive radiance. Its own attachment rather than a channel stolen from
    // another: base colour, normal and surface are all full, and the lighting
    // pass has no per-object data to fall back on — which is why the emission
    // colour was unreachable before this existed.
    m_gbuffer3_handle = m_graph->create_texture("GBuffer3_Emissive", color_desc);
    rhi::TextureDesc hdr_desc = color_desc;
    hdr_desc.format = rhi::Format::R16G16B16A16_SFloat;
    m_hdr_handle = m_graph->create_texture("HDR", hdr_desc);
    create_bloom_textures(width, height);
    create_ssao_textures(width, height);
    rhi::TextureDesc depth_desc{};
    depth_desc.width = width;
    depth_desc.height = height;
    depth_desc.format = rhi::Format::D32_SFloat;
    depth_desc.usage = rhi::ImageUsage::DepthAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    m_depth_handle = m_graph->create_texture("Depth", depth_desc);
    if (!m_graph->compile()) return;

    create_resolution_dependent(width, height);
}

void Renderer3D::shutdown() {
    if (!m_device) return;
    m_device->wait_idle();
    destroy_resolution_dependent();
    for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
        m_frame_uniforms[slot].reset();
        m_descriptor_allocators[slot].reset();
        m_present_allocators[slot].reset();
    }
    m_prepared.clear();
    m_transparent.clear();
    m_forward_sets.clear();
    m_splat_palette.reset();
    m_material_library.reset();
    m_gbuffer_material.reset();
    if (m_pipeline_cache) {
        delete m_pipeline_cache;
        m_pipeline_cache = nullptr;
    }
    m_depth_pipeline = nullptr;
    m_lighting_pipeline = nullptr;
    m_forward_pipeline = nullptr;
    m_tonemap_pipeline_off = nullptr;
    m_tonemap_pipeline_present = nullptr;
    m_bloom_pipeline = nullptr;
    m_ssao_pipeline = nullptr;
    m_ssao_blur_pipeline = nullptr;
    m_present_pipeline_off = nullptr;
    m_present_pipeline_present = nullptr;
    m_graph.reset();
    m_depth_vs.reset(); m_depth_fs.reset();
    m_gbuffer_vs.reset(); m_gbuffer_fs.reset();
    m_lighting_vs.reset(); m_lighting_fs.reset();
    m_forward_vs.reset(); m_forward_fs.reset();
    m_tonemap_vs.reset(); m_tonemap_fs.reset();
    m_present_vs.reset(); m_present_fs.reset();
    m_bloom_vs.reset(); m_bloom_fs.reset();
    m_ssao_vs.reset(); m_ssao_fs.reset(); m_ssao_blur_fs.reset();
    m_material_layout.reset();
    m_lighting_layout.reset();
    m_forward_layout.reset();
    m_tonemap_layout.reset();
    m_bloom_layout.reset();
    m_ssao_layout.reset();
    m_ssao_blur_layout.reset();
    m_depth_rp.reset();
    m_gbuffer_rp.reset();
    m_lighting_rp.reset();
    m_transparency_rp.reset();
    m_tonemap_rp_off.reset();
    m_tonemap_rp_present.reset();
    m_bloom_rp.reset();
    m_ssao_rp.reset();
    m_tonemap_present_ready = false;
    for (u32 level = 0; level < kBloomLevels; ++level) {
        m_bloom_handles[level] = kInvalidRGHandle;
    }
    m_ssao_raw_handle = kInvalidRGHandle;
    m_ssao_blur_handle = kInvalidRGHandle;
    m_white_view.reset();
    m_white_texture.reset();
    m_env_view.reset();
    m_env_texture.reset();
    m_env_sampler.reset();
    m_env_baked_once = false;
    m_env_has_mips = false;
    m_shadow_fb.reset();
    m_shadow_view.reset();
    m_shadow_map.reset();
    m_shadow_atlas_size = 0;
    m_local_shadow_fb.reset();
    m_local_shadow_view.reset();
    m_local_shadow_map.reset();
    m_local_shadow_atlas_size = 0;
    m_sampler.reset();
    m_device = nullptr;
}

void Renderer3D::set_shadow_tile_size(u32 tile) {
    // Zero would make a zero-sized atlas; the rebuild below is what actually
    // applies this, so a later render() picks it up.
    m_shadow_tile_size = (tile == 0) ? 1u : tile;
}

void Renderer3D::set_shadow_lambda(float lambda) {
    m_cascades.lambda = std::clamp(lambda, 0.0f, 1.0f);
}

void Renderer3D::set_shadow_fade_range(float fade_range) {
    m_cascades.fade_range = std::clamp(fade_range, 0.0f, 1.0f);
}

void Renderer3D::set_local_shadow_tile_size(u32 tile) {
    // Same contract as the directional tile size: zero would make a zero-sized
    // atlas, and ensure_local_shadow_atlas() applied on the next render() is
    // what actually rebuilds it.
    m_local_shadow_tile_size = (tile == 0) ? 1u : tile;
}

void Renderer3D::set_splat_layer_color(u32 slot, const Vec3& color) {
    // Slot 0 is the material itself, so the palette only answers for 1..N-1.
    // Out-of-range is clamped onto the nearest real slot rather than ignored so
    // a caller's off-by-one still lands somewhere defined.
    const u32 s = std::min(slot, kSplatPaletteLayers - 1u);
    m_splat_palette_cpu.layers[s] = {color.x, color.y, color.z, 1.0f};
    if (m_device && m_splat_palette) {
        // The renderer may be mid-frame when the scene is authored; the GPU
        // side is only read inside the gbuffer pass of the next render().
        m_device->wait_idle();
        m_splat_palette->update(&m_splat_palette_cpu, 0, sizeof(m_splat_palette_cpu));
    }
}

Vec3 Renderer3D::splat_layer_color(u32 slot) const {
    const u32 s = std::min(slot, kSplatPaletteLayers - 1u);
    const auto& l = m_splat_palette_cpu.layers[s];
    return Vec3{l[0], l[1], l[2]};
}

bool Renderer3D::ensure_shadow_atlas(u32 tile_size) {
    if (!m_device || !m_depth_rp) return false;
    if (tile_size == 0) tile_size = 1;
    const u32 size = tile_size * kShadowTileGrid;
    if (m_shadow_map && m_shadow_view && m_shadow_fb) {
        if (m_shadow_atlas_size == size) return true;
    }

    // Build the replacement BEFORE releasing the old one: a device that refuses
    // the new atlas then leaves the renderer with a working one rather than with
    // none, and m_shadow_atlas_size keeps describing what is actually bound.
    rhi::TextureDesc shadow_desc{};
    shadow_desc.width = size;
    shadow_desc.height = size;
    shadow_desc.format = rhi::Format::D32_SFloat;
    shadow_desc.usage = rhi::ImageUsage::DepthAtt | rhi::ImageUsage::Sampled;
    auto texture = m_device->create_texture(shadow_desc);
    if (!texture) return false;

    rhi::TextureViewDesc svd{};
    svd.dimension = rhi::ViewDimension::View2D;
    svd.aspect = rhi::ImageAspect::Depth;
    svd.base_mip = 0;
    svd.mip_count = 1;
    svd.base_layer = 0;
    svd.layer_count = 1;
    svd.texture = texture.get();
    auto view = m_device->create_texture_view(svd);
    if (!view) return false;

    const std::array<rhi::Texture*, 0> no_colors{};
    auto fb = m_device->create_framebuffer(
        *m_depth_rp, std::span<rhi::Texture* const>(no_colors), texture.get());
    if (!fb) return false;

    // The atlas being replaced may still be referenced by a frame in flight —
    // descriptor sets sampled from its view are only released at the next fence
    // wait, and a tile-size change can arrive mid-frame.
    m_device->wait_idle();
    m_shadow_map = std::move(texture);
    m_shadow_view = std::move(view);
    m_shadow_fb = std::move(fb);
    m_shadow_atlas_size = size;
    m_shadow_tile_size = tile_size;
    return true;
}

bool Renderer3D::ensure_local_shadow_atlas(u32 tile_size) {
    // Same lifecycle and same "build the replacement first" rule as the cascade
    // atlas; only the grid differs (7x7 for 28 tiles). Both atlases share the
    // depth-only render pass and the depth pipeline.
    if (!m_device || !m_depth_rp) return false;
    if (tile_size == 0) tile_size = 1;
    const u32 size = tile_size * kLocalShadowTileGrid;
    if (m_local_shadow_map && m_local_shadow_view && m_local_shadow_fb) {
        if (m_local_shadow_atlas_size == size) return true;
    }

    rhi::TextureDesc desc{};
    desc.width = size;
    desc.height = size;
    desc.format = rhi::Format::D32_SFloat;
    desc.usage = rhi::ImageUsage::DepthAtt | rhi::ImageUsage::Sampled;
    auto texture = m_device->create_texture(desc);
    if (!texture) return false;

    rhi::TextureViewDesc svd{};
    svd.dimension = rhi::ViewDimension::View2D;
    svd.aspect = rhi::ImageAspect::Depth;
    svd.base_mip = 0;
    svd.mip_count = 1;
    svd.base_layer = 0;
    svd.layer_count = 1;
    svd.texture = texture.get();
    auto view = m_device->create_texture_view(svd);
    if (!view) return false;

    const std::array<rhi::Texture*, 0> no_colors{};
    auto fb = m_device->create_framebuffer(
        *m_depth_rp, std::span<rhi::Texture* const>(no_colors), texture.get());
    if (!fb) return false;

    m_device->wait_idle();
    m_local_shadow_map = std::move(texture);
    m_local_shadow_view = std::move(view);
    m_local_shadow_fb = std::move(fb);
    m_local_shadow_atlas_size = size;
    m_local_shadow_tile_size = tile_size;
    return true;
}

bool Renderer3D::ensure_env_map() {
    // The sky bake target: one equirect, half-float HDR, full mip chain for
    // the roughness walk. Created once (resolution-independent), uploaded and
    // mipmapped by refresh_env_map() whenever the sky moves.
    if (m_env_texture && m_env_view && m_env_sampler) return true;
    if (!m_device) return false;
    rhi::TextureDesc desc{};
    desc.width = static_cast<u32>(kSkyEnvWidth);
    desc.height = static_cast<u32>(kSkyEnvHeight);
    desc.mip_levels = static_cast<u32>(kSkyEnvMips);
    desc.format = rhi::Format::R16G16B16A16_SFloat;
    // TransferSrc is required, not just TransferDst: the mip chain is built
    // with blits that READ each level as a blit source. Missing it is a
    // validation error (VUID-vkCmdBlitImage-srcImage-00219) that runs silently
    // without the validation layer and fails loudly with it.
    desc.usage = rhi::ImageUsage::TransferDst | rhi::ImageUsage::TransferSrc |
                 rhi::ImageUsage::Sampled;
    auto texture = m_device->create_texture(desc);
    if (!texture) return false;
    rhi::TextureViewDesc vd{};
    vd.texture = texture.get();
    vd.dimension = rhi::ViewDimension::View2D;
    vd.aspect = rhi::ImageAspect::Color;
    vd.base_mip = 0;
    vd.mip_count = static_cast<u32>(kSkyEnvMips);
    vd.base_layer = 0;
    vd.layer_count = 1;
    auto view = m_device->create_texture_view(vd);
    if (!view) return false;
    // Clamp-to-edge on all axes: the u = 0/1 seam is one texel column apart
    // in the bake, and Repeat would smear the -X direction across it.
    rhi::SamplerDesc sd{};
    sd.mag = rhi::Filter::Linear;
    sd.min = rhi::Filter::Linear;
    sd.mip = rhi::MipMapMode::Linear;
    sd.address_u = rhi::AddressMode::ClampToEdge;
    sd.address_v = rhi::AddressMode::ClampToEdge;
    sd.address_w = rhi::AddressMode::ClampToEdge;
    auto sampler = m_device->create_sampler(sd);
    if (!sampler) return false;
    m_env_texture = std::move(texture);
    m_env_view = std::move(view);
    m_env_sampler = std::move(sampler);
    m_env_baked_once = false;
    return true;
}

bool Renderer3D::refresh_env_map(const SkyParams& sky, const Vec3& sun_dir,
                                 const Vec3& sun_color, bool light_enabled) {
    if (!ensure_env_map()) return false;
    const SkyEnvKey key = SkyEnvKey::make(sky, sun_dir, sun_color, light_enabled);
    if (m_env_baked_once && m_env_baked_key.matches(key)) return true;
    bake_sky_env(m_env_pixels, kSkyEnvWidth, kSkyEnvHeight, sky, sun_dir,
                 sun_color, light_enabled);
    const usize byte_size = m_env_pixels.size() * sizeof(u16);
    rhi::BufferDesc staging_desc{};
    staging_desc.size = byte_size;
    staging_desc.usage = rhi::BufferUsage::TransferSrc;
    staging_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto staging = m_device->create_buffer(staging_desc);
    if (!staging) return false;
    staging->update(m_env_pixels.data(), 0, byte_size);
    // The bake replaces the LIVE texture in place (same object, new
    // contents), so quiesce first: the other in-flight slot may still be
    // sampling it, and uploading under a pending read is a data race. This
    // runs only when the sky actually moved, so the stall is rare by design.
    m_device->wait_idle();
    auto upload = m_device->create_upload_context();
    if (!upload) return false;
    upload->copy_buffer_to_texture(*staging, *m_env_texture, 0, 0, 0,
                                   static_cast<u32>(kSkyEnvWidth),
                                   static_cast<u32>(kSkyEnvHeight));
    auto fence = upload->submit();
    if (!fence || !fence->wait()) return false;
    auto mip_cmd = m_device->create_command_buffer();
    auto mip_fence = m_device->create_fence(false);
    if (!mip_cmd || !mip_fence) return false;
    mip_cmd->begin();
    // Blurs level 0 down the chain for the roughness walk. On success every
    // level ends SHADER_READ; on refusal (format cannot blit) the backend
    // returns BEFORE recording anything, so the buffer below submits empty —
    // a harmless no-op — and the lobes sample level 0: shiny but valid,
    // never black.
    m_env_has_mips = mip_cmd->generate_mipmaps(*m_env_texture);
    mip_cmd->end();
    m_device->submit(*mip_cmd, rhi::SubmitInfo{.signal_fence = mip_fence.get()});
    if (!mip_fence->wait()) return false;
    if (!m_env_has_mips) {
        auto trans = m_device->create_command_buffer();
        auto trans_fence = m_device->create_fence(false);
        if (!trans || !trans_fence) return false;
        trans->begin();
        trans->transition_texture_for_sampling(*m_env_texture);
        trans->end();
        m_device->submit(*trans, rhi::SubmitInfo{.signal_fence = trans_fence.get()});
        if (!trans_fence->wait()) return false;
    }
    m_env_baked_key = key;
    m_env_baked_once = true;
    return true;
}

bool Renderer3D::render(rhi::CommandBuffer& cmd, const RenderWorld& render_world,
                        const Camera& camera, rhi::Texture& out_target,
                        bool out_is_present_source, u32 frame_slot) {
    if (!m_device || !m_graph) return false;
    // Defensive clamp: a caller passing a nonsense slot must not index out of
    // the per-slot arrays. Every shipped caller passes frame % kFramesInFlight.
    if (frame_slot >= kFramesInFlight) frame_slot = 0;

    // Applies a set_shadow_tile_size() issued since the last frame. A no-op in
    // steady state; the atlas is deliberately not rebuilt on resize(), because
    // it does not depend on the output resolution.
    if (!ensure_shadow_atlas(m_shadow_tile_size)) return false;
    if (!ensure_local_shadow_atlas(m_local_shadow_tile_size)) return false;

    // SSAO stage state, decided once like bloom_on: the uniform fill, the
    // lighting binding, and the pass list below must agree, and a predicate
    // recomputed in three places eventually disagrees with itself in one.
    bool ssao_on = m_ssao_enabled && m_ssao_pipeline != nullptr &&
                   m_ssao_blur_pipeline != nullptr && m_ssao_raw_view && m_ssao_blur_view &&
                   m_ssao_raw_fb && m_ssao_blur_fb &&
                   m_ssao_raw_handle != kInvalidRGHandle &&
                   m_ssao_blur_handle != kInvalidRGHandle &&
                   m_graph->get_texture(m_ssao_raw_handle) != nullptr &&
                   m_graph->get_texture(m_ssao_blur_handle) != nullptr;

    // Sky environment bake for image-based lighting. Re-bakes only when the
    // sky or the sun moved (SkyEnvKey epsilon); the first frame always bakes.
    // env_ready false means "read the scalar ambient" — the shader's
    // ibl_params.x guard — so a refused texture can never black the scene.
    bool env_ready = false;
    if (m_ibl_enabled) {
        const Vec3 sun_dir = Vec3{-m_directional.direction.x, -m_directional.direction.y,
                                  -m_directional.direction.z}
                                 .normalized();
        const Vec3 sun_col = m_directional.color * m_directional.intensity;
        env_ready = refresh_env_map(m_sky, sun_dir, sun_col, m_directional.enabled);
    }

    // --- Frustum culling: RenderWorld → Visible Objects ---
    nf::Clock cull_clock;
    cull_render_world(render_world, camera, m_visible);
    m_stats.cull_us = cull_clock.elapsed_us();
    m_stats.extracted = static_cast<u32>(render_world.objects.size());
    m_stats.visible = static_cast<u32>(m_visible.size());
    m_stats.draw_calls = 0;
    m_stats.material_sets_built = 0;
    m_stats.transparent_objects = 0;
    m_stats.bloom_levels_recorded = 0;

    // --- Frame uniforms (camera + lights) ---
    FrameUniforms fu{};
    const Mat4 inv_vp = camera.view_projection.inverse();
    std::memcpy(fu.inv_view_proj, inv_vp.m, sizeof(fu.inv_view_proj));
    fu.cam_pos_ambient[0] = camera.position.x;
    fu.cam_pos_ambient[1] = camera.position.y;
    fu.cam_pos_ambient[2] = camera.position.z;
    fu.cam_pos_ambient[3] = m_ambient;
    // The previous frame's view-projection, or the CURRENT one before any frame
    // has been rendered — so the first frame reports zero velocity instead of a
    // jump from the identity matrix, which would smear the whole image on the
    // frame a scene opens.
    {
        const Mat4& prev = m_has_prev_view_proj ? m_prev_view_proj : camera.view_projection;
        std::memcpy(fu.prev_view_proj, prev.m, sizeof(fu.prev_view_proj));
    }
    fu.dir_dir_enable[0] = m_directional.direction.x;
    fu.dir_dir_enable[1] = m_directional.direction.y;
    fu.dir_dir_enable[2] = m_directional.direction.z;
    fu.dir_dir_enable[3] = m_directional.enabled ? 1.0f : 0.0f;
    fu.dir_color_int[0] = m_directional.color.x;
    fu.dir_color_int[1] = m_directional.color.y;
    fu.dir_color_int[2] = m_directional.color.z;
    fu.dir_color_int[3] = m_directional.intensity;
    // Cascaded shadow transforms, each fitted around the slice of THIS camera's
    // frustum it covers. The previous code built one ortho box around the world
    // origin with the light parked at `dir * -20`, so walking the camera more
    // than ~12 units from (0,0,0) silently deleted every shadow in the scene.
    // Row-vector order matches update_camera (view * projection), so the shader
    // consumes each matrix the same way it consumes the camera's.
    const CascadeConfig cfg = sanitize_cascade_config(m_cascades);
    const u32 cascade_count = std::clamp(m_directional.shadow_cascades, 1u, kMaxShadowCascades);
    // The atlas always holds kMaxShadowCascades tiles; cascades past the active
    // count must still hold a VALID matrix, because the shader's blend reads
    // cascade i+1 and a zero matrix would blank the shadow instead of lighting
    // it. Duplicating the last real cascade is the safe filler.
    CascadeFit fits[kMaxShadowCascades];
    {
        // Shadow reach: the light's own cap, itself capped by the far plane the
        // camera actually renders to, so we never fit a range nothing draws.
        float far_z = camera.far_plane;
        if (m_directional.shadow_distance > 0.0f) {
            far_z = std::min(far_z, m_directional.shadow_distance);
        }
        // Keep the range non-degenerate for the split maths; a far plane at or
        // inside the near plane is a broken camera, not a reason to emit NaNs.
        if (!(far_z > camera.near_plane)) far_z = camera.near_plane * 1.001f;

        float splits[kMaxShadowCascades + 1]{};
        compute_cascade_splits(camera.near_plane, far_z, cascade_count, cfg.lambda, splits);

        for (u32 i = 0; i < cascade_count; ++i) {
            fits[i] = fit_cascade(camera, m_directional.direction, splits[i], splits[i + 1],
                                  m_shadow_tile_size, cfg.caster_extrusion);
            fu.cascade_splits[i] = splits[i + 1];
        }
        for (u32 i = cascade_count; i < kMaxShadowCascades; ++i) {
            fits[i] = fits[cascade_count - 1];
            fu.cascade_splits[i] = splits[cascade_count];
        }
        // Each cascade gets its OWN minimum bias, derived from its texel size,
        // because that minimum is a world distance and the cascades do not share
        // one. The artist's shadow_bias is added on top of it by the shader as
        // extra, which keeps that field's documented range and meaning intact.
        for (u32 i = 0; i < kMaxShadowCascades; ++i) {
            fu.cascade_bias[i] = cascade_auto_bias(fits[i], m_shadow_tile_size);
        }
        for (u32 i = 0; i < kMaxShadowCascades; ++i) {
            std::memcpy(fu.light_view_proj[i], fits[i].light_view_proj.m,
                        sizeof(fu.light_view_proj[i]));
        }
    }
    fu.cascade_info[0] = static_cast<float>(cascade_count);
    fu.cascade_info[1] = cascade_tile_scale();
    fu.cascade_info[2] = cfg.fade_range;
    fu.cascade_info[3] = 0.0f;
    {
        // Cascade selection happens in the shader, which only has world
        // positions — so it needs the camera's forward axis to turn one into a
        // view-space distance. Falls back to -Z (the unrotated camera basis)
        // when the target coincides with the eye, where there is no axis.
        Vec3 fwd = camera.target - camera.position;
        const float flen = fwd.length();
        fwd = (flen > 1e-6f) ? fwd / flen : Vec3{0.0f, 0.0f, -1.0f};
        fu.cam_forward[0] = fwd.x;
        fu.cam_forward[1] = fwd.y;
        fu.cam_forward[2] = fwd.z;
        fu.cam_forward[3] = 0.0f;
    }
    const bool shadows_on = m_directional.enabled && m_directional.shadows_enabled;
    fu.shadow_params[0] = shadows_on ? 1.0f : 0.0f;
    fu.shadow_params[1] = m_directional.shadow_strength;
    fu.shadow_params[2] = m_directional.shadow_bias;
    fu.shadow_params[3] = 1.0f / static_cast<float>(m_shadow_atlas_size);
    // Procedural sky (Phase 13): std140 vec4s straight from SkyParams.
    fu.sky_zenith[0] = m_sky.zenith.x; fu.sky_zenith[1] = m_sky.zenith.y;
    fu.sky_zenith[2] = m_sky.zenith.z; fu.sky_zenith[3] = 0.0f;
    fu.sky_horizon[0] = m_sky.horizon.x; fu.sky_horizon[1] = m_sky.horizon.y;
    fu.sky_horizon[2] = m_sky.horizon.z; fu.sky_horizon[3] = 0.0f;
    fu.sky_ground[0] = m_sky.ground.x; fu.sky_ground[1] = m_sky.ground.y;
    fu.sky_ground[2] = m_sky.ground.z; fu.sky_ground[3] = 0.0f;
    fu.sky_params[0] = m_sky.enabled ? 1.0f : 0.0f;
    fu.sky_params[1] = m_sky.sun_disk;
    fu.sky_params[2] = m_sky.sun_glow;
    fu.sky_params[3] = 0.0f;
    fu.sky_clear[0] = m_sky.clear.x; fu.sky_clear[1] = m_sky.clear.y;
    fu.sky_clear[2] = m_sky.clear.z; fu.sky_clear[3] = 1.0f;
    // Procedural clouds: coverage, altitude, scale, time. Coverage 0 is exactly
    // "no layer", so an unset sky is the gradient it always was.
    fu.sky_cloud[0] = m_sky.cloud_coverage;
    fu.sky_cloud[1] = m_sky.cloud_altitude;
    fu.sky_cloud[2] = m_sky.cloud_scale;
    fu.sky_cloud[3] = m_sky.cloud_time;
    // IBL uniforms (appended last — see the header). w is the specular walk's
    // top lod: full chain when the bake blurred, 0 when it did not.
    fu.ibl_params[0] = env_ready ? 1.0f : 0.0f;
    fu.ibl_params[1] = m_ibl_diffuse;
    fu.ibl_params[2] = m_ibl_specular;
    fu.ibl_params[3] =
        (env_ready && m_env_has_mips) ? static_cast<float>(kSkyEnvMips - 1) : 0.0f;
    // SSAO uniforms (appended last). x rides the stage state so the lighting
    // pass never samples a target that was not rendered this frame.
    fu.ssao_params[0] = ssao_on ? 1.0f : 0.0f;
    fu.ssao_params[1] = m_ssao_intensity;
    fu.ssao_params[2] = m_ssao_radius;
    fu.ssao_params[3] = m_ssao_bias;
    fu.counts[0] = static_cast<i32>(m_point_lights.size());
    fu.counts[1] = static_cast<i32>(m_spot_lights.size());
    for (u32 i = 0; i < m_point_lights.size() && i < kMaxPointLights; ++i) {
        const PointLight& l = m_point_lights[i];
        fu.points[i].pos_radius[0] = l.position.x;
        fu.points[i].pos_radius[1] = l.position.y;
        fu.points[i].pos_radius[2] = l.position.z;
        fu.points[i].pos_radius[3] = l.radius;
        fu.points[i].color_int[0] = l.color.x;
        fu.points[i].color_int[1] = l.color.y;
        fu.points[i].color_int[2] = l.color.z;
        fu.points[i].color_int[3] = l.intensity;
    }
    for (u32 i = 0; i < m_spot_lights.size() && i < kMaxSpotLights; ++i) {
        const SpotLight& l = m_spot_lights[i];
        fu.spots[i].pos[0] = l.position.x;
        fu.spots[i].pos[1] = l.position.y;
        fu.spots[i].pos[2] = l.position.z;
        fu.spots[i].dir_inner[0] = l.direction.x;
        fu.spots[i].dir_inner[1] = l.direction.y;
        fu.spots[i].dir_inner[2] = l.direction.z;
        fu.spots[i].dir_inner[3] = std::cos(l.inner_angle_rad);
        fu.spots[i].color_int[0] = l.color.x;
        fu.spots[i].color_int[1] = l.color.y;
        fu.spots[i].color_int[2] = l.color.z;
        fu.spots[i].color_int[3] = l.intensity;
        fu.spots[i].outer_range[0] = std::cos(l.outer_angle_rad);
        // The falloff window the shader windows the cone by. Clamped away from
        // zero on both sides: a negative or NaN reach would make the window
        // comparison meaningless rather than simply "no light".
        fu.spots[i].outer_range[1] = l.range > 0.0f ? l.range
                                                    : kLocalShadowSpotDefaultFar;
    }

    // Local (point/spot) shadow projectors, one per atlas tile. fu is
    // value-initialised above, so every tile starts DISABLED with a zero
    // matrix and only the tiles a light actually claims are written — the
    // shader's enabled check is what makes an unshadowed light free.
    //
    // The assignment and the fits come from plan_local_shadows rather than
    // being restated here: the tiles stay indexed by LIGHT index (point i owns
    // [6i, 6i+6)) even when an earlier light opts out, and that rule has to be
    // the same one the shader inverts, so it lives in the header where the
    // shader's face-selection comment can point at it — and where a CPU test
    // can exercise it. Restating it here would let the two drift apart the way
    // an earlier inlined copy did (it capped tiles by a count of opt-ins, so
    // one light opting out handed a later light tiles past the 28 that exist).
    const LocalShadowPlan local_plan =
        plan_local_shadows(m_point_lights, m_spot_lights, m_local_shadow_tile_size);
    for (u32 t = 0; t < kLocalShadowTileCount; ++t) {
        const LocalShadowTile& tile = local_plan.tiles[t];
        std::memcpy(fu.local_shadow_view_proj[t], tile.view_proj.m,
                    sizeof(fu.local_shadow_view_proj[t]));
        fu.local_shadow_params[t][0] = tile.enabled;
        fu.local_shadow_params[t][1] = tile.strength;
        fu.local_shadow_params[t][2] = tile.auto_bias;
        fu.local_shadow_params[t][3] = tile.artist_bias;
    }
    fu.counts[2] = static_cast<i32>(kLocalShadowTileGrid);
    // The PCF tap stride and the half-texel clamp are expressed in atlas uv, so
    // the shader needs the tile's pixel size alongside the grid: texel_uv =
    // (1/grid) / tile_pixels.
    fu.counts[3] = static_cast<i32>(m_local_shadow_tile_size);

    // Fog. Appended at the end of the block, so this writes two trailing vec4s
    // and touches nothing above. The enabled flag is uploaded as a float because
    // the shader compares it against 0.5 — std140 has no bools, and a bool
    // uniform read back as a float is driver-dependent in exactly the direction
    // that turns "fog is off" into "fog is everywhere".
    fu.fog_color[0] = m_fog.color.x;
    fu.fog_color[1] = m_fog.color.y;
    fu.fog_color[2] = m_fog.color.z;
    fu.fog_color[3] = 1.0f;
    fu.fog_params[0] = m_fog.enabled ? 1.0f : 0.0f;
    fu.fog_params[1] = m_fog.start;
    fu.fog_params[2] = m_fog.end;
    fu.fog_params[3] = 0.0f;
    m_frame_uniforms[frame_slot]->update(&fu, 0, sizeof(fu));

    // --- Per-frame descriptor sets ---
    // Contract: the GPU work previously recorded through THIS slot has
    // completed (the caller waited the slot's fence), so recycling the slot's
    // allocator here cannot touch in-flight descriptor sets. The OTHER slot
    // may still be executing — that is the whole point of two slots.
    m_descriptor_allocators[frame_slot]->reset();

    // Material sets are allocated lazily per visible object below; allocate
    // lighting/tonemap sets once.
    auto lighting_set = m_descriptor_allocators[frame_slot]->allocate(*m_lighting_layout);
    auto tonemap_set = m_descriptor_allocators[frame_slot]->allocate(*m_tonemap_layout);
    if (!lighting_set || !tonemap_set) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: descriptor allocation failed");
        return false;
    }
    // SSAO sets (raw + blur), held until execute() returns like the bloom
    // sets. Allocated only when the stage records this frame.
    std::unique_ptr<rhi::DescriptorSet> ssao_set;
    std::unique_ptr<rhi::DescriptorSet> ssao_blur_set;
    if (ssao_on) {
        ssao_set = m_descriptor_allocators[frame_slot]->allocate(*m_ssao_layout);
        ssao_blur_set = m_descriptor_allocators[frame_slot]->allocate(*m_ssao_blur_layout);
        if (!ssao_set || !ssao_blur_set) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: SSAO descriptor allocation failed");
            return false;
        }
        // Clamp-to-edge sampling: spiral/blur taps past the screen edge must
        // clamp, not wrap (m_sampler repeats). The env sampler has the right
        // state; the white fallback never applies here (views are confirmed).
        const rhi::Sampler* clamp_sampler = m_env_sampler ? m_env_sampler.get() : m_sampler.get();
        const std::array<rhi::DescriptorWrite, 3> ssao_writes{{
            {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
             m_gbuffer_depth_view.get(), clamp_sampler},
            {1, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
             m_gbuffer1_view.get(), clamp_sampler},
            {2, rhi::DescriptorType::UniformBuffer, m_frame_uniforms[frame_slot].get(), 0, 144,
             nullptr, nullptr},
        }};
        m_device->update_descriptor_set(*ssao_set,
                                        std::span<const rhi::DescriptorWrite>(ssao_writes));
        const std::array<rhi::DescriptorWrite, 3> ssao_blur_writes{{
            {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
             m_ssao_raw_view.get(), clamp_sampler},
            {1, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
             m_gbuffer_depth_view.get(), clamp_sampler},
            {2, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
             m_gbuffer1_view.get(), clamp_sampler},
        }};
        m_device->update_descriptor_set(*ssao_blur_set,
                                        std::span<const rhi::DescriptorWrite>(ssao_blur_writes));
    }

    // Whether the bloom chain records this frame. Decided once, here, because
    // three separate things depend on it — the tonemap descriptor writes, the
    // tonemap pass's read set, and the pass list itself — and a predicate
    // recomputed in three places is a predicate that will eventually disagree
    // with itself in one of them.
    bool bloom_on = m_postfx.bloom.enabled && m_bloom_pipeline != nullptr;
    if (bloom_on) {
        for (u32 level = 0; level < kBloomLevels; ++level) {
            if (!m_bloom_views[level] || !m_bloom_fbs[level] ||
                m_graph->get_texture(m_bloom_handles[level]) == nullptr) {
                bloom_on = false;
                break;
            }
        }
    }
    // The chain's own descriptor sets (one per level: each pass reads the level
    // above it). Held in this scope until m_graph->execute() returns, the same
    // contract the forward sets follow.
    std::array<std::unique_ptr<rhi::DescriptorSet>, kBloomLevels> bloom_sets{};
    if (bloom_on) {
        for (u32 level = 0; level < kBloomLevels; ++level) {
            bloom_sets[level] = m_descriptor_allocators[frame_slot]->allocate(*m_bloom_layout);
            if (!bloom_sets[level]) {
                NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: bloom descriptor allocation failed");
                bloom_on = false;
                break;
            }
        }
    }
    if (m_postfx.bloom.enabled && !bloom_on) {
        // Not an error: an optional stage whose resources are missing degrades
        // to "no glow", the same contract present_texture follows. TRACE rather
        // than WARN because this is a per-frame condition, and a warning here
        // would print once per frame for the rest of the process.
        NF_LOG_TRACE(LogCategory::RHI, "Renderer3D: bloom requested but unavailable this frame");
    }

    {
        // The env bake, or the white texture when there is no bake: an
        // unwritten binding is not a valid set, and the ibl_params.x guard
        // means the substitute is never sampled.
        const rhi::TextureView* env_view =
            (env_ready && m_env_view) ? m_env_view.get() : m_white_view.get();
        const rhi::Sampler* env_sampler =
            (env_ready && m_env_sampler) ? m_env_sampler.get() : m_sampler.get();
        // Same rule for the blurred SSAO (white = 1.0 = no occlusion), bound
        // once the stage below confirms it rendered this frame.
        const std::array<rhi::DescriptorWrite, 10> lighting_writes{{
            {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer0_view.get(), m_sampler.get()},
            {1, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer1_view.get(), m_sampler.get()},
            {2, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer2_view.get(), m_sampler.get()},
            {3, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer_depth_view.get(), m_sampler.get()},
            {4, rhi::DescriptorType::UniformBuffer, m_frame_uniforms[frame_slot].get(), 0, sizeof(FrameUniforms), nullptr, nullptr},
            {5, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_shadow_view.get(), m_sampler.get()},
            {6, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_local_shadow_view.get(), m_sampler.get()},
            {7, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer3_view.get(), m_sampler.get()},
            {8, rhi::DescriptorType::SampledImage, nullptr, 0, 0, env_view, env_sampler},
            {9, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
             (m_ssao_blur_view && fu.ssao_params[0] > 0.5f) ? m_ssao_blur_view.get() : m_white_view.get(),
             m_sampler.get()},
        }};
        m_device->update_descriptor_set(*lighting_set, std::span<const rhi::DescriptorWrite>(lighting_writes));

        // Binding 0 is the HDR image; 1..kBloomLevels are the bloom levels.
        // With bloom off the HDR view stands in for every level: the tonemap
        // shader's `bloom_intensity > 0` guard means they are never read, so
        // the substitution cannot change a pixel — but it does keep every
        // binding a VALID image, which a set with an unwritten binding is not.
        std::array<rhi::DescriptorWrite, 4 + kBloomLevels> tonemap_writes{};
        tonemap_writes[0] = {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                             m_hdr_view.get(), m_sampler.get()};
        for (u32 level = 0; level < kBloomLevels; ++level) {
            const rhi::TextureView* view =
                bloom_on ? m_bloom_views[level].get() : m_hdr_view.get();
            tonemap_writes[1 + level] = {1 + level, rhi::DescriptorType::SampledImage,
                                         nullptr, 0, 0, view, m_sampler.get()};
        }
        // The depth-of-field and motion-blur inputs. The range covers exactly
        // what tonemap.frag declares — `invViewProj` (64 B) + `cam_pos_ambient`
        // (16 B) + `prevViewProj` (64 B) = 144 B — because a std140 block may
        // describe a PREFIX of the buffer, and declaring the other forty floats
        // again would be a second copy of FrameUniforms to keep in sync for
        // three members.
        tonemap_writes[1 + kBloomLevels] = {1 + kBloomLevels, rhi::DescriptorType::UniformBuffer,
                                            m_frame_uniforms[frame_slot].get(), 0, 144,
                                            nullptr, nullptr};
        // The depth view, which the lighting pass already samples. Bound even
        // when DOF is off: an unwritten binding is not a valid set, and the
        // shader's `dof_max_radius > 0` guard means it is then never read.
        tonemap_writes[2 + kBloomLevels] = {2 + kBloomLevels, rhi::DescriptorType::SampledImage,
                                            nullptr, 0, 0, m_gbuffer_depth_view.get(),
                                            m_sampler.get()};
        // The colour-grading LUT, or the 1x1 white texture when none is bound.
        // Bound unconditionally for the usual reason: an unwritten binding is
        // not a valid set, and `lut_strength` at 0 means the substitute is
        // never sampled.
        tonemap_writes[3 + kBloomLevels] = {
            3 + kBloomLevels, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
            m_color_lut_view != nullptr ? m_color_lut_view : m_white_view.get(), m_sampler.get()};
        m_device->update_descriptor_set(*tonemap_set, std::span<const rhi::DescriptorWrite>(tonemap_writes));

        if (bloom_on) {
            // Level i's pass reads level i-1; level 0's reads the HDR target.
            for (u32 level = 0; level < kBloomLevels; ++level) {
                const rhi::TextureView* src_view =
                    (level == 0) ? m_hdr_view.get() : m_bloom_views[level - 1].get();
                const std::array<rhi::DescriptorWrite, 1> writes{{
                    {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0, src_view, m_sampler.get()},
                }};
                m_device->update_descriptor_set(*bloom_sets[level],
                                                std::span<const rhi::DescriptorWrite>(writes));
            }
        }
    }

    // --- Draw submission preparation: resolve handles, allocate per-object
    // material descriptor sets. Changing material parameters never lands here
    // as pipeline work — parameters live in the instances' UBOs.
    nf::Clock prep_clock;
    // Scratch vectors are members (see Renderer3D.hpp): the capacity survives
    // frames, so a 5000-object scene stops reallocating 5000 entries per frame.
    std::vector<PreparedDraw>& prepared = m_prepared;
    prepared.clear();
    prepared.reserve(m_visible.size());
    std::vector<TransparentDraw>& transparent = m_transparent;
    transparent.clear();
    // Owns this frame's forward descriptor sets. The transparency pass is
    // recorded below as a graph lambda that only keeps raw pointers, so the
    // sets have to live here until m_graph->execute() returns; the allocator
    // that handed them out is not reset until this slot runs again, after the
    // slot's fence.
    std::vector<std::unique_ptr<rhi::DescriptorSet>>& forward_sets = m_forward_sets;
    forward_sets.clear();
    forward_sets.reserve(m_visible.size());
    for (u32 idx : m_visible) {
        const RenderObject& ro = render_world.objects[idx];
        if (!ro.mesh_handle.valid() || !m_mesh_library) continue;
        const StaticMesh* mesh = m_mesh_library->get(ro.mesh_handle);
        if (!mesh || !mesh->is_uploaded()) continue;
        if (mesh->lods().empty()) continue;

        PreparedDraw pd{&ro, mesh, nullptr, 0};
        float camera_distance = 0.0f;
        // Distance LOD, resolved once per frame (not per pass): the authored
        // lod is a floor, distance only coarsens, and the result is clamped
        // into range — a stale lod on a poorer mesh draws the coarsest LOD
        // instead of indexing out of bounds.
        {
            const float dx = ro.sphere.cx - camera.position.x;
            const float dy = ro.sphere.cy - camera.position.y;
            const float dz = ro.sphere.cz - camera.position.z;
            camera_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const u32 distance_lod = select_lod(camera_distance,
                                                static_cast<u32>(mesh->lods().size()),
                                                std::span<const float>(m_lod_max_distances));
            u32 effective = ro.lod > distance_lod ? ro.lod : distance_lod;
            if (effective >= mesh->lods().size()) {
                effective = static_cast<u32>(mesh->lods().size()) - 1;
            }
            pd.lod = effective;
        }
        MaterialEntry* entry = m_material_library->get(ro.material_handle);
        if (entry && entry->material && entry->material->valid()) {
            if (entry->params.base_color[3] < 1.0f) {
                // The transparent path. Its descriptor set is the union layout
                // — material pair AND the frame/shadow bindings the shared BRDF
                // reads — built per object per frame. That is deliberate where
                // the opaque path caches: a transparent object carries its own
                // alpha, so two objects sharing one material instance still
                // differ, and the expected count here is a handful of panes.
                auto fwd_set = m_descriptor_allocators[frame_slot]->allocate(*m_forward_layout);
                if (!fwd_set) {
                    NF_LOG_ERROR(LogCategory::RHI,
                                 "Renderer3D: forward descriptor allocation failed");
                    continue;
                }
                const rhi::TextureView* albedo =
                    entry->albedo_view ? entry->albedo_view : m_white_view.get();
                // Binding 9 is the env bake (forward.frag), with the same
                // valid-substitute rule as the lighting set above.
                const rhi::TextureView* fwd_env =
                    (env_ready && m_env_view) ? m_env_view.get() : m_white_view.get();
                const rhi::Sampler* fwd_env_sampler =
                    (env_ready && m_env_sampler) ? m_env_sampler.get() : m_sampler.get();
                const std::array<rhi::DescriptorWrite, 11> forward_writes{{
                    {4, rhi::DescriptorType::UniformBuffer, m_frame_uniforms[frame_slot].get(), 0,
                     sizeof(FrameUniforms), nullptr, nullptr},
                    {5, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                     m_shadow_view.get(), m_sampler.get()},
                    {6, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                     m_local_shadow_view.get(), m_sampler.get()},
                    {7, rhi::DescriptorType::UniformBuffer, entry->params_ubo.get(), 0,
                     16 * sizeof(float), nullptr, nullptr},
                    {8, rhi::DescriptorType::SampledImage, nullptr, 0, 0, albedo, m_sampler.get()},
                    {9, rhi::DescriptorType::SampledImage, nullptr, 0, 0, fwd_env, fwd_env_sampler},
                    {10, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                     entry->normal_view ? entry->normal_view : m_white_view.get(), m_sampler.get()},
                    {11, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                     entry->mrough_view ? entry->mrough_view : m_white_view.get(), m_sampler.get()},
                    {12, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                     entry->occlusion_view ? entry->occlusion_view : m_white_view.get(), m_sampler.get()},
                    {13, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                     entry->emissive_view ? entry->emissive_view : m_white_view.get(), m_sampler.get()},
                    {14, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                     (m_ssao_blur_view && fu.ssao_params[0] > 0.5f) ? m_ssao_blur_view.get()
                                                                    : m_white_view.get(),
                     m_sampler.get()},
                }};
                m_device->update_descriptor_set(*fwd_set,
                                                std::span<const rhi::DescriptorWrite>(forward_writes));
                transparent.push_back(TransparentDraw{&ro, mesh, fwd_set.get(), pd.lod,
                                                      camera_distance});
                forward_sets.push_back(std::move(fwd_set));
                continue;
            }
            // One set per material instance, reused every frame. Objects share
            // material instances, so building per object meant N allocations
            // and N vkUpdateDescriptorSets per frame for a handful of distinct
            // materials. Rebuilding is safe here because the frame fence has
            // already been waited on above, so nothing in flight references the
            // set being replaced.
            if (entry->set_dirty || !entry->cached_set) {
                entry->cached_set = m_device->create_descriptor_set(*m_material_layout);
                if (!entry->cached_set) {
                    NF_LOG_ERROR(LogCategory::RHI,
                                 "Renderer3D: material descriptor set creation failed");
                    continue;
                }
                const rhi::TextureView* albedo =
                    entry->albedo_view ? entry->albedo_view : m_white_view.get();
                // Unbound map slots read the white fallback: an unwritten
                // binding is not a valid set, and the mapFlags guard means the
                // substitute is never sampled.
                const rhi::TextureView* map_normal =
                    entry->normal_view ? entry->normal_view : m_white_view.get();
                const rhi::TextureView* map_mrough =
                    entry->mrough_view ? entry->mrough_view : m_white_view.get();
                const rhi::TextureView* map_occlusion =
                    entry->occlusion_view ? entry->occlusion_view : m_white_view.get();
                const rhi::TextureView* map_emissive =
                    entry->emissive_view ? entry->emissive_view : m_white_view.get();
                const std::array<rhi::DescriptorWrite, 7> material_writes{{
                    {0, rhi::DescriptorType::UniformBuffer, entry->params_ubo.get(), 0,
                     16 * sizeof(float), nullptr, nullptr},
                    {1, rhi::DescriptorType::SampledImage, nullptr, 0, 0, albedo, m_sampler.get()},
                    {2, rhi::DescriptorType::UniformBuffer, m_splat_palette.get(), 0,
                     sizeof(SplatPaletteGPU), nullptr, nullptr},
                    {3, rhi::DescriptorType::SampledImage, nullptr, 0, 0, map_normal, m_sampler.get()},
                    {4, rhi::DescriptorType::SampledImage, nullptr, 0, 0, map_mrough, m_sampler.get()},
                    {5, rhi::DescriptorType::SampledImage, nullptr, 0, 0, map_occlusion, m_sampler.get()},
                    {6, rhi::DescriptorType::SampledImage, nullptr, 0, 0, map_emissive, m_sampler.get()},
                }};
                m_device->update_descriptor_set(*entry->cached_set,
                                                std::span<const rhi::DescriptorWrite>(material_writes));
                entry->set_dirty = false;
                ++m_stats.material_sets_built;
            }
            pd.material_set = entry->cached_set.get();
        }
        prepared.push_back(pd);
    }
    m_stats.transparent_objects = static_cast<u32>(transparent.size());
    // Farthest first: alpha blending is not commutative, and the depth test
    // does not save us here because a transparent surface never WRITES depth,
    // so a near pane drawn before a far one has nothing behind it yet. The sort
    // is by the bounding sphere, which is a proxy for the surface — two
    // interpenetrating panes are still wrong, and only per-fragment depth
    // peeling would fix that; the engine does not do it, and neither does
    // anything else shipping a single forward transparency pass.
    std::sort(transparent.begin(), transparent.end(),
              [](const TransparentDraw& a, const TransparentDraw& b) {
                  return a.distance > b.distance;
              });
    // Opaque draw order: state sort by (mesh, lod, material set). Kit scenes —
    // the whole reason this renderer exists — draw the same mesh hundreds of
    // times and share a handful of material instances, so grouping by state
    // turns "bind VB/IB + descriptor set per object" into "bind per group".
    // The sort is stable on nothing else by design: depth order is provided by
    // the depth prepass, and the gbuffer pass writes no blend, so any
    // permutation produces the same pixels.
    // The sort is stable on nothing else by design: depth order is provided by
    // the depth prepass, and the gbuffer pass writes no blend, so any
    // permutation produces the same pixels for non-coplanar surfaces.
    // Coplanar surfaces compare EQUAL in depth; the gbuffer's LessEqual test
    // passes both and the last-drawn fragment wins — that is inherent to
    // z-fighting and is the scene author's problem, not the sort's (see the
    // pane z in test_3d_renderer.cpp for the fix on the test side).
    std::sort(prepared.begin(), prepared.end(),
              [](const PreparedDraw& a, const PreparedDraw& b) {
                  if (a.mesh != b.mesh) return a.mesh < b.mesh;
                  if (a.lod != b.lod) return a.lod < b.lod;
                  return a.material_set < b.material_set;
              });
    m_stats.draw_prep_us = prep_clock.elapsed_us();
    // --- Build the frame graph ---
    m_graph->reset(true);
    auto rg_out = m_graph->import_texture("Output", &out_target);
    // The shadow map is a standalone fixed-size texture; importing it lets
    // the graph transition it (depth write in the shadow pass, fragment read
    // in lighting) with the same barriers as owned targets.
    auto rg_shadow = m_graph->import_texture("ShadowAtlas", m_shadow_map.get());
    auto rg_local_shadow =
        m_graph->import_texture("LocalShadowAtlas", m_local_shadow_map.get());

    // Shadow atlas (depth from the light). All cascades are rendered inside ONE
    // pass: begin_render_pass clears the whole atlas once, then each cascade
    // gets its own viewport/scissor + light matrix and re-draws the same caster
    // list. One pass means one barrier in the graph, and it is legal because the
    // backend issues a straight vkCmdSetViewport/vkCmdSetScissor per call.
    //
    // Layered rendering would be tidier, but VulkanFramebuffer binds
    // texture->view(), which is a single-layer View2D — an array target needs an
    // RHI change for no gain at four cascades. See ShadowCascades.hpp.
    RGPassDesc shadow_pass{};
    shadow_pass.name = "ShadowAtlas";
    shadow_pass.depth_attachment = rg_shadow;
    shadow_pass.execute = [&](rhi::CommandBuffer& gcmd) {
        const std::array<rhi::ClearValue, 0> no_clears{};
        gcmd.begin_render_pass(*m_depth_rp, *m_shadow_fb, std::span<const rhi::ClearValue>(no_clears), 1.0f, 0);
        gcmd.bind_pipeline(*m_depth_pipeline);
        struct Push { float view_proj[16]; float model[16]; };
        Push push{};
        static_assert(sizeof(fu.light_view_proj[0]) == sizeof(push.view_proj));
        const u32 tile = m_shadow_tile_size;
        // Bind state survives the per-cascade viewport changes (only the
        // scissor/viewport and push constants differ), so the mesh binds are
        // reused across the whole pass while the sorted draw order repeats it.
        const StaticMesh* bound_mesh = nullptr;
        u32 bound_lod = u32_max;
        for (u32 cascade = 0; cascade < cascade_count; ++cascade) {
            const u32 col = cascade % kShadowTileGrid;
            const u32 row = cascade / kShadowTileGrid;
            std::memcpy(push.view_proj, fu.light_view_proj[cascade], sizeof(push.view_proj));
            gcmd.set_viewport(col * tile, row * tile, tile, tile);
            gcmd.set_scissor(col * tile, row * tile, tile, tile);
            for (auto& pd : prepared) {
                // A cascade only pays for casters its own ortho box can receive
                // depth from: parallel light rays map light-space (x, y) 1:1 to
                // where a shadow lands, so a caster outside the window casts
                // outside the window, and one beyond the far plane casts past
                // the map. Skipping both is pixel-identical to drawing them and
                // letting the GPU clip — without this the pass is one FULL
                // scene render per cascade (four per frame with the default
                // count), which is what made many-mesh scenes crawl.
                if (!cascade_keeps_caster(fits[cascade], pd.object->sphere.cx,
                                          pd.object->sphere.cy, pd.object->sphere.cz,
                                          pd.object->sphere.radius)) {
                    continue;
                }
                std::memcpy(push.model, pd.object->world.m, sizeof(push.model));
                gcmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(Push), &push);
                const rhi::Buffer* vb = pd.mesh->vertex_buffer(pd.lod);
                const rhi::Buffer* ib = pd.mesh->index_buffer(pd.lod);
                if (!vb || !ib) continue;
                if (bound_mesh != pd.mesh || bound_lod != pd.lod) {
                    const std::array<const rhi::Buffer*, 1> vbs{vb};
                    gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
                    gcmd.bind_index_buffer(*ib, 0);
                    bound_mesh = pd.mesh;
                    bound_lod = pd.lod;
                }
                const MeshLOD& lod = pd.mesh->lods()[pd.lod];
                for (const SubMesh& sm : lod.submeshes) {
                    gcmd.draw_indexed(sm.index_count, 1, sm.index_offset, static_cast<i32>(sm.vertex_offset), 0);
                }
            }
        }
        gcmd.end_render_pass();
    };
    m_graph->add_pass(shadow_pass);

    // Local shadow atlas (point/spot). One pass over 28 tiles the same way the
    // cascade pass is one pass over four: begin_render_pass clears the whole
    // atlas once, then each ENABLED tile gets its own viewport/scissor and its
    // own projector. Tiles with no shadowed light behind them are skipped and
    // stay clear, so the pass costs one clear when nothing casts.
    //
    // The pass runs even with every tile disabled (rather than being omitted)
    // because binding 6 samples this texture unconditionally: a never-written
    // imported texture has no defined content to sample, and the shader's
    // enabled flag is a per-tile judgement, not a guarantee the binding is safe.
    RGPassDesc local_shadow_pass{};
    local_shadow_pass.name = "LocalShadowAtlas";
    local_shadow_pass.depth_attachment = rg_local_shadow;
    local_shadow_pass.execute = [&](rhi::CommandBuffer& gcmd) {
        const std::array<rhi::ClearValue, 0> no_clears{};
        gcmd.begin_render_pass(*m_depth_rp, *m_local_shadow_fb,
                               std::span<const rhi::ClearValue>(no_clears), 1.0f, 0);
        gcmd.bind_pipeline(*m_depth_pipeline);
        struct Push { float view_proj[16]; float model[16]; };
        Push push{};
        static_assert(sizeof(fu.local_shadow_view_proj[0]) == sizeof(push.view_proj));
        const u32 tile = m_local_shadow_tile_size;
        const StaticMesh* bound_mesh = nullptr;
        u32 bound_lod = u32_max;
        for (u32 t = 0; t < kLocalShadowTileCount; ++t) {
            if (fu.local_shadow_params[t][0] <= 0.5f) continue; // nobody's tile
            const u32 col = t % kLocalShadowTileGrid;
            const u32 row = t / kLocalShadowTileGrid;
            std::memcpy(push.view_proj, fu.local_shadow_view_proj[t], sizeof(push.view_proj));
            gcmd.set_viewport(col * tile, row * tile, tile, tile);
            gcmd.set_scissor(col * tile, row * tile, tile, tile);
            for (auto& pd : prepared) {
                // A tile only pays for casters its own projector can see: at
                // 28 tiles the difference between "every caster" and "the ones
                // in this frustum" is the pass's cost.
                if (!projector_might_see(local_plan.tiles[t].view_proj,
                                         pd.object->sphere)) continue;
                std::memcpy(push.model, pd.object->world.m, sizeof(push.model));
                gcmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(Push), &push);
                const rhi::Buffer* vb = pd.mesh->vertex_buffer(pd.lod);
                const rhi::Buffer* ib = pd.mesh->index_buffer(pd.lod);
                if (!vb || !ib) continue;
                if (bound_mesh != pd.mesh || bound_lod != pd.lod) {
                    const std::array<const rhi::Buffer*, 1> vbs{vb};
                    gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
                    gcmd.bind_index_buffer(*ib, 0);
                    bound_mesh = pd.mesh;
                    bound_lod = pd.lod;
                }
                const MeshLOD& lod = pd.mesh->lods()[pd.lod];
                for (const SubMesh& sm : lod.submeshes) {
                    gcmd.draw_indexed(sm.index_count, 1, sm.index_offset,
                                      static_cast<i32>(sm.vertex_offset), 0);
                }
            }
        }
        gcmd.end_render_pass();
    };
    m_graph->add_pass(local_shadow_pass);

    // Depth prepass
    RGPassDesc depth_pass{};
    depth_pass.name = "DepthPrepass";
    depth_pass.depth_attachment = m_depth_handle;
    depth_pass.execute = [&](rhi::CommandBuffer& gcmd) {
        const std::array<rhi::ClearValue, 0> no_clears{};
        gcmd.begin_render_pass(*m_depth_rp, *m_depth_fb, std::span<const rhi::ClearValue>(no_clears), 1.0f, 0);
        gcmd.bind_pipeline(*m_depth_pipeline);
        struct Push { float view_proj[16]; float model[16]; };
        Push push{};
        std::memcpy(push.view_proj, camera.view_projection.m, sizeof(push.view_proj));
        gcmd.set_viewport(0, 0, m_width, m_height);
        gcmd.set_scissor(0, 0, m_width, m_height);
        const StaticMesh* bound_mesh = nullptr;
        u32 bound_lod = u32_max;
        for (auto& pd : prepared) {
            std::memcpy(push.model, pd.object->world.m, sizeof(push.model));
            gcmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(Push), &push);
            const rhi::Buffer* vb = pd.mesh->vertex_buffer(pd.lod);
            const rhi::Buffer* ib = pd.mesh->index_buffer(pd.lod);
            if (!vb || !ib) continue;
            if (bound_mesh != pd.mesh || bound_lod != pd.lod) {
                const std::array<const rhi::Buffer*, 1> vbs{vb};
                gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
                gcmd.bind_index_buffer(*ib, 0);
                bound_mesh = pd.mesh;
                bound_lod = pd.lod;
            }
            const MeshLOD& lod = pd.mesh->lods()[pd.lod];
            for (const SubMesh& sm : lod.submeshes) {
                gcmd.draw_indexed(sm.index_count, 1, sm.index_offset, static_cast<i32>(sm.vertex_offset), 0);
                ++m_stats.draw_calls;
            }
        }
        gcmd.end_render_pass();
    };
    m_graph->add_pass(depth_pass);

    // GBuffer
    RGPassDesc gbuffer_pass{};
    gbuffer_pass.name = "GBuffer";
    gbuffer_pass.color_attachments = {m_gbuffer0_handle, m_gbuffer1_handle, m_gbuffer2_handle,
                                      m_gbuffer3_handle};
    gbuffer_pass.depth_attachment = m_depth_handle;
    gbuffer_pass.execute = [&](rhi::CommandBuffer& gcmd) {
        // The emissive attachment clears to BLACK, which is the correct
        // radiance for an unlit surface — a surface that never writes it (or a
        // pixel no geometry covers) must contribute no glow, not the previous
        // frame's.
        const std::array<rhi::ClearValue, 4> clears{
            rhi::ClearValue{0.0f, 0.0f, 0.0f, 1.0f},
            rhi::ClearValue{0.0f, 0.0f, 0.0f, 0.0f},
            rhi::ClearValue{0.0f, 0.0f, 0.0f, 0.0f},
            rhi::ClearValue{0.0f, 0.0f, 0.0f, 0.0f},
        };
        gcmd.begin_render_pass(*m_gbuffer_rp, *m_gbuffer_fb, std::span<const rhi::ClearValue>(clears), 1.0f, 0);
        gcmd.bind_pipeline(m_gbuffer_material->pipeline());
        struct Push { float view_proj[16]; float model[16]; };
        Push push{};
        std::memcpy(push.view_proj, camera.view_projection.m, sizeof(push.view_proj));
        gcmd.set_viewport(0, 0, m_width, m_height);
        gcmd.set_scissor(0, 0, m_width, m_height);
        const StaticMesh* bound_mesh = nullptr;
        u32 bound_lod = u32_max;
        const rhi::DescriptorSet* bound_set = nullptr;
        for (auto& pd : prepared) {
            if (!pd.material_set) continue;
            std::memcpy(push.model, pd.object->world.m, sizeof(push.model));
            gcmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(Push), &push);
            // The state sort groups objects by (mesh, lod, material set), so a
            // kit scene's hundreds of same-mesh same-material pieces collapse
            // to one bind per group instead of one bind per object.
            if (bound_set != pd.material_set) {
                const std::array<const rhi::DescriptorSet*, 1> sets{pd.material_set};
                gcmd.bind_descriptor_sets(*m_material_layout, std::span<const rhi::DescriptorSet* const>(sets), 0);
                bound_set = pd.material_set;
            }
            const rhi::Buffer* vb = pd.mesh->vertex_buffer(pd.lod);
            const rhi::Buffer* ib = pd.mesh->index_buffer(pd.lod);
            if (!vb || !ib) continue;
            if (bound_mesh != pd.mesh || bound_lod != pd.lod) {
                const std::array<const rhi::Buffer*, 1> vbs{vb};
                gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
                gcmd.bind_index_buffer(*ib, 0);
                bound_mesh = pd.mesh;
                bound_lod = pd.lod;
            }
            const MeshLOD& lod = pd.mesh->lods()[pd.lod];
            for (const SubMesh& sm : lod.submeshes) {
                gcmd.draw_indexed(sm.index_count, 1, sm.index_offset, static_cast<i32>(sm.vertex_offset), 0);
                ++m_stats.draw_calls;
            }
        }
        gcmd.end_render_pass();
    };
    m_graph->add_pass(gbuffer_pass);

    // Lighting: GBuffer + Depth + ShadowMap → HDR
    RGPassDesc lighting_pass{};
    lighting_pass.name = "Lighting";
    lighting_pass.reads = {m_gbuffer0_handle, m_gbuffer1_handle, m_gbuffer2_handle,
                           m_gbuffer3_handle, m_depth_handle,
                           rg_shadow, rg_local_shadow};
    // The blurred SSAO joins the read set only when the stage rendered it:
    // the graph orders lighting after the blur from this edge, and the
    // barrier lands on it for the same reason. Unconditional would pin a
    // never-written texture into every frame's barrier list.
    if (ssao_on) {
        lighting_pass.reads.push_back(m_ssao_blur_handle);
    }
    lighting_pass.color_attachments = {m_hdr_handle};
    lighting_pass.execute = [&](rhi::CommandBuffer& gcmd) {
        const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{0.0f, 0.0f, 0.0f, 1.0f}};
        gcmd.begin_render_pass(*m_lighting_rp, *m_lighting_fb, std::span<const rhi::ClearValue>(clears));
        gcmd.bind_pipeline(*m_lighting_pipeline);
        const std::array<const rhi::DescriptorSet*, 1> sets{lighting_set.get()};
        gcmd.bind_descriptor_sets(*m_lighting_layout, std::span<const rhi::DescriptorSet* const>(sets), 0);
        gcmd.set_viewport(0, 0, m_width, m_height);
        gcmd.set_scissor(0, 0, m_width, m_height);
        gcmd.draw(3);
        gcmd.end_render_pass();
    };
    m_graph->add_pass(lighting_pass);

    // SSAO pair (raw half-res hemisphere + bilateral blur). Recorded only
    // when the stage is on — the lighting binding stays valid either way
    // (white fallback + ssao_params.x guard). The graph orders them from the
    // read/write sets: raw after the gbuffer, blur after raw, lighting after
    // blur. Half viewports: occlusion is low frequency once blurred.
    if (ssao_on) {
        struct SsaoPush {
            float view_proj[16];
            float radius;
            float bias;
            float intensity;
            float unused;
        };
        static_assert(sizeof(SsaoPush) == 80, "must match ssao.frag's push block");
        SsaoPush push{};
        std::memcpy(push.view_proj, camera.view_projection.m, sizeof(push.view_proj));
        push.radius = m_ssao_radius;
        push.bias = m_ssao_bias;
        push.intensity = m_ssao_intensity;
        push.unused = 0.0f;
        const u32 ssao_w = std::max(1u, m_width >> 1);
        const u32 ssao_h = std::max(1u, m_height >> 1);

        RGPassDesc ssao_pass{};
        ssao_pass.name = "SSAORaw";
        ssao_pass.reads = {m_depth_handle, m_gbuffer1_handle};
        ssao_pass.color_attachments = {m_ssao_raw_handle};
        // `push`, `ssao_w` and `ssao_h` ride BY VALUE (not by the [&] around
        // it): the locals die at the end of this block but the lambda runs at
        // execute() far below. A reference capture reads dead stack there —
        // in practice a zero viewport, which renders nothing and fails
        // SILENTLY (white clears survive, lighting multiplies by 1.0, every
        // test passes vacuously). This exact dangling-capture is what made
        // the stage a no-op.
        ssao_pass.execute = [&, push, ssao_w, ssao_h](rhi::CommandBuffer& gcmd) {
            const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{1.0f, 1.0f, 1.0f, 1.0f}};
            gcmd.begin_render_pass(*m_ssao_rp, *m_ssao_raw_fb,
                                   std::span<const rhi::ClearValue>(clears));
            gcmd.bind_pipeline(*m_ssao_pipeline);
            const std::array<const rhi::DescriptorSet*, 1> sets{ssao_set.get()};
            gcmd.bind_descriptor_sets(*m_ssao_layout,
                                      std::span<const rhi::DescriptorSet* const>(sets), 0);
            gcmd.push_constants(rhi::ShaderStage::Fragment, 0, sizeof(push), &push);
            gcmd.set_viewport(0, 0, ssao_w, ssao_h);
            gcmd.set_scissor(0, 0, ssao_w, ssao_h);
            gcmd.draw(3);
            gcmd.end_render_pass();
        };
        m_graph->add_pass(ssao_pass);

        RGPassDesc ssao_blur{};
        ssao_blur.name = "SSAOBlur";
        ssao_blur.reads = {m_ssao_raw_handle, m_depth_handle, m_gbuffer1_handle};
        ssao_blur.color_attachments = {m_ssao_blur_handle};
        // Same by-value capture as the raw pass (see above): ssao_w/ssao_h
        // die with the block, the lambda outlives it.
        ssao_blur.execute = [&, ssao_w, ssao_h](rhi::CommandBuffer& gcmd) {
            const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{1.0f, 1.0f, 1.0f, 1.0f}};
            gcmd.begin_render_pass(*m_ssao_rp, *m_ssao_blur_fb,
                                   std::span<const rhi::ClearValue>(clears));
            gcmd.bind_pipeline(*m_ssao_blur_pipeline);
            const std::array<const rhi::DescriptorSet*, 1> sets{ssao_blur_set.get()};
            gcmd.bind_descriptor_sets(*m_ssao_blur_layout,
                                      std::span<const rhi::DescriptorSet* const>(sets), 0);
            gcmd.set_viewport(0, 0, ssao_w, ssao_h);
            gcmd.set_scissor(0, 0, ssao_w, ssao_h);
            gcmd.draw(3);
            gcmd.end_render_pass();
        };
        m_graph->add_pass(ssao_blur);
    }

    // Transparency: forward PBR over the HDR image Lighting just wrote.
    //
    // The pass is added unconditionally even when nothing is transparent —
    // otherwise a scene that grows a glass pane changes the set of barriers
    // recorded, and a caller comparing frames with and without one would be
    // comparing two different command streams. With the pass always present, a
    // frame with zero transparent draws records a begin/end with no draws: the
    // LOAD preserves the image and the tonemap reads it back unchanged.
    //
    // The depth attachment is declared here but NOT listed in reads: the graph
    // would then transition it to ShaderRead while this pass needs it as a
    // depth attachment. Declaring it as the depth attachment is what moves it
    // into the right state, the same way the gbuffer pass does after the
    // prepass.
    RGPassDesc transparency_pass{};
    transparency_pass.name = "Transparency";
    transparency_pass.reads = {rg_shadow, rg_local_shadow};
    transparency_pass.color_attachments = {m_hdr_handle};
    transparency_pass.depth_attachment = m_depth_handle;
    transparency_pass.execute = [&](rhi::CommandBuffer& gcmd) {
        const std::array<rhi::ClearValue, 0> no_clears{};
        // Both attachments Load: the colour holds the lit image, the depth
        // holds the prepass. The clear values are ignored under Load.
        gcmd.begin_render_pass(*m_transparency_rp, *m_transparency_fb,
                               std::span<const rhi::ClearValue>(no_clears), 1.0f, 0);
        if (transparent.empty()) {
            gcmd.end_render_pass();
            return;
        }
        gcmd.bind_pipeline(*m_forward_pipeline);
        struct Push { float view_proj[16]; float model[16]; };
        Push push{};
        std::memcpy(push.view_proj, camera.view_projection.m, sizeof(push.view_proj));
        gcmd.set_viewport(0, 0, m_width, m_height);
        gcmd.set_scissor(0, 0, m_width, m_height);
        for (auto& td : transparent) {
            std::memcpy(push.model, td.object->world.m, sizeof(push.model));
            gcmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(Push), &push);
            const std::array<const rhi::DescriptorSet*, 1> sets{td.forward_set};
            gcmd.bind_descriptor_sets(*m_forward_layout,
                                      std::span<const rhi::DescriptorSet* const>(sets), 0);
            const rhi::Buffer* vb = td.mesh->vertex_buffer(td.lod);
            const rhi::Buffer* ib = td.mesh->index_buffer(td.lod);
            if (!vb || !ib) continue;
            const std::array<const rhi::Buffer*, 1> vbs{vb};
            gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
            gcmd.bind_index_buffer(*ib, 0);
            const MeshLOD& lod = td.mesh->lods()[td.lod];
            for (const SubMesh& sm : lod.submeshes) {
                gcmd.draw_indexed(sm.index_count, 1, sm.index_offset,
                                  static_cast<i32>(sm.vertex_offset), 0);
                ++m_stats.draw_calls;
            }
        }
        gcmd.end_render_pass();
    };
    m_graph->add_pass(transparency_pass);

    // Bloom chain (design §206). Recorded only when the stage is on — the
    // tonemap bindings stay valid either way (see `bloom_on` above), so a
    // scene that never asks for bloom records the same pass list it recorded
    // before this stage existed, plus nothing.
    if (bloom_on) {
        struct BloomPush {
            float src_texel_x;
            float src_texel_y;
            float threshold; // prefilter only
            float knee;      // prefilter only
            float radius;    // downsample only
            float mode;      // 0 = prefilter, 1 = downsample
            float pad0;
            float pad1;
        };
        static_assert(sizeof(BloomPush) == 32, "must match bloom.frag's push block");
        constexpr float kPrefilterMode = 0.0f;
        constexpr float kDownsampleMode = 1.0f;
        for (u32 level = 0; level < kBloomLevels; ++level) {
            rhi::Texture* src = (level == 0)
                                    ? m_graph->get_texture(m_hdr_handle)
                                    : m_graph->get_texture(m_bloom_handles[level - 1]);
            // Validated in the `bloom_on` block above; the check here is a
            // guard against the two blocks drifting, not a live branch.
            if (src == nullptr) {
                NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: bloom level {} has no source", level);
                return false;
            }
            BloomPush push{};
            push.src_texel_x = 1.0f / static_cast<float>(std::max(1u, src->width()));
            push.src_texel_y = 1.0f / static_cast<float>(std::max(1u, src->height()));
            push.threshold = m_postfx.bloom.threshold;
            push.knee = m_postfx.bloom.knee;
            push.radius = m_postfx.bloom.radius;
            push.mode = (level == 0) ? kPrefilterMode : kDownsampleMode;

            RGPassDesc bloom_pass{};
            bloom_pass.name = (level == 0) ? "BloomPrefilter"
                                           : ("BloomDown" + std::to_string(level));
            bloom_pass.reads = {(level == 0) ? m_hdr_handle : m_bloom_handles[level - 1]};
            bloom_pass.color_attachments = {m_bloom_handles[level]};
            // Everything that varies per iteration is captured BY VALUE (`push`,
            // `level`, the level's own size). A plain `[&]` would capture the
            // loop variables by reference, so every recorded pass would read
            // the LAST iteration's values at execute time — four identical
            // passes over level 3, which renders a plausible-looking glow and
            // is completely wrong.
            const rhi::Texture* dst_tex = m_graph->get_texture(m_bloom_handles[level]);
            const u32 dst_w = dst_tex ? dst_tex->width() : 1;
            const u32 dst_h = dst_tex ? dst_tex->height() : 1;
            bloom_pass.execute = [&, push, level, dst_w, dst_h](rhi::CommandBuffer& gcmd) {
                const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{0.0f, 0.0f, 0.0f, 1.0f}};
                gcmd.begin_render_pass(*m_bloom_rp, *m_bloom_fbs[level],
                                       std::span<const rhi::ClearValue>(clears));
                gcmd.bind_pipeline(*m_bloom_pipeline);
                const std::array<const rhi::DescriptorSet*, 1> sets{bloom_sets[level].get()};
                gcmd.bind_descriptor_sets(*m_bloom_layout,
                                          std::span<const rhi::DescriptorSet* const>(sets), 0);
                gcmd.push_constants(rhi::ShaderStage::Fragment, 0, sizeof(push), &push);
                // Sized to the LEVEL, not the render target: a level is a
                // fraction of it, and a viewport left at the target's size
                // would scale the fullscreen triangle into the wrong quadrant.
                gcmd.set_viewport(0, 0, dst_w, dst_h);
                gcmd.set_scissor(0, 0, dst_w, dst_h);
                gcmd.draw(3);
                gcmd.end_render_pass();
            };
            m_graph->add_pass(bloom_pass);
            ++m_stats.bloom_levels_recorded;
        }
    }

    // Tonemap: HDR (+ the bloom levels) → output target
    RGPassDesc tonemap_pass{};
    tonemap_pass.name = "Tonemap";
    // The depth target is declared for the same reason the bloom levels are:
    // the depth-of-field stage samples it, so the graph has to transition it
    // into ShaderRead before this pass. It is declared unconditionally because
    // the binding is written unconditionally — a set with an unwritten binding
    // is not a valid set, and the shader's radius guard means an off stage
    // never reads it.
    tonemap_pass.reads = {m_hdr_handle, m_depth_handle};
    if (bloom_on) {
        // Declared so the graph transitions each level out of
        // ColorAttachment into ShaderRead before the tonemap pass samples it.
        // Without this the levels would be read in the layout they were
        // written in, which is a validation error on one driver and a garbage
        // glow on another.
        for (u32 level = 0; level < kBloomLevels; ++level) {
            tonemap_pass.reads.push_back(m_bloom_handles[level]);
        }
    }
    tonemap_pass.color_attachments = {rg_out};

    rhi::RenderPass* tonemap_rp = m_tonemap_rp_off.get();
    rhi::Pipeline* tonemap_pipe = m_tonemap_pipeline_off;
    if (out_is_present_source) {
        // The present variant needs the swapchain extension, which a
        // headless/CI device does not have — created lazily on first use.
        if (!ensure_present_variants()) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create present render pass "
                                           "(device has no swapchain extension?)");
            return false;
        }
        tonemap_rp = m_tonemap_rp_present.get();
        tonemap_pipe = m_tonemap_pipeline_present;
    }
    tonemap_pass.execute = [&](rhi::CommandBuffer& gcmd) {
        rhi::RenderPass& rp = *tonemap_rp;
        rhi::Pipeline& pipe = *tonemap_pipe;
        const TonemapFBKey key{out_target.creation_serial(), &out_target, out_is_present_source};
        auto it = m_tonemap_fbs.find(key);
        if (it == m_tonemap_fbs.end()) {
            if (m_tonemap_fbs.size() >= kMaxTonemapFramebuffers) {
                // Entries never expire on their own (see the header comment), so
                // bound the cache rather than growing it without limit.
                m_tonemap_fbs.clear();
            }
            const std::array<rhi::Texture*, 1> colors{&out_target};
            auto fb = m_device->create_framebuffer(rp, std::span<rhi::Texture* const>(colors), nullptr);
            if (!fb) {
                // Never cache or dereference a failed framebuffer: the caller
                // would otherwise begin a render pass on a null handle.
                NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: tonemap framebuffer creation failed");
                return;
            }
            it = m_tonemap_fbs.emplace(key, std::move(fb)).first;
        }
        const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{0.0f, 0.0f, 0.0f, 1.0f}};
        gcmd.begin_render_pass(rp, *it->second, std::span<const rhi::ClearValue>(clears));
        gcmd.bind_pipeline(pipe);
        const std::array<const rhi::DescriptorSet*, 1> sets{tonemap_set.get()};
        gcmd.bind_descriptor_sets(*m_tonemap_layout, std::span<const rhi::DescriptorSet* const>(sets), 0);
        // Must match tonemap.frag PushConstants (24 floats, 96 bytes).
        //
        // Every stage value is written even when its stage is off, and the
        // OFF value is the stage's exact identity rather than zero for the
        // ones whose neutral is not zero (saturation 1, grade contrast 1,
        // grade gamma 1). A zero here would be a real grade — full desaturation
        // and a black crush — so "off" has to mean the neutral number, with the
        // enable flag doing the switching.
        const float tonemap_push[24] = {
            m_exposure,
            m_postfx.vignette,
            m_postfx.saturation,
            static_cast<float>(static_cast<int>(m_tonemap_mode)),
            bloom_on ? m_postfx.bloom.intensity : 0.0f,
            (m_postfx.sharpen.enabled ? m_postfx.sharpen.amount : 0.0f),
            m_postfx.grade.enabled ? 1.0f : 0.0f,
            m_postfx.grade.contrast,
            m_postfx.grade.pivot,
            m_postfx.grade.temperature,
            m_postfx.grade.tint,
            m_postfx.grade.gamma,
            // The sharpen taps are expressed in the OUTPUT target's texels,
            // because the unsharp mask runs on the tonemapped image's own
            // resolution, not on any bloom level's.
            1.0f / static_cast<float>(std::max(1u, m_width)),
            1.0f / static_cast<float>(std::max(1u, m_height)),
            m_postfx.sharpen.radius,
            // Lens effects. The shader has no enabled flag for this stage, so
            // `enabled` is folded into the two coefficients here — the same
            // thing `bloom_on` does for the intensity above. Both zero is
            // exactly identity, which is why an unchecked box and a slider at
            // zero agree.
            m_postfx.lens.enabled ? m_postfx.lens.distortion : 0.0f,
            m_postfx.lens.enabled ? m_postfx.lens.chromatic_aberration : 0.0f,
            // Depth of field. `enabled` folds into the RADIUS, which is this
            // stage's own off switch: 0 means every circle of confusion is 0,
            // the gather returns its own centre, and the stage is exactly
            // identity. The two distances are still written when the stage is
            // off, because the shader's guard reads only the radius and a stale
            // focus distance would be a trap the next time it is switched on.
            m_postfx.dof.focus_distance,
            m_postfx.dof.focus_range,
            m_postfx.dof.enabled ? m_postfx.dof.max_radius : 0.0f,
            // Motion blur. `enabled` folds into the INTENSITY, this stage's own
            // off switch: 0 means the smear is never evaluated.
            m_postfx.motion.enabled ? m_postfx.motion.intensity : 0.0f,
            m_postfx.motion.max_length,
            // Colour-grading LUT strength, folded to 0 when NO LUT is bound.
            // The slot then holds the 1x1 white texture, so an authored
            // strength with no LUT would grade the whole frame toward white —
            // the same trap `bloom_on` avoids by folding the intensity. An
            // unbound LUT is exactly identity whatever the scene asked for.
            m_color_lut_view != nullptr ? m_postfx.lut_strength : 0.0f,
            0.0f,
        };
        gcmd.push_constants(rhi::ShaderStage::Fragment, 0, sizeof(tonemap_push), &tonemap_push);
        gcmd.set_viewport(0, 0, m_width, m_height);
        gcmd.set_scissor(0, 0, m_width, m_height);
        gcmd.draw(3);
        gcmd.end_render_pass();
    };
    m_graph->add_pass(tonemap_pass);

    if (!m_graph->compile()) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: graph compile failed");
        return false;
    }
    for (u32 pi : m_graph->execution_order()) {
        NF_LOG_TRACE(LogCategory::RHI, "Renderer3D pass {}: {}", pi, m_graph->pass_name(pi));
    }
    NF_LOG_TRACE(LogCategory::RHI, "Renderer3D prepared meshes: {}", prepared.size());
    m_graph->execute(cmd);
    // Remember the camera for the NEXT frame's motion blur. Recorded after the
    // graph runs so a frame that failed to record does not advance the history
    // and report a velocity for a frame that never appeared.
    m_prev_view_proj = camera.view_projection;
    m_has_prev_view_proj = true;
    return true;
}

bool Renderer3D::ensure_present_variants() {
    if (m_tonemap_present_ready) return true;
    rhi::ColorAttachment back_present{};
    const std::array<rhi::ColorAttachment, 1> present_atts{back_present};
    rhi::RenderPassDesc tonemap_present_rpd{};
    tonemap_present_rpd.color_attachments = std::span<const rhi::ColorAttachment>(present_atts);
    tonemap_present_rpd.present_source = true;
    m_tonemap_rp_present = m_device->create_render_pass(tonemap_present_rpd);
    if (!m_tonemap_rp_present) {
        return false;
    }
    rhi::PipelineDesc tppd{};
    tppd.vs = m_tonemap_vs.get();
    tppd.fs = m_tonemap_fs.get();
    tppd.render_pass = m_tonemap_rp_present.get();
    tppd.descriptor_set_layout = m_tonemap_layout.get();
    tppd.rasterizer.cull_mode = rhi::CullMode::None;
    tppd.depth.test_enabled = false;
    tppd.depth.write_enabled = false;
    // 24 floats, matching tonemap.frag's PushConstants AND the offscreen
    // tonemap pipeline above: the two run the same shader, so a push that fits
    // one and overruns the other is a validation error that only shows on the
    // path the tests happen to exercise.
    tppd.push_constant_size = 96;
    tppd.push_constant_stages = rhi::ShaderStage::Fragment;
    m_tonemap_pipeline_present = m_pipeline_cache->get_or_create(tppd);
    if (!m_tonemap_pipeline_present) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create present tonemap pipeline");
        return false;
    }
    // The passthrough twin (present_texture), only when its shaders loaded.
    if (m_present_vs && m_present_fs) {
        rhi::PipelineDesc pppd{};
        pppd.vs = m_present_vs.get();
        pppd.fs = m_present_fs.get();
        pppd.render_pass = m_tonemap_rp_present.get();
        pppd.descriptor_set_layout = m_tonemap_layout.get();
        pppd.rasterizer.cull_mode = rhi::CullMode::None;
        pppd.depth.test_enabled = false;
        pppd.depth.write_enabled = false;
        pppd.push_constant_size = 16;
        pppd.push_constant_stages = rhi::ShaderStage::Fragment;
        m_present_pipeline_present = m_pipeline_cache->get_or_create(pppd);
        if (!m_present_pipeline_present) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create present passthrough pipeline");
            return false;
        }
    }
    m_tonemap_present_ready = true;
    return true;
}

bool Renderer3D::present_texture(rhi::CommandBuffer& cmd, rhi::Texture& src,
                                 rhi::TextureView* src_view, rhi::Texture& out_target,
                                 bool out_is_present_source, u32 frame_slot) {
    if (!m_device || !m_graph) return false;
    if (frame_slot >= kFramesInFlight) frame_slot = 0;
    if (src_view == nullptr) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D::present_texture: null source view");
        return false;
    }
    if (!m_present_pipeline_off) {
        NF_LOG_ERROR(LogCategory::RHI,
                     "Renderer3D::present_texture: present shaders unavailable "
                     "(shader directory has no present_*.spv)");
        return false;
    }
    if (out_is_present_source && !ensure_present_variants()) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D::present_texture: present pass unavailable");
        return false;
    }

    rhi::RenderPass* rp = out_is_present_source ? m_tonemap_rp_present.get()
                                                : m_tonemap_rp_off.get();
    rhi::Pipeline* pipe = out_is_present_source ? m_present_pipeline_present
                                                : m_present_pipeline_off;
    if (!rp || !pipe) return false;

    // Same slot contract as render(): this slot's previous GPU work has been
    // waited, so the present allocator may recycle. Separate from the scene
    // allocator — a caller (the editor) runs a scene render and a present in
    // the same frame through different command buffers.
    m_present_allocators[frame_slot]->reset();
    auto set = m_present_allocators[frame_slot]->allocate(*m_tonemap_layout);
    if (!set) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: present descriptor allocation failed");
        return false;
    }
    // All seven bindings, even though present.frag reads only binding 0. The set
    // comes from m_tonemap_layout, whose other bindings are the bloom levels and
    // the depth-of-field inputs, and a set with an unwritten binding is not a
    // valid set. Binding 5 is a UNIFORM BUFFER, so it cannot go through the
    // image loop below — writing an image view into a uniform-buffer binding is
    // a validation error, not a harmless substitution.
    std::array<rhi::DescriptorWrite, 4 + kBloomLevels> writes{};
    for (u32 b = 0; b < 1 + kBloomLevels; ++b) {
        writes[b] = {b, rhi::DescriptorType::SampledImage, nullptr, 0, 0, src_view, m_sampler.get()};
    }
    writes[1 + kBloomLevels] = {1 + kBloomLevels, rhi::DescriptorType::UniformBuffer,
                                m_frame_uniforms[frame_slot].get(), 0, 144, nullptr, nullptr};
    // The depth view when it exists, the source otherwise: the passthrough
    // shader never samples it, so any valid image satisfies the binding.
    const rhi::TextureView* depth_view =
        m_gbuffer_depth_view ? m_gbuffer_depth_view.get() : src_view;
    writes[2 + kBloomLevels] = {2 + kBloomLevels, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                                depth_view, m_sampler.get()};
    writes[3 + kBloomLevels] = {3 + kBloomLevels, rhi::DescriptorType::SampledImage, nullptr, 0, 0,
                                src_view, m_sampler.get()};
    m_device->update_descriptor_set(*set, std::span<const rhi::DescriptorWrite>(writes));

    // The graph exists purely for state transitions here: src (written by an
    // earlier submission) becomes shader-readable, and the output lands in its
    // final (presentation-ready when requested) layout — the same machinery
    // the tonemap pass inside render() runs through.
    m_graph->reset(true);
    auto rg_out = m_graph->import_texture("Output", &out_target);
    auto rg_src = m_graph->import_texture("PresentSrc", &src);

    RGPassDesc pass{};
    pass.name = "Present";
    pass.reads = {rg_src};
    pass.color_attachments = {rg_out};
    pass.execute = [&](rhi::CommandBuffer& gcmd) {
        const TonemapFBKey key{out_target.creation_serial(), &out_target, out_is_present_source};
        auto it = m_tonemap_fbs.find(key);
        if (it == m_tonemap_fbs.end()) {
            if (m_tonemap_fbs.size() >= kMaxTonemapFramebuffers) {
                m_tonemap_fbs.clear();
            }
            const std::array<rhi::Texture*, 1> colors{&out_target};
            auto fb = m_device->create_framebuffer(*rp, std::span<rhi::Texture* const>(colors), nullptr);
            if (!fb) {
                NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: present framebuffer creation failed");
                return;
            }
            it = m_tonemap_fbs.emplace(key, std::move(fb)).first;
        }
        const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{0.0f, 0.0f, 0.0f, 1.0f}};
        gcmd.begin_render_pass(*rp, *it->second, std::span<const rhi::ClearValue>(clears));
        gcmd.bind_pipeline(*pipe);
        const std::array<const rhi::DescriptorSet*, 1> sets{set.get()};
        gcmd.bind_descriptor_sets(*m_tonemap_layout, std::span<const rhi::DescriptorSet* const>(sets), 0);
        // Unused by present.frag; the push block keeps the pipeline layout
        // identical to tonemap's.
        const float passthrough_push[4] = {1.0f, 0.0f, 1.0f, 0.0f};
        gcmd.push_constants(rhi::ShaderStage::Fragment, 0, sizeof(passthrough_push), &passthrough_push);
        // The OUTPUT's size, not the renderer's: the caller's renderer may be
        // sized for an offscreen target (the editor's viewport panel) while
        // this pass fills the whole swapchain image.
        gcmd.set_viewport(0, 0, out_target.width(), out_target.height());
        gcmd.set_scissor(0, 0, out_target.width(), out_target.height());
        gcmd.draw(3);
        gcmd.end_render_pass();
    };
    m_graph->add_pass(pass);
    if (!m_graph->compile()) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: present graph compile failed");
        return false;
    }
    m_graph->execute(cmd);
    return true;
}

Vec3 tonemap(Vec3 hdr, float exposure, TonemapMode mode) {
    // Must match tonemap.frag exactly. The ACES constants are the published
    // Narkowicz fit, not a hand-tuned approximation — the shader spells the
    // same polynomial, and simplifying one side only desyncs the CPU mirror.
    const auto op = [exposure, mode](f32 c) -> f32 {
        const f32 x = c * exposure;
        switch (mode) {
            case TonemapMode::Exponential: return 1.0f - std::exp(-x);
            case TonemapMode::ACES: {
                const f32 kA = 2.51f, kB = 0.03f, kC = 2.43f, kD = 0.59f, kE = 0.14f;
                return std::clamp((x * (kA * x + kB)) / (x * (kC * x + kD) + kE), 0.0f, 1.0f);
            }
            case TonemapMode::Reinhard: return x / (1.0f + x);
            case TonemapMode::Linear:
            default: return x; // unclamped: the UNorm target clamps on write
        }
    };
    return {op(hdr.x), op(hdr.y), op(hdr.z)};
}

Vec3 apply_postfx(Vec3 color, Vec2 uv, const PostFxParams& params) {
    // Must match tonemap.frag exactly (Rec.709 luma, same smoothstep band).
    const float luma = color.x * 0.2126f + color.y * 0.7152f + color.z * 0.0722f;
    Vec3 out{luma + (color.x - luma) * params.saturation,
             luma + (color.y - luma) * params.saturation,
             luma + (color.z - luma) * params.saturation};
    const float dx = uv.x - 0.5f;
    const float dy = uv.y - 0.5f;
    const float dist = std::sqrt(dx * dx + dy * dy);
    const float t = std::clamp((dist - 0.3f) / (0.9f - 0.3f), 0.0f, 1.0f);
    const float vig = t * t * (3.0f - 2.0f * t);
    const float dark = 1.0f - params.vignette * vig;
    out.x *= dark;
    out.y *= dark;
    out.z *= dark;
    return out;
}

Vec3 bloom_prefilter(Vec3 hdr, float threshold, float knee) {
    // Must match bloom.frag's prefilter branch exactly.
    //
    // Luminance is the MAX channel, not the Rec.709 dot product the saturation
    // stage uses. A coloured highlight (a saturated red lamp, say) has a high
    // max channel and a middling luma, and a luma threshold would refuse to
    // bloom exactly the light sources bloom exists for.
    const float lum = std::max(hdr.x, std::max(hdr.y, hdr.z));
    float soft = 0.0f;
    if (knee > 0.0f) {
        // Quadratic ramp across the knee. The 1e-4 is not a fudge: a knee of 0
        // reaches the else-branch above, but a knee of 1e-6 from a slider does
        // not, and 4*knee alone would divide by ~0.
        soft = std::clamp(lum - threshold + knee, 0.0f, 2.0f * knee);
        soft = soft * soft / (4.0f * knee + 1e-4f);
    }
    // max(soft, lum - threshold) keeps the ramp from REDUCING a pixel that is
    // already past the knee: the two curves meet at lum == threshold + knee.
    const float contrib = std::max(soft, lum - threshold) / std::max(lum, 1e-4f);
    return {hdr.x * contrib, hdr.y * contrib, hdr.z * contrib};
}

Vec3 bloom_downsample(std::span<const Vec3> src, u32 src_w, u32 src_h,
                      Vec2 uv, float radius) {
    if (src.empty() || src_w == 0 || src_h == 0) return Vec3{0.0f, 0.0f, 0.0f};
    const i32 w = static_cast<i32>(src_w);
    const i32 h = static_cast<i32>(src_h);
    // Nearest-texel fetch. Exact against the shader whenever a tap lands on a
    // texel centre (which is what the tests arrange), and edge-clamped the way
    // a clamped sampler behaves when a tap runs off the image.
    const auto tap = [&](float ox, float oy) -> Vec3 {
        const float tx = uv.x * static_cast<float>(src_w) + ox * radius;
        const float ty = uv.y * static_cast<float>(src_h) + oy * radius;
        const i32 ix = std::clamp(static_cast<i32>(std::floor(tx)), 0, w - 1);
        const i32 iy = std::clamp(static_cast<i32>(std::floor(ty)), 0, h - 1);
        const usize idx = static_cast<usize>(iy) * src_w + static_cast<usize>(ix);
        return idx < src.size() ? src[idx] : Vec3{0.0f, 0.0f, 0.0f};
    };
    const Vec3 corner = tap(-2.0f, -2.0f);
    const Vec3 corner2 = tap(2.0f, -2.0f);
    const Vec3 corner3 = tap(-2.0f, 2.0f);
    const Vec3 corner4 = tap(2.0f, 2.0f);
    const Vec3 edge = tap(0.0f, -2.0f);
    const Vec3 edge2 = tap(0.0f, 2.0f);
    const Vec3 edge3 = tap(-2.0f, 0.0f);
    const Vec3 edge4 = tap(2.0f, 0.0f);
    const Vec3 diag = tap(-1.0f, -1.0f);
    const Vec3 diag2 = tap(1.0f, -1.0f);
    const Vec3 diag3 = tap(-1.0f, 1.0f);
    const Vec3 diag4 = tap(1.0f, 1.0f);
    const Vec3 centre = tap(0.0f, 0.0f);
    // Weights sum to exactly 1: 0.125 centre + 4*0.125 diagonal + 4*0.0625
    // edge + 4*0.03125 corner. A downsampled level of a flat image is that
    // image, which is what makes the chain's overall brightness independent of
    // the level count.
    const float wc = 0.125f, wd = 0.125f, we = 0.0625f, wk = 0.03125f;
    return Vec3{
        (corner.x + corner2.x + corner3.x + corner4.x) * wk +
            (edge.x + edge2.x + edge3.x + edge4.x) * we +
            (diag.x + diag2.x + diag3.x + diag4.x) * wd + centre.x * wc,
        (corner.y + corner2.y + corner3.y + corner4.y) * wk +
            (edge.y + edge2.y + edge3.y + edge4.y) * we +
            (diag.y + diag2.y + diag3.y + diag4.y) * wd + centre.y * wc,
        (corner.z + corner2.z + corner3.z + corner4.z) * wk +
            (edge.z + edge2.z + edge3.z + edge4.z) * we +
            (diag.z + diag2.z + diag3.z + diag4.z) * wd + centre.z * wc};
}

Vec3 unsharp_hdr(Vec3 center, Vec3 blurred, float amount) {
    return {center.x + (center.x - blurred.x) * amount,
            center.y + (center.y - blurred.y) * amount,
            center.z + (center.z - blurred.z) * amount};
}

Vec2 lens_sample_uv(Vec2 uv, const LensParams& params, int channel) {
    // Matches lens_sample_uv() in tonemap.frag.
    //
    // `enabled` is folded HERE, the same way the renderer folds it at push time
    // for the other stages (bloom writes `intensity` or 0): the shader has no
    // enabled flag for this stage, so a mirror that honoured the flag without
    // the renderer doing the same would compare two different images.
    const float distortion = params.enabled ? params.distortion : 0.0f;
    const float chroma = params.enabled ? params.chromatic_aberration : 0.0f;
    if (channel < 0 || channel > 2) {
        return uv;
    }
    if (distortion != 0.0f) {
        const Vec2 c{uv.x - 0.5f, uv.y - 0.5f};
        const float scale = 1.0f + distortion * (c.x * c.x + c.y * c.y);
        uv = Vec2{0.5f + c.x * scale, 0.5f + c.y * scale};
    }
    if (chroma != 0.0f && channel != 1) {
        const Vec2 c{uv.x - 0.5f, uv.y - 0.5f};
        const float scale = (channel == 0) ? (1.0f + chroma) : (1.0f - chroma);
        uv = Vec2{0.5f + c.x * scale, 0.5f + c.y * scale};
    }
    return uv;
}

float dof_coc(float view_depth, const DofParams& params) {
    // Matches dof_coc() in tonemap.frag.
    //
    // `enabled` is folded here the same way the renderer folds it at push time
    // for the other stages — the shader has no separate flag for this one, so a
    // mirror that honoured the flag without the renderer doing the same would
    // compare two different images.
    const float max_radius = params.enabled ? params.max_radius : 0.0f;
    if (max_radius <= 0.0f || params.focus_range <= 0.0f) {
        return 0.0f;
    }
    const float t = std::fabs(view_depth - params.focus_distance) / params.focus_range;
    const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return clamped * max_radius;
}

Vec2 motion_prev_uv(Vec2 uv, float depth, const Mat4& inv_view_proj,
                    const Mat4& prev_view_proj) {
    // Matches motion_prev_uv() in tonemap.frag.
    if (!(depth < 0.999999f)) {
        return uv; // the sky is at infinity: no surface to reproject
    }
    const Vec4 world =
        inv_view_proj * Vec4{Vec3{uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, depth}, 1.0f};
    if (std::fabs(world.w) < 1e-9f) {
        return uv;
    }
    const Vec3 wp{world.x / world.w, world.y / world.w, world.z / world.w};
    const Vec4 prev = prev_view_proj * Vec4{wp, 1.0f};
    if (std::fabs(prev.w) < 1e-9f) {
        return uv;
    }
    return Vec2{prev.x / prev.w * 0.5f + 0.5f, prev.y / prev.w * 0.5f + 0.5f};
}

Vec2 motion_smear(Vec2 uv, float depth, const Mat4& inv_view_proj,
                  const Mat4& prev_view_proj, const MotionBlurParams& params) {
    // `enabled` is folded here the same way the renderer folds it at push time.
    const float intensity = params.enabled ? params.intensity : 0.0f;
    if (intensity <= 0.0f) {
        return Vec2{0.0f, 0.0f};
    }
    const Vec2 prev = motion_prev_uv(uv, depth, inv_view_proj, prev_view_proj);
    const Vec2 velocity{(uv.x - prev.x) * intensity, (uv.y - prev.y) * intensity};
    const float len = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
    if (len < 1e-6f) {
        return Vec2{0.0f, 0.0f};
    }
    const float span = std::min(len, std::max(params.max_length, 0.0f));
    const float s = span / len;
    return Vec2{velocity.x * s, velocity.y * s};
}

Vec3 apply_color_grade(Vec3 hdr, const ColorGradeParams& params) {
    // Must match tonemap.frag's grade branch exactly, including the 0.25 gain
    // scale: it makes +/-1 a visibly strong correction without letting a
    // slider at full deflection drive a channel to zero (which would be a
    // colour shift so total it reads as a bug rather than a grade).
    constexpr float kGain = 0.25f;
    Vec3 c = hdr;
    const float temp = params.temperature;
    if (temp != 0.0f) {
        // Warm moves red up and blue down by the same amount, which is what
        // keeps the overall exposure roughly fixed as the balance moves.
        c.x *= 1.0f + kGain * temp;
        c.z *= 1.0f - kGain * temp;
    }
    if (params.tint != 0.0f) {
        // Positive tint is magenta: green comes down, and only green. Scaling
        // red and blue up instead would make "tint" a second exposure control.
        c.y *= 1.0f - kGain * params.tint;
    }
    if (params.contrast != 1.0f) {
        c.x = (c.x - params.pivot) * params.contrast + params.pivot;
        c.y = (c.y - params.pivot) * params.contrast + params.pivot;
        c.z = (c.z - params.pivot) * params.contrast + params.pivot;
    }
    if (params.gamma != 1.0f && params.gamma > 0.0f) {
        const float inv = 1.0f / params.gamma;
        // max(0, ...) because pow() of a negative base is undefined in GLSL and
        // a contrast pivot below the black point can push a channel negative.
        c.x = std::pow(std::max(c.x, 0.0f), inv);
        c.y = std::pow(std::max(c.y, 0.0f), inv);
        c.z = std::pow(std::max(c.z, 0.0f), inv);
    }
    return c;
}

Vec3 apply_color_lut(Vec3 color, std::span<const u8> rgba, float strength) {
    // Matches apply_lut() in tonemap.frag.
    const u32 n = kLutSize;
    const usize expected = static_cast<usize>(n) * n * n * 4u;
    if (strength <= 0.0f || rgba.size() < expected) {
        return color;
    }
    const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    // The cube is a strip of n slices, each n x n, row-major with a top-left
    // origin: slice i occupies x in [i*n, (i+1)*n).
    const auto sample = [&](u32 slice, u32 x, u32 y) -> Vec3 {
        const usize idx =
            (static_cast<usize>(y) * (n * n) + static_cast<usize>(slice) * n + x) * 4u;
        constexpr float kInv = 1.0f / 255.0f;
        return Vec3{rgba[idx] * kInv, rgba[idx + 1] * kInv, rgba[idx + 2] * kInv};
    };
    const Vec3 c{clamp01(color.x), clamp01(color.y), clamp01(color.z)};
    const float slice = c.z * static_cast<float>(n - 1);
    const float s0 = std::floor(slice);
    const float s1 = std::min(s0 + 1.0f, static_cast<float>(n - 1));
    const float f = slice - s0;
    // NEAREST texel, not bilinear: the shader samples with a LINEAR filter and a
    // half-texel offset, so a test that asserted equality against a LUT with
    // varying texels would be pinning the filtering mode rather than the
    // lookup. `bloom_downsample` documents the same convention. The cases this
    // is compared against are chosen so the two agree exactly — a constant LUT,
    // and a strength of 0.
    const auto texel = [&](float s) {
        const u32 si = static_cast<u32>(s) < n ? static_cast<u32>(s) : n - 1;
        const u32 x = static_cast<u32>(c.x * static_cast<float>(n - 1) + 0.5f);
        const u32 y = static_cast<u32>(c.y * static_cast<float>(n - 1) + 0.5f);
        return sample(si, x < n ? x : n - 1, y < n ? y : n - 1);
    };
    const Vec3 a = texel(s0);
    const Vec3 b = texel(s1);
    const Vec3 graded{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f};
    const float t = clamp01(strength);
    return Vec3{color.x + (graded.x - color.x) * t, color.y + (graded.y - color.y) * t,
                color.z + (graded.z - color.z) * t};
}

Vec3 apply_post_chain(Vec3 hdr, Vec3 blurred, Vec3 bloom, Vec2 uv,
                      float exposure, TonemapMode mode, const PostFxParams& params) {
    // The implemented order, which is what the shader spells:
    //   sharpen -> bloom add -> exposure -> colour grade -> tonemap operator
    //   -> gamma -> saturation -> vignette
    //
    // §206 lists the stack as Exposure -> Color Grading -> Bloom -> ... ->
    // Tonemapping. The bloom add sits before exposure here for one concrete
    // reason: the prefilter that produced it reads the raw HDR target, so its
    // threshold is expressed in pre-exposure units. Adding the bloom after
    // exposure would make the glow's strength track the exposure control while
    // its threshold did not, which is the combination that makes a bright
    // scene bloom and the same scene at a lower exposure stop.
    if (params.sharpen.enabled && params.sharpen.amount > 0.0f) {
        hdr = unsharp_hdr(hdr, blurred, params.sharpen.amount);
    }
    if (params.bloom.enabled && params.bloom.intensity > 0.0f) {
        const float k = params.bloom.intensity;
        hdr = Vec3{hdr.x + bloom.x * k, hdr.y + bloom.y * k, hdr.z + bloom.z * k};
    }
    // Exposure is applied here and the operator is then called with a factor of
    // 1. `tonemap()` folds exposure in for its own callers; doing it twice
    // would square it. Splitting it out is what lets grading sit between the
    // two, which is where §206 puts it.
    hdr = Vec3{hdr.x * exposure, hdr.y * exposure, hdr.z * exposure};
    if (params.grade.enabled) {
        hdr = apply_color_grade(hdr, params.grade);
    }
    Vec3 mapped = tonemap(hdr, 1.0f, mode);
    const float inv_gamma = 1.0f / 2.2f;
    mapped = Vec3{std::pow(std::max(mapped.x, 0.0f), inv_gamma),
                  std::pow(std::max(mapped.y, 0.0f), inv_gamma),
                  std::pow(std::max(mapped.z, 0.0f), inv_gamma)};
    return apply_postfx(mapped, uv, params);
}

} // namespace nf::rendering
