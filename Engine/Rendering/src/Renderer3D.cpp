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

        const std::array<rhi::DescriptorBinding, 6> lighting_binds{{
            {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // base
            {1, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // normal
            {2, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // surface
            {3, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // depth
            {4, rhi::DescriptorType::UniformBuffer, rhi::ShaderStage::Fragment, 1},// frame uniforms
            {5, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1}, // shadow map
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

        // Tonemap: one pipeline per render pass flavour
        rhi::PipelineDesc tpd{};
        tpd.vs = m_tonemap_vs.get();
        tpd.fs = m_tonemap_fs.get();
        tpd.render_pass = m_tonemap_rp_off.get();
        tpd.descriptor_set_layout = m_tonemap_layout.get();
        tpd.rasterizer.cull_mode = rhi::CullMode::None;
        tpd.depth.test_enabled = false;
        tpd.depth.write_enabled = false;
        tpd.push_constant_size = 16; // exposure + vignette + saturation + pad
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
    m_shadow_fb.reset();
    m_shadow_view.reset();
    m_shadow_map.reset();
    m_shadow_atlas_size = 0;
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

bool Renderer3D::render(rhi::CommandBuffer& cmd, const RenderWorld& render_world,
                        const Camera& camera, rhi::Texture& out_target,
                        bool out_is_present_source) {
    if (!m_device || !m_graph) return false;

    // Applies a set_shadow_tile_size() issued since the last frame. A no-op in
    // steady state; the atlas is deliberately not rebuilt on resize(), because
    // it does not depend on the output resolution.
    if (!ensure_shadow_atlas(m_shadow_tile_size)) return false;

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
        const std::array<rhi::DescriptorWrite, 6> lighting_writes{{
            {0, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer0_view.get(), m_sampler.get()},
            {1, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer1_view.get(), m_sampler.get()},
            {2, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer2_view.get(), m_sampler.get()},
            {3, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_gbuffer_depth_view.get(), m_sampler.get()},
            {4, rhi::DescriptorType::UniformBuffer, m_frame_uniforms.get(), 0, sizeof(FrameUniforms), nullptr, nullptr},
            {5, rhi::DescriptorType::SampledImage, nullptr, 0, 0, m_shadow_view.get(), m_sampler.get()},
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
        u32 lod = 0;                            // effective LOD for this frame
    };
    std::vector<PreparedDraw> prepared;
    prepared.reserve(m_visible.size());
    for (u32 idx : m_visible) {
        const RenderObject& ro = render_world.objects[idx];
        if (!ro.mesh_handle.valid() || !m_mesh_library) continue;
        const StaticMesh* mesh = m_mesh_library->get(ro.mesh_handle);
        if (!mesh || !mesh->is_uploaded()) continue;
        if (mesh->lods().empty()) continue;

        PreparedDraw pd{&ro, mesh, nullptr, 0};
        // Distance LOD, resolved once per frame (not per pass): the authored
        // lod is a floor, distance only coarsens, and the result is clamped
        // into range — a stale lod on a poorer mesh draws the coarsest LOD
        // instead of indexing out of bounds.
        {
            const float dx = ro.sphere.cx - camera.position.x;
            const float dy = ro.sphere.cy - camera.position.y;
            const float dz = ro.sphere.cz - camera.position.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const u32 distance_lod = select_lod(distance,
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
    // The shadow map is a standalone fixed-size texture; importing it lets
    // the graph transition it (depth write in the shadow pass, fragment read
    // in lighting) with the same barriers as owned targets.
    auto rg_shadow = m_graph->import_texture("ShadowAtlas", m_shadow_map.get());

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
        for (u32 cascade = 0; cascade < cascade_count; ++cascade) {
            const u32 col = cascade % kShadowTileGrid;
            const u32 row = cascade / kShadowTileGrid;
            std::memcpy(push.view_proj, fu.light_view_proj[cascade], sizeof(push.view_proj));
            gcmd.set_viewport(col * tile, row * tile, tile, tile);
            gcmd.set_scissor(col * tile, row * tile, tile, tile);
            for (auto& pd : prepared) {
                std::memcpy(push.model, pd.object->world.m, sizeof(push.model));
                gcmd.push_constants(rhi::ShaderStage::Vertex, 0, sizeof(Push), &push);
                const rhi::Buffer* vb = pd.mesh->vertex_buffer(pd.lod);
                const rhi::Buffer* ib = pd.mesh->index_buffer(pd.lod);
                if (!vb || !ib) continue;
                const std::array<const rhi::Buffer*, 1> vbs{vb};
                gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
                gcmd.bind_index_buffer(*ib, 0);
                const MeshLOD& lod = pd.mesh->lods()[pd.lod];
                for (const SubMesh& sm : lod.submeshes) {
                    gcmd.draw_indexed(sm.index_count, 1, sm.index_offset, static_cast<i32>(sm.vertex_offset), 0);
                }
            }
        }
        gcmd.end_render_pass();
    };
    m_graph->add_pass(shadow_pass);

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
            const rhi::Buffer* vb = pd.mesh->vertex_buffer(pd.lod);
            const rhi::Buffer* ib = pd.mesh->index_buffer(pd.lod);
            if (!vb || !ib) continue;
            const std::array<const rhi::Buffer*, 1> vbs{vb};
            gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
            gcmd.bind_index_buffer(*ib, 0);
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
            const rhi::Buffer* vb = pd.mesh->vertex_buffer(pd.lod);
            const rhi::Buffer* ib = pd.mesh->index_buffer(pd.lod);
            if (!vb || !ib) continue;
            const std::array<const rhi::Buffer*, 1> vbs{vb};
            gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
            gcmd.bind_index_buffer(*ib, 0);
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
    lighting_pass.reads = {m_gbuffer0_handle, m_gbuffer1_handle, m_gbuffer2_handle, m_depth_handle,
                           rg_shadow};
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
            tppd.push_constant_size = 16; // exposure + vignette + saturation + pad
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
        // Must match tonemap.frag PushConstants (std140-adjacent packing).
        const float tonemap_push[4] = {m_exposure, m_postfx.vignette, m_postfx.saturation, 0.0f};
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
    return true;
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

} // namespace nf::rendering
