#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

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
    auto tvs = load_spirv_file(sdir / "tonemap_vert.spv");
    auto tfs = load_spirv_file(sdir / "tonemap_frag.spv");
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
    m_tonemap_vs = device.create_shader_module({tvs, rhi::ShaderStage::Vertex});
    m_tonemap_fs = device.create_shader_module({tfs, rhi::ShaderStage::Fragment});
    if (!m_depth_vs || !m_gbuffer_vs || !m_lighting_vs || !m_tonemap_vs) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create shader modules");
        return false;
    }

    // Static vertex layout shared by every mesh (see Vertex in StaticMesh.hpp)
    static const std::array<rhi::VertexAttrib, 3> kMeshAttribs{{
        {0, offsetof(Vertex, position), rhi::Format::R32G32B32_SFloat},
        {1, offsetof(Vertex, normal), rhi::Format::R32G32B32_SFloat},
        {2, offsetof(Vertex, uv0), rhi::Format::R32G32_SFloat},
    }};

    // --- descriptor layouts ---
    {
        const std::array<rhi::DescriptorBinding, 2> material_binds{{
            {0, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1},
            {1, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
        }};
        rhi::DescriptorSetLayoutDesc ld{};
        ld.bindings = std::span<const rhi::DescriptorBinding>(material_binds);
        m_material_layout = device.create_descriptor_set_layout(ld);

        const std::array<rhi::DescriptorBinding, 5> lighting_binds{{
            {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // base
            {1, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // normal
            {2, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // surface
            {3, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // depth
            {4, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1},// frame uniforms
        }};
        rhi::DescriptorSetLayoutDesc lld{};
        lld.bindings = std::span<const rhi::DescriptorBinding>(lighting_binds);
        m_lighting_layout = device.create_descriptor_set_layout(lld);

        const std::array<rhi::DescriptorBinding, 1> tonemap_binds{{
            {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
        }};
        rhi::DescriptorSetLayoutDesc tld{};
        tld.bindings = std::span<const rhi::DescriptorBinding>(tonemap_binds);
        m_tonemap_layout = device.create_descriptor_set_layout(tld);
    }
    if (!m_material_layout || !m_lighting_layout || !m_tonemap_layout) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create descriptor layouts");
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
        const std::array<rhi::ColorAttachment, 3> gcolor_atts{gcolor, gcolor, gcolor};
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

        rhi::ColorAttachment back_off{};
        back_off.format = rhi::Format::R8G8B8A8_UNorm;
        const std::array<rhi::ColorAttachment, 1> off_atts{back_off};
        rhi::RenderPassDesc tonemap_off_rpd{};
        tonemap_off_rpd.color_attachments = std::span<const rhi::ColorAttachment>(off_atts);
        tonemap_off_rpd.present_source = false;
        m_tonemap_rp_off = device.create_render_pass(tonemap_off_rpd);

        // The present variant is format-agnostic at creation (the swapchain
        // format is fixed at init time by the caller's swapchain, typically
        // B8G8R8A8); a separate pipeline covers it.
    }
    if (!m_depth_rp || !m_gbuffer_rp || !m_lighting_rp || !m_tonemap_rp_off) {
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

        // Tonemap: one pipeline per render pass flavour
        rhi::PipelineDesc tpd{};
        tpd.vs = m_tonemap_vs.get();
        tpd.fs = m_tonemap_fs.get();
        tpd.render_pass = m_tonemap_rp_off.get();
        tpd.descriptor_set_layout = m_tonemap_layout.get();
        tpd.rasterizer.cull_mode = rhi::CullMode::None;
        tpd.depth.test_enabled = false;
        tpd.depth.write_enabled = false;
        tpd.push_constant_size = 4; // exposure
        tpd.push_constant_stages = rhi::ShaderStage::Fragment;
        m_tonemap_pipeline_off = m_pipeline_cache->get_or_create(tpd);

    }
    if (!m_depth_pipeline || !m_gbuffer_material || !m_gbuffer_material->valid() ||
        !m_lighting_pipeline || !m_tonemap_pipeline_off) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create pipelines");
        return false;
    }

    // --- per-frame helpers ---
    m_material_library = std::make_unique<MaterialLibrary>(device);
    m_descriptor_allocator = device.create_descriptor_allocator(32);

    rhi::BufferDesc frame_ubo_desc{};
    frame_ubo_desc.size = sizeof(FrameUniforms);
    frame_ubo_desc.usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst;
    frame_ubo_desc.memory = rhi::MemoryUsage::CPUToGPU;
    m_frame_uniforms = device.create_buffer(frame_ubo_desc);
    if (!m_frame_uniforms) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create frame uniform buffer");
        return false;
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

    rhi::TextureDesc hdr_desc = color_desc;
    hdr_desc.format = rhi::Format::R16G16B16A16_SFloat;
    m_hdr_handle = m_graph->create_texture("HDR", hdr_desc);

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

bool Renderer3D::create_resolution_dependent(u32 width, u32 height) {
    m_width = width;
    m_height = height;

    rhi::Texture* depth = m_graph->get_texture(m_depth_handle);
    rhi::Texture* gb0 = m_graph->get_texture(m_gbuffer0_handle);
    rhi::Texture* gb1 = m_graph->get_texture(m_gbuffer1_handle);
    rhi::Texture* gb2 = m_graph->get_texture(m_gbuffer2_handle);
    rhi::Texture* hdr = m_graph->get_texture(m_hdr_handle);
    if (!depth || !gb0 || !gb1 || !gb2 || !hdr) return false;

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
    vd.texture = hdr;
    m_hdr_view = m_device->create_texture_view(vd);
    vd.texture = depth;
    vd.aspect = rhi::ImageAspect::Depth;
    m_gbuffer_depth_view = m_device->create_texture_view(vd);
    if (!m_gbuffer0_view || !m_gbuffer1_view || !m_gbuffer2_view || !m_hdr_view || !m_gbuffer_depth_view) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create target views");
        return false;
    }

    const std::array<rhi::Texture*, 0> no_colors{};
    m_depth_fb = m_device->create_framebuffer(*m_depth_rp,
                                              std::span<rhi::Texture* const>(no_colors), depth);
    const std::array<rhi::Texture*, 3> gbuffer_colors{gb0, gb1, gb2};
    m_gbuffer_fb = m_device->create_framebuffer(*m_gbuffer_rp,
                                                std::span<rhi::Texture* const>(gbuffer_colors), depth);
    const std::array<rhi::Texture*, 1> hdr_colors{hdr};
    m_lighting_fb = m_device->create_framebuffer(*m_lighting_rp,
                                                 std::span<rhi::Texture* const>(hdr_colors), nullptr);
    if (!m_depth_fb || !m_gbuffer_fb || !m_lighting_fb) {
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
    m_tonemap_fbs.clear();
    m_gbuffer0_view.reset();
    m_gbuffer1_view.reset();
    m_gbuffer2_view.reset();
    m_gbuffer_depth_view.reset();
    m_hdr_view.reset();
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
    rhi::TextureDesc hdr_desc = color_desc;
    hdr_desc.format = rhi::Format::R16G16B16A16_SFloat;
    m_hdr_handle = m_graph->create_texture("HDR", hdr_desc);
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
    m_frame_uniforms.reset();
    m_descriptor_allocator.reset();
    m_material_library.reset();
    m_gbuffer_material.reset();
    if (m_pipeline_cache) {
        delete m_pipeline_cache;
        m_pipeline_cache = nullptr;
    }
    m_depth_pipeline = nullptr;
    m_lighting_pipeline = nullptr;
    m_tonemap_pipeline_off = nullptr;
    m_tonemap_pipeline_present = nullptr;
    m_graph.reset();
    m_depth_vs.reset(); m_depth_fs.reset();
    m_gbuffer_vs.reset(); m_gbuffer_fs.reset();
    m_lighting_vs.reset(); m_lighting_fs.reset();
    m_tonemap_vs.reset(); m_tonemap_fs.reset();
    m_material_layout.reset();
    m_lighting_layout.reset();
    m_tonemap_layout.reset();
    m_depth_rp.reset();
    m_gbuffer_rp.reset();
    m_lighting_rp.reset();
    m_tonemap_rp_off.reset();
    m_tonemap_rp_present.reset();
    m_tonemap_present_ready = false;
    m_white_view.reset();
    m_white_texture.reset();
    m_sampler.reset();
    m_device = nullptr;
}

bool Renderer3D::render(rhi::CommandBuffer& cmd, const RenderWorld& render_world,
                        const Camera& camera, rhi::Texture& out_target,
                        bool out_is_present_source) {
    if (!m_device || !m_graph) return false;

    // --- Frustum culling: RenderWorld → Visible Objects ---
    nf::Clock cull_clock;
    cull_render_world(render_world, camera, m_visible);
    m_stats.cull_us = cull_clock.elapsed_us();
    m_stats.extracted = static_cast<u32>(render_world.objects.size());
    m_stats.visible = static_cast<u32>(m_visible.size());
    m_stats.draw_calls = 0;
    m_stats.material_sets_built = 0;

    // --- Frame uniforms (camera + lights) ---
    FrameUniforms fu{};
    const Mat4 inv_vp = camera.view_projection.inverse();
    std::memcpy(fu.inv_view_proj, inv_vp.m, sizeof(fu.inv_view_proj));
    fu.cam_pos_ambient[0] = camera.position.x;
    fu.cam_pos_ambient[1] = camera.position.y;
    fu.cam_pos_ambient[2] = camera.position.z;
    fu.cam_pos_ambient[3] = m_ambient;
    fu.dir_dir_enable[0] = m_directional.direction.x;
    fu.dir_dir_enable[1] = m_directional.direction.y;
    fu.dir_dir_enable[2] = m_directional.direction.z;
    fu.dir_dir_enable[3] = m_directional.enabled ? 1.0f : 0.0f;
    fu.dir_color_int[0] = m_directional.color.x;
    fu.dir_color_int[1] = m_directional.color.y;
    fu.dir_color_int[2] = m_directional.color.z;
    fu.dir_color_int[3] = m_directional.intensity;
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
        fu.spots[i].outer_pad[0] = std::cos(l.outer_angle_rad);
    }
    m_frame_uniforms->update(&fu, 0, sizeof(fu));

    // --- Per-frame descriptor sets ---
    // Contract: previous frame's GPU work has completed (fence waited), so
    // recycling the allocator here cannot touch in-flight descriptor sets.
    m_descriptor_allocator->reset();

    // Material sets are allocated lazily per visible object below; allocate
    // lighting/tonemap sets once.
    auto lighting_set = m_descriptor_allocator->allocate(*m_lighting_layout);
    auto tonemap_set = m_descriptor_allocator->allocate(*m_tonemap_layout);
    if (!lighting_set || !tonemap_set) {
        NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: descriptor allocation failed");
        return false;
    }
    {
        const std::array<rhi::DescriptorWrite, 5> lighting_writes{{
            {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer0_view.get(), m_sampler.get()},
            {1, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer1_view.get(), m_sampler.get()},
            {2, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer2_view.get(), m_sampler.get()},
            {3, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer_depth_view.get(), m_sampler.get()},
            {4, rhi::DescriptorType::UniformBuffer, m_frame_uniforms.get(), 0, sizeof(FrameUniforms), nullptr, nullptr},
        }};
        m_device->update_descriptor_set(*lighting_set, std::span<const rhi::DescriptorWrite>(lighting_writes));
        const std::array<rhi::DescriptorWrite, 1> tonemap_writes{{
            {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_hdr_view.get(), m_sampler.get()},
        }};
        m_device->update_descriptor_set(*tonemap_set, std::span<const rhi::DescriptorWrite>(tonemap_writes));
    }

    // --- Draw submission preparation: resolve handles, allocate per-object
    // material descriptor sets. Changing material parameters never lands here
    // as pipeline work — parameters live in the instances' UBOs.
    nf::Clock prep_clock;
    struct PreparedDraw {
        const RenderObject* object;
        const StaticMesh* mesh;
        const rhi::DescriptorSet* material_set; // non-owning: cached on the entry
    };
    std::vector<PreparedDraw> prepared;
    prepared.reserve(m_visible.size());
    for (u32 idx : m_visible) {
        const RenderObject& ro = render_world.objects[idx];
        if (!ro.mesh_handle.valid() || !m_mesh_library) continue;
        const StaticMesh* mesh = m_mesh_library->get(ro.mesh_handle);
        if (!mesh || !mesh->is_uploaded()) continue;

        PreparedDraw pd{&ro, mesh, nullptr};
        MaterialEntry* entry = m_material_library->get(ro.material_handle);
        if (entry && entry->material && entry->material->valid()) {
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
                const std::array<rhi::DescriptorWrite, 2> material_writes{{
                    {0, rhi::DescriptorType::UniformBuffer, entry->params_ubo.get(), 0,
                     12 * sizeof(float), nullptr, nullptr},
                    {1, rhi::DescriptorType::SampledImage, nullptr, 0, 0, albedo, m_sampler.get()},
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
    m_stats.draw_prep_us = prep_clock.elapsed_us();

    // --- Build the frame graph ---
    m_graph->reset(true);
    auto rg_out = m_graph->import_texture("Output", &out_target);

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
        for (auto& pd : prepared) {
            std::memcpy(push.model, pd.object->world.m, sizeof(push.model));
            gcmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(Push), &push);
            const rhi::Buffer* vb = pd.mesh->vertex_buffer(pd.object->lod);
            const rhi::Buffer* ib = pd.mesh->index_buffer(pd.object->lod);
            if (!vb || !ib) continue;
            const std::array<const rhi::Buffer*, 1> vbs{vb};
            gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
            gcmd.bind_index_buffer(*ib, 0);
            const MeshLOD& lod = pd.mesh->lods()[pd.object->lod];
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
    gbuffer_pass.color_attachments = {m_gbuffer0_handle, m_gbuffer1_handle, m_gbuffer2_handle};
    gbuffer_pass.depth_attachment = m_depth_handle;
    gbuffer_pass.execute = [&](rhi::CommandBuffer& gcmd) {
        const std::array<rhi::ClearValue, 3> clears{
            rhi::ClearValue{0.0f, 0.0f, 0.0f, 1.0f},
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
        for (auto& pd : prepared) {
            if (!pd.material_set) continue;
            std::memcpy(push.model, pd.object->world.m, sizeof(push.model));
            gcmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(Push), &push);
            const std::array<const rhi::DescriptorSet*, 1> sets{pd.material_set};
            gcmd.bind_descriptor_sets(*m_material_layout, std::span<const rhi::DescriptorSet* const>(sets), 0);
            const rhi::Buffer* vb = pd.mesh->vertex_buffer(pd.object->lod);
            const rhi::Buffer* ib = pd.mesh->index_buffer(pd.object->lod);
            if (!vb || !ib) continue;
            const std::array<const rhi::Buffer*, 1> vbs{vb};
            gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
            gcmd.bind_index_buffer(*ib, 0);
            const MeshLOD& lod = pd.mesh->lods()[pd.object->lod];
            for (const SubMesh& sm : lod.submeshes) {
                gcmd.draw_indexed(sm.index_count, 1, sm.index_offset, static_cast<i32>(sm.vertex_offset), 0);
                ++m_stats.draw_calls;
            }
        }
        gcmd.end_render_pass();
    };
    m_graph->add_pass(gbuffer_pass);

    // Lighting: GBuffer + Depth → HDR
    RGPassDesc lighting_pass{};
    lighting_pass.name = "Lighting";
    lighting_pass.reads = {m_gbuffer0_handle, m_gbuffer1_handle, m_gbuffer2_handle, m_depth_handle};
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

    // Tonemap: HDR → output target
    RGPassDesc tonemap_pass{};
    tonemap_pass.name = "Tonemap";
    tonemap_pass.reads = {m_hdr_handle};
    tonemap_pass.color_attachments = {rg_out};

    rhi::RenderPass* tonemap_rp = m_tonemap_rp_off.get();
    rhi::Pipeline* tonemap_pipe = m_tonemap_pipeline_off;
    if (out_is_present_source) {
        if (!m_tonemap_present_ready) {
            rhi::ColorAttachment back_present{};
            const std::array<rhi::ColorAttachment, 1> present_atts{back_present};
            rhi::RenderPassDesc tonemap_present_rpd{};
            tonemap_present_rpd.color_attachments = std::span<const rhi::ColorAttachment>(present_atts);
            tonemap_present_rpd.present_source = true;
            m_tonemap_rp_present = m_device->create_render_pass(tonemap_present_rpd);
            if (!m_tonemap_rp_present) {
                NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create present render pass "
                                               "(device has no swapchain extension?)");
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
            tppd.push_constant_size = 4;
            tppd.push_constant_stages = rhi::ShaderStage::Fragment;
            m_tonemap_pipeline_present = m_pipeline_cache->get_or_create(tppd);
            if (!m_tonemap_pipeline_present) {
                NF_LOG_ERROR(LogCategory::RHI, "Renderer3D: failed to create present tonemap pipeline");
                return false;
            }
            m_tonemap_present_ready = true;
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
        gcmd.push_constants(rhi::ShaderStage::Fragment, 0, sizeof(float), &m_exposure);
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
    return true;
}

} // namespace nf::rendering
