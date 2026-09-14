// Samples/TexturedQuad/main.cpp — v0.2 now via Rendering layer
//
//   Sample → Rendering (RenderGraph + Material + PipelineCache) → RHI → GPU
// This is the first sample that does not talk to Vulkan directly.

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/Material.hpp>
#include <NF/Rendering/PipelineCache.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nf::sample::quad {

namespace {

std::optional<std::string> env_var(const char* name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    usize size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return std::nullopt;
    std::string value(buffer); std::free(buffer); return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

#ifndef NF_QUAD_SHADER_DIR
    #define NF_QUAD_SHADER_DIR ""
#endif

std::filesystem::path shader_directory() {
    if (const std::string_view configured = NF_QUAD_SHADER_DIR; !configured.empty()) {
        if (std::filesystem::exists(configured)) return std::filesystem::path(configured);
    }
    std::error_code ec;
    std::filesystem::path exe_dir = std::filesystem::current_path(ec);
    for (std::string_view candidate : { "Samples/TexturedQuad/shaders", "../Samples/TexturedQuad/shaders" }) {
        if (std::filesystem::exists(exe_dir / candidate)) return exe_dir / candidate;
    }
    return {};
}

std::vector<u8> load_spirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { NF_LOG_ERROR(LogCategory::RHI, "Failed to open shader file: {}", path.string()); return {}; }
    const auto size = static_cast<usize>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (static_cast<usize>(file.gcount()) != size) { NF_LOG_ERROR(LogCategory::RHI, "Short read on shader file: {}", path.string()); return {}; }
    return data;
}

struct Vertex { float x, y; float u, v; };
constexpr std::array<Vertex, 4> kVertices{{ { -0.8f, -0.8f, 0.0f, 0.0f }, {  0.8f, -0.8f, 1.0f, 0.0f }, {  0.8f,  0.8f, 1.0f, 1.0f }, { -0.8f,  0.8f, 0.0f, 1.0f } }};
constexpr std::array<u32, 6> kIndices{ 0, 1, 2, 2, 3, 0 };
constexpr u32 kTexWidth = 128;
constexpr u32 kTexHeight = 128;

std::vector<u8> make_texture() {
    std::vector<u8> data(kTexWidth * kTexHeight * 4);
    for (u32 y = 0; y < kTexHeight; ++y) for (u32 x = 0; x < kTexWidth; ++x) {
        const usize offset = (static_cast<usize>(y) * kTexWidth + x) * 4;
        const bool checker = ((x / 8) + (y / 8)) % 2 == 0;
        const float gradient = static_cast<float>(x + y) / static_cast<float>(kTexWidth + kTexHeight);
        const u8 base = checker ? 235 : 60;
        data[offset + 0] = static_cast<u8>(base * (0.35f + 0.65f * gradient));
        data[offset + 1] = static_cast<u8>(base * 0.55f);
        data[offset + 2] = static_cast<u8>(base * (1.0f - 0.65f * gradient));
        data[offset + 3] = 255;
    }
    return data;
}

struct SampleConfig { u32 max_frames = 0; bool enable_validation = false; };

SampleConfig parse_args(int argc, char** argv) {
    SampleConfig config;
    if (const auto env_frames = env_var("NF_QUAD_FRAMES")) config.max_frames = static_cast<u32>(std::atoi(env_frames->c_str()));
    config.enable_validation = env_var("NF_QUAD_VALIDATION").has_value();
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) config.max_frames = static_cast<u32>(std::atoi(argv[++i]));
        else if (arg.starts_with("--frames=")) config.max_frames = static_cast<u32>(std::atoi(arg.substr(9).data()));
        else if (arg == "--validation") config.enable_validation = true;
        else if (arg == "--help") std::cout << "NOVAForge Textured Quad sample\n  --frames N      Render N frames then exit (0 = until closed)\n  --validation    Request Vulkan validation layers\n";
    }
    return config;
}

std::unique_ptr<rhi::Buffer> upload_buffer(rhi::IGraphicsDevice& device, rhi::BufferUsage usage, const void* data, usize size) {
    rhi::BufferDesc staging_desc{}; staging_desc.size = size; staging_desc.usage = rhi::BufferUsage::TransferSrc; staging_desc.memory = rhi::MemoryUsage::CPUToGPU;
    auto staging = device.create_buffer(staging_desc); if (!staging) return nullptr;
    void* mapped = staging->map(); if (!mapped) return nullptr;
    std::memcpy(mapped, data, size); staging->unmap();
    rhi::BufferDesc gpu_desc{}; gpu_desc.size = size; gpu_desc.usage = usage | rhi::BufferUsage::TransferDst; gpu_desc.memory = rhi::MemoryUsage::GPUOnly;
    auto gpu_buffer = device.create_buffer(gpu_desc); if (!gpu_buffer) return nullptr;
    auto cmd = device.create_command_buffer(); auto fence = device.create_fence(false);
    if (!cmd || !fence) return nullptr;
    cmd->begin(); cmd->copy_buffer(*staging, *gpu_buffer, 0, 0, size); cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{ .signal_fence = fence.get() });
    fence->wait(); device.wait_idle();
    return gpu_buffer;
}

i32 run(const SampleConfig& config) {
    const std::filesystem::path shader_dir = shader_directory();
    if (shader_dir.empty()) { NF_LOG_FATAL(LogCategory::RHI, "Could not locate the shader directory"); return -1; }
    auto vert_code = load_spirv(shader_dir / "textured_quad_vert.spv");
    auto frag_code = load_spirv(shader_dir / "textured_quad_frag.spv");
    if (vert_code.empty() || frag_code.empty()) { NF_LOG_FATAL(LogCategory::RHI, "Failed to load SPIR-V shaders from {}", shader_dir.string()); return -1; }

    WindowDesc wdesc{}; wdesc.width=1280; wdesc.height=720; wdesc.title="NOVAForge Engine — Textured Quad (RenderGraph)"; wdesc.vsync=true;
    Window window; if (!window.create(wdesc)) { NF_LOG_FATAL(LogCategory::Platform, "Failed to create window"); return -1; }
    if (window.width()==0 || window.height()==0) { NF_LOG_FATAL(LogCategory::Platform, "Window has a zero-sized client area"); window.destroy(); return -1; }

    auto device = rhi::create_device();
    rhi::DeviceDesc dev_desc{}; dev_desc.window_handle = window.native_handle(); dev_desc.enable_validation = config.enable_validation;
    if (!device || !device->init(dev_desc)) { NF_LOG_FATAL(LogCategory::RHI, "Failed to initialize graphics device"); window.destroy(); return -1; }
    NF_LOG_INFO(LogCategory::RHI, "Graphics device initialized: {} (validation: {})", device->backend_name(), config.enable_validation ? "on" : "auto");

    rhi::SwapchainDesc swapchain_desc{}; swapchain_desc.width=window.width(); swapchain_desc.height=window.height();
    swapchain_desc.format=rhi::Format::B8G8R8A8_UNorm; swapchain_desc.present=rhi::PresentMode::FIFO; swapchain_desc.image_count=2;
    auto swapchain = device->create_swapchain(swapchain_desc);
    if (!swapchain) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create swapchain"); device->shutdown(); window.destroy(); return -1; }
    NF_LOG_INFO(LogCategory::RHI, "Swapchain created: {}x{}, {} images", swapchain->width(), swapchain->height(), swapchain->image_count());

    auto vs = device->create_shader_module(rhi::ShaderModuleDesc{ vert_code, rhi::ShaderStage::Vertex });
    auto fs = device->create_shader_module(rhi::ShaderModuleDesc{ frag_code, rhi::ShaderStage::Fragment });
    if (!vs || !fs) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create shader modules"); swapchain.reset(); device->shutdown(); window.destroy(); return -1; }

    rhi::ColorAttachment color_attachment{}; color_attachment.format=swapchain->format(); color_attachment.blend_enabled=false;
    const std::array<rhi::ColorAttachment,1> color_attachments{ color_attachment };
    rhi::RenderPassDesc rp_desc{}; rp_desc.color_attachments=std::span<const rhi::ColorAttachment>(color_attachments); rp_desc.has_depth=false;
    // Swapchain image as the color attachment: the pass must end in
    // PRESENT_SRC_KHR or every vkQueuePresentKHR fails validation.
    rp_desc.present_source = true;
    auto render_pass = device->create_render_pass(rp_desc);
    if (!render_pass) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create render pass"); swapchain.reset(); device->shutdown(); window.destroy(); return -1; }

    // --- Rendering layer scope: all Rendering objects must be destroyed before device shutdown ---
    int rendered_frames = -1;
    {
        rendering::PipelineCache pipeline_cache(*device);
        const std::array<rhi::DescriptorBinding,1> bindings{{ {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1} }};
        rhi::DescriptorSetLayoutDesc layout_desc{}; layout_desc.bindings=std::span<const rhi::DescriptorBinding>(bindings);
        auto set_layout = device->create_descriptor_set_layout(layout_desc);
        if (!set_layout) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create descriptor set layout"); return -1; }

        const std::array<rhi::VertexAttrib,2> attribs{{ {0, offsetof(Vertex,x), rhi::Format::R32G32_SFloat}, {1, offsetof(Vertex,u), rhi::Format::R32G32_SFloat} }};
        rhi::VertexLayout vl{}; vl.binding=0; vl.stride=sizeof(Vertex); vl.attributes=std::span<const rhi::VertexAttrib>(attribs);

        rendering::MaterialDesc mat_desc{};
        mat_desc.vs=vs.get(); mat_desc.fs=fs.get(); mat_desc.render_pass=render_pass.get();
        mat_desc.descriptor_set_layout=set_layout.get(); mat_desc.vertex_layout=vl;
        mat_desc.topology=rhi::PrimitiveTopology::TriangleList;
        mat_desc.rasterizer.cull_mode=rhi::CullMode::None;
        rendering::Material material(*device, pipeline_cache, mat_desc);
        if (!material.valid()) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create material (pipeline)"); return -1; }
        NF_LOG_INFO(LogCategory::RHI, "Material created (pipeline cache size {})", pipeline_cache.size());

        auto descriptor_allocator = device->create_descriptor_allocator(32);
        if (!descriptor_allocator) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create descriptor allocator"); return -1; }
        rendering::MaterialInstance mat_instance(material, *descriptor_allocator);
        if (!mat_instance.valid() && set_layout) { NF_LOG_FATAL(LogCategory::RHI, "Failed to allocate material instance set"); return -1; }

        // Geometry
        auto vertex_buffer = upload_buffer(*device, rhi::BufferUsage::Vertex, kVertices.data(), sizeof(Vertex)*kVertices.size());
        auto index_buffer = upload_buffer(*device, rhi::BufferUsage::Index, kIndices.data(), sizeof(u32)*kIndices.size());
        if (!vertex_buffer || !index_buffer) { NF_LOG_FATAL(LogCategory::RHI, "Failed to upload geometry"); return -1; }
        NF_LOG_INFO(LogCategory::RHI, "Geometry uploaded: {} vertices, {} indices", kVertices.size(), kIndices.size());

        // Texture — use UploadContext (async foundation) for the initial upload
        rhi::TextureDesc tex_desc{}; tex_desc.width=kTexWidth; tex_desc.height=kTexHeight; tex_desc.format=rhi::Format::R8G8B8A8_UNorm; tex_desc.usage=rhi::ImageUsage::Sampled|rhi::ImageUsage::TransferDst;
        auto texture = device->create_texture(tex_desc); if (!texture) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create texture"); return -1; }
        const std::vector<u8> texel_data = make_texture();
        rhi::BufferDesc tex_staging_desc{}; tex_staging_desc.size=texel_data.size(); tex_staging_desc.usage=rhi::BufferUsage::TransferSrc; tex_staging_desc.memory=rhi::MemoryUsage::CPUToGPU;
        auto tex_staging = device->create_buffer(tex_staging_desc); if (!tex_staging) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create texture staging buffer"); return -1; }
        void* tex_mapped = tex_staging->map(); if (!tex_mapped) { NF_LOG_FATAL(LogCategory::RHI, "Failed to map texture staging buffer"); return -1; }
        std::memcpy(tex_mapped, texel_data.data(), texel_data.size()); tex_staging->unmap();
        {
            auto upload_ctx = device->create_upload_context();
            if (!upload_ctx) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create upload context"); return -1; }
            upload_ctx->copy_buffer_to_texture(*tex_staging, *texture, 0, 0,0,kTexWidth,kTexHeight);
            auto fence = upload_ctx->submit();
            if (!fence || !fence->wait()) { NF_LOG_FATAL(LogCategory::RHI, "Texture upload failed"); return -1; }
            device->wait_idle();
            auto trans_cmd = device->create_command_buffer(); auto trans_fence = device->create_fence(false);
            trans_cmd->begin(); trans_cmd->transition_texture_for_sampling(*texture); trans_cmd->end();
            device->submit(*trans_cmd, rhi::SubmitInfo{.signal_fence=trans_fence.get()});
            trans_fence->wait(); device->wait_idle();
        }
        NF_LOG_INFO(LogCategory::RHI, "Texture uploaded: {}x{} via UploadContext", kTexWidth, kTexHeight);

        rhi::SamplerDesc sampler_desc{}; sampler_desc.mag=rhi::Filter::Linear; sampler_desc.min=rhi::Filter::Linear;
        sampler_desc.mip=rhi::MipMapMode::None; sampler_desc.address_u=rhi::AddressMode::ClampToEdge; sampler_desc.address_v=rhi::AddressMode::ClampToEdge;
        auto sampler = device->create_sampler(sampler_desc);
        rhi::TextureViewDesc view_desc{}; view_desc.texture=texture.get(); view_desc.dimension=rhi::ViewDimension::View2D;
        view_desc.aspect=rhi::ImageAspect::Color; view_desc.base_mip=0; view_desc.mip_count=1; view_desc.base_layer=0; view_desc.layer_count=1;
        auto texture_view = device->create_texture_view(view_desc);
        if (!sampler || !texture_view) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create sampler or texture view"); return -1; }
        mat_instance.set_texture(0, *texture_view, *sampler);
        mat_instance.update();
        NF_LOG_INFO(LogCategory::RHI, "Material instance bound to texture view (2D, Color, mip0)");

        const u32 image_count = swapchain->image_count();
        std::vector<std::unique_ptr<rhi::Framebuffer>> framebuffers(image_count);
        for (u32 i=0;i<image_count;++i){
            rhi::Texture* tex = swapchain->get_texture(i);
            if (!tex) { NF_LOG_FATAL(LogCategory::RHI, "Swapchain image {} has no texture view", i); return -1; }
            const std::array<rhi::Texture*,1> attachments{ tex };
            framebuffers[i] = device->create_framebuffer(*render_pass, std::span<rhi::Texture* const>(attachments), nullptr);
            if (!framebuffers[i]) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create framebuffer {}", i); return -1; }
        }

        auto image_available = device->create_semaphore(); auto frame_fence = device->create_fence(true); auto cmd = device->create_command_buffer();
        std::vector<std::unique_ptr<rhi::Semaphore>> render_finished(image_count);
        bool sync_ok = static_cast<bool>(image_available) && static_cast<bool>(frame_fence) && static_cast<bool>(cmd);
        for (u32 i=0;i<image_count && sync_ok;++i){ render_finished[i]=device->create_semaphore(); sync_ok = sync_ok && static_cast<bool>(render_finished[i]); }
        if (!sync_ok){ NF_LOG_FATAL(LogCategory::RHI, "Failed to create synchronization primitives"); return -1; }

        const std::array<rhi::ClearValue,1> clear_values{ rhi::ClearValue{ 0.03f, 0.03f, 0.07f, 1.0f } };
        NF_LOG_INFO(LogCategory::RHI, "All rendering resources created — entering render loop (RenderGraph path)");

        Clock clock; u32 frame_count=0; bool swapchain_lost=false;
        while (!window.should_close() && !swapchain_lost) {
            window.poll_events();
            frame_fence->wait(); frame_fence->reset();
            const u32 image_index = swapchain->acquire_next_image(*image_available);
            if (image_index==u32_max){ NF_LOG_WARN(LogCategory::RHI, "Swapchain out of date — stopping the loop"); swapchain_lost=true; break; }

            rendering::RenderGraph graph(*device);
            auto rg_backbuffer = graph.import_texture("Backbuffer", swapchain->get_texture(image_index));
            auto rg_checker = graph.import_texture("Checker", texture.get());

            rendering::RGPassDesc quad_pass{};
            quad_pass.name = "TexturedQuad";
            quad_pass.reads = { rg_checker };
            quad_pass.writes = { rg_backbuffer };
            quad_pass.color_attachments = { rg_backbuffer };
            quad_pass.execute = [&](rhi::CommandBuffer& gcmd){
                gcmd.begin_render_pass(*render_pass, *framebuffers[image_index], std::span<const rhi::ClearValue>(clear_values));
                mat_instance.bind_pipeline_and_descriptors(gcmd);
                const std::array<const rhi::Buffer*,1> vbs{ vertex_buffer.get() };
                gcmd.bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
                gcmd.bind_index_buffer(*index_buffer, 0);
                gcmd.set_viewport(0,0,swapchain->width(), swapchain->height());
                gcmd.set_scissor(0,0,swapchain->width(), swapchain->height());
                gcmd.draw_indexed(static_cast<u32>(kIndices.size()));
                gcmd.end_render_pass();
            };
            graph.add_pass(quad_pass);
            NF_ASSERT(graph.compile(), "RenderGraph compile failed");

            cmd->reset(); cmd->begin();
            graph.execute(*cmd);
            cmd->end();

            const std::array<const rhi::Semaphore*,1> wait_sems{ image_available.get() };
            const std::array<rhi::PipelineStage,1> wait_stages{ rhi::PipelineStage::ColorAttachmentOutput };
            const std::array<const rhi::Semaphore*,1> signal_sems{ render_finished[image_index].get() };
            rhi::SubmitInfo submit_info{};
            submit_info.wait_semaphores=std::span<const rhi::Semaphore* const>(wait_sems);
            submit_info.wait_stages=std::span<const rhi::PipelineStage>(wait_stages);
            submit_info.signal_semaphores=std::span<const rhi::Semaphore* const>(signal_sems);
            submit_info.signal_fence=frame_fence.get();
            device->submit(*cmd, submit_info);

            const std::array<const rhi::Semaphore*,1> present_wait_sems{ render_finished[image_index].get() };
            swapchain->present(image_index, std::span<const rhi::Semaphore* const>(present_wait_sems));

            ++frame_count;
            if (config.max_frames>0 && frame_count>=config.max_frames){ NF_LOG_INFO(LogCategory::RHI, "Reached the requested frame budget ({})", config.max_frames); break; }
        }

        device->wait_idle();
        NF_LOG_INFO(LogCategory::RHI, "Rendered {} frames via RenderGraph+Material", frame_count);
        rendered_frames = static_cast<int>(frame_count);
        // Framebuffers and sync are destroyed here with the inner scope; the remaining
        // Rendering objects (mat_instance → allocator → material → cache) are destroyed
        // in the correct order when this block exits, before device shutdown.
    }

    // Now all Rendering objects are gone, safe to shut down the device
    render_pass.reset(); fs.reset(); vs.reset(); swapchain.reset();
    device->shutdown(); device.reset();
    window.destroy();
    return rendered_frames;
}

} // namespace

int run_sample(int argc, char** argv) {
    const SampleConfig config = parse_args(argc, argv);
    Logger& logger = Logger::instance(); logger.add_sink(Logger::make_console_sink()); logger.set_min_level(LogLevel::Debug);
    NF_LOG_INFO(LogCategory::Core, "=== NOVAForge Engine — Textured Quad Sample (RenderGraph) ===");
    platform_init();
    const i32 frames = run(config);
    platform_shutdown();
    if (frames < 0){ NF_LOG_ERROR(LogCategory::Core, "=== Textured Quad Sample failed ==="); return 1; }
    NF_LOG_INFO(LogCategory::Core, "=== Textured Quad Sample exited cleanly ({} frames) ===", frames);
    return 0;
}

} // namespace nf::sample::quad
int main(int argc, char** argv){ return nf::sample::quad::run_sample(argc, argv); }
