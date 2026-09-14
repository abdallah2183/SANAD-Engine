// NF/Sample/Triangle/main.cpp
// First milestone: Window → Graphics Device → Render Triangle
// Design doc Section 305: "Open Window → Create Device → Render Triangle"
//
// The sample is deliberately thin: every rendering decision lives in the RHI,
// and this file only wires objects together. If something here needs to know
// about Vulkan specifically, that logic belongs one layer down.

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/RHI/RHI.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nf::sample::triangle {

namespace {

/// Environment lookup without the MSVC "unsafe CRT" deprecation noise.
/// Returns nullopt when the variable is unset.
std::optional<std::string> env_var(const char* name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    usize size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

/// Directory holding the compiled SPIR-V, injected by CMake.
/// Falls back to a path relative to the executable so the sample also works
/// when it is launched from somewhere other than the project root.
#ifndef NF_TRIANGLE_SHADER_DIR
    #define NF_TRIANGLE_SHADER_DIR ""
#endif

std::filesystem::path shader_directory() {
    // 1. Compile-time location (the normal case).
    if (const std::string_view configured = NF_TRIANGLE_SHADER_DIR; !configured.empty()) {
        if (std::filesystem::exists(configured)) {
            return std::filesystem::path(configured);
        }
    }

    // 2. Next to the executable — works for installed / relocated builds.
    std::error_code ec;
    std::filesystem::path exe_dir = std::filesystem::current_path(ec);

    for (std::string_view candidate : { "Samples/Triangle/shaders",
                                        "../Samples/Triangle/shaders" }) {
        if (std::filesystem::exists(exe_dir / candidate)) {
            return exe_dir / candidate;
        }
    }

    return {};
}

/// Loads a SPIR-V binary from disk into a byte vector.
std::vector<u8> load_spirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to open shader file: {}", path.string());
        return {};
    }

    const auto size = static_cast<usize>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<u8> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));

    if (static_cast<usize>(file.gcount()) != size) {
        NF_LOG_ERROR(LogCategory::RHI, "Short read on shader file: {}", path.string());
        return {};
    }

    return data;
}

struct SampleConfig {
    u32 max_frames = 0; // 0 = run until the window closes
    bool enable_validation = false;
};

SampleConfig parse_args(int argc, char** argv) {
    SampleConfig config;

    // An environment variable lets CI drive the sample without arguments.
    if (const auto env_frames = env_var("NF_TRIANGLE_FRAMES")) {
        config.max_frames = static_cast<u32>(std::atoi(env_frames->c_str()));
    }
    config.enable_validation = env_var("NF_TRIANGLE_VALIDATION").has_value();

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            config.max_frames = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg.starts_with("--frames=")) {
            config.max_frames = static_cast<u32>(std::atoi(arg.substr(9).data()));
        } else if (arg == "--validation") {
            config.enable_validation = true;
        } else if (arg == "--help") {
            std::cout << "NOVAForge Triangle sample\n"
                      << "  --frames N      Render N frames then exit (0 = until closed)\n"
                      << "  --validation    Request Vulkan validation layers\n";
        }
    }

    return config;
}

/// Renders the triangle. Returns the number of frames actually presented, or
/// a negative value on failure.
i32 run(const SampleConfig& config) {
    // --- Load shaders -----------------------------------------------------
    // SPIR-V binaries are compiled from GLSL at build time; loading them first
    // keeps the failure path cheap (no device teardown needed).
    const std::filesystem::path shader_dir = shader_directory();
    if (shader_dir.empty()) {
        NF_LOG_FATAL(LogCategory::RHI, "Could not locate the shader directory");
        return -1;
    }

    auto vert_code = load_spirv(shader_dir / "triangle_vert.spv");
    auto frag_code = load_spirv(shader_dir / "triangle_frag.spv");

    if (vert_code.empty() || frag_code.empty()) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to load SPIR-V shaders from {}",
                     shader_dir.string());
        return -1;
    }

    // --- Create window ----------------------------------------------------
    WindowDesc wdesc;
    wdesc.width = 1280;
    wdesc.height = 720;
    wdesc.title = "NOVAForge Engine — Triangle";
    wdesc.vsync = true;

    Window window;
    if (!window.create(wdesc)) {
        NF_LOG_FATAL(LogCategory::Platform, "Failed to create window");
        return -1;
    }

    if (window.width() == 0 || window.height() == 0) {
        NF_LOG_FATAL(LogCategory::Platform, "Window has a zero-sized client area");
        window.destroy();
        return -1;
    }

    // --- Create graphics device (Vulkan) ----------------------------------
    auto device = rhi::create_device();
    rhi::DeviceDesc dev_desc;
    dev_desc.window_handle = window.native_handle();
    dev_desc.enable_validation = config.enable_validation;
    if (!device || !device->init(dev_desc)) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to initialize graphics device");
        window.destroy();
        return -1;
    }

    NF_LOG_INFO(LogCategory::RHI, "Graphics device initialized: {}", device->backend_name());

    // --- Create swapchain -------------------------------------------------
    rhi::SwapchainDesc swapchain_desc;
    swapchain_desc.width = window.width();
    swapchain_desc.height = window.height();
    swapchain_desc.format = rhi::Format::B8G8R8A8_UNorm;
    swapchain_desc.present = rhi::PresentMode::FIFO;
    swapchain_desc.image_count = 2;

    auto swapchain = device->create_swapchain(swapchain_desc);
    if (!swapchain) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to create swapchain");
        device->shutdown();
        window.destroy();
        return -1;
    }

    NF_LOG_INFO(LogCategory::RHI, "Swapchain created: {}x{}, {} images",
                swapchain->width(), swapchain->height(), swapchain->image_count());

    // --- Create shader modules --------------------------------------------
    rhi::ShaderModuleDesc vert_desc{ vert_code, rhi::ShaderStage::Vertex };
    rhi::ShaderModuleDesc frag_desc{ frag_code, rhi::ShaderStage::Fragment };

    auto vs = device->create_shader_module(vert_desc);
    auto fs = device->create_shader_module(frag_desc);

    if (!vs || !fs) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to create shader modules");
        swapchain.reset();
        device->shutdown();
        window.destroy();
        return -1;
    }

    // --- Create render pass -----------------------------------------------
    // One color attachment matching the swapchain format, no depth.
    rhi::ColorAttachment color_attachment;
    color_attachment.format = swapchain->format();
    color_attachment.blend_enabled = false;

    const std::array<rhi::ColorAttachment, 1> color_attachments{ color_attachment };

    rhi::RenderPassDesc rp_desc;
    rp_desc.color_attachments = std::span<const rhi::ColorAttachment>(color_attachments);
    rp_desc.has_depth = false;
    // The color attachment IS a swapchain image, so the pass must leave it in
    // PRESENT_SRC_KHR. Leaving this false yields COLOR_ATTACHMENT_OPTIMAL and
    // every vkQueuePresentKHR fails validation (VUID-VkPresentInfoKHR-
    // pImageIndices-01430).
    rp_desc.present_source = true;

    auto render_pass = device->create_render_pass(rp_desc);
    if (!render_pass) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to create render pass");
        fs.reset();
        vs.reset();
        swapchain.reset();
        device->shutdown();
        window.destroy();
        return -1;
    }

    // --- Create pipeline --------------------------------------------------
    // No vertex buffer — the vertex shader uses gl_VertexIndex to generate
    // positions from a hardcoded array. This is the simplest correct Vulkan
    // triangle and the foundation all future rendering builds on.
    rhi::PipelineDesc pipeline_desc;
    pipeline_desc.vs = vs.get();
    pipeline_desc.fs = fs.get();
    pipeline_desc.topology = rhi::PrimitiveTopology::TriangleList;
    pipeline_desc.rasterizer.cull_mode = rhi::CullMode::None;
    pipeline_desc.depth.test_enabled = false;
    pipeline_desc.depth.write_enabled = false;
    pipeline_desc.render_pass = render_pass.get();
    // No vertex layout — the shader generates vertices from gl_VertexIndex.
    pipeline_desc.vertex_layout.stride = 0;

    auto pipeline = device->create_pipeline(pipeline_desc);
    if (!pipeline) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to create graphics pipeline");
        render_pass.reset();
        fs.reset();
        vs.reset();
        swapchain.reset();
        device->shutdown();
        window.destroy();
        return -1;
    }

    // --- Create per-frame resources ---------------------------------------
    // One framebuffer per swapchain image, each wrapping one swapchain image
    // as a color attachment. The render pass and framebuffer must be
    // compatible (same attachment count and formats), which holds because both
    // are built from the same attachment description.
    const u32 image_count = swapchain->image_count();
    std::vector<std::unique_ptr<rhi::Framebuffer>> framebuffers(image_count);

    for (u32 i = 0; i < image_count; ++i) {
        rhi::Texture* tex = swapchain->get_texture(i);
        if (!tex) {
            NF_LOG_FATAL(LogCategory::RHI, "Swapchain image {} has no texture view", i);
            return -1;
        }

        const std::array<rhi::Texture*, 1> attachments{ tex };

        framebuffers[i] = device->create_framebuffer(
            *render_pass, std::span<rhi::Texture* const>(attachments), nullptr);

        if (!framebuffers[i]) {
            NF_LOG_FATAL(LogCategory::RHI, "Failed to create framebuffer {}", i);
            return -1;
        }
    }

    // --- Synchronization --------------------------------------------------
    //
    // Three primitives, each with a distinct lifetime rule:
    //
    // image_available — signaled by acquire, waited on by the submit. One is
    //   enough: the semaphore is only re-signaled after the previous submit has
    //   waited on it, which the frame fence guarantees.
    //
    // render_finished — signaled by the submit, waited on by presentation.
    //   This one MUST be per swapchain image. A presentation operation keeps
    //   its semaphore in use until the present completes, and the frame fence
    //   says nothing about presentation: it is signaled when the queue work
    //   ends, which can be well before the image is actually shown. Reusing a
    //   single semaphore therefore races with the presentation engine, and the
    //   validation layer flags it. Indexing by image index makes reuse safe:
    //   an image can only be re-acquired once its previous present finished.
    //
    // frame_fence — tells the CPU when the command buffer may be reset. One
    //   frame in flight is the simplest correct scheme; it leaves performance
    //   on the table but pins the lifetime rules down explicitly.
    auto image_available = device->create_semaphore();
    auto frame_fence = device->create_fence(true); // Start signaled so the first wait succeeds
    auto cmd = device->create_command_buffer();

    std::vector<std::unique_ptr<rhi::Semaphore>> render_finished(image_count);
    bool sync_ok = static_cast<bool>(image_available) &&
                   static_cast<bool>(frame_fence) &&
                   static_cast<bool>(cmd);

    for (u32 i = 0; i < image_count && sync_ok; ++i) {
        render_finished[i] = device->create_semaphore();
        sync_ok = sync_ok && static_cast<bool>(render_finished[i]);
    }

    if (!sync_ok) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to create synchronization primitives");
        return -1;
    }

    // Clear color: deep blue-black background.
    const std::array<rhi::ClearValue, 1> clear_values{
        rhi::ClearValue{ 0.03f, 0.03f, 0.07f, 1.0f }
    };

    NF_LOG_INFO(LogCategory::RHI, "All resources created — entering render loop");

    // --- Main loop ---------------------------------------------------------
    Clock clock;
    u32 frame_count = 0;
    bool swapchain_lost = false;

    while (!window.should_close() && !swapchain_lost) {
        window.poll_events();

        // Wait for the previous frame to finish — the fence was signaled at
        // the end of the last submit. The command buffer may only be reset
        // once the GPU is done reading it.
        frame_fence->wait();
        frame_fence->reset();

        // Acquire the next swapchain image. The semaphore is signaled when
        // the image is ready for rendering.
        const u32 image_index = swapchain->acquire_next_image(*image_available);
        if (image_index == u32_max) {
            NF_LOG_WARN(LogCategory::RHI, "Swapchain out of date — stopping the loop");
            swapchain_lost = true;
            break;
        }

        // Record commands
        cmd->reset();
        cmd->begin();

        cmd->begin_render_pass(*render_pass, *framebuffers[image_index],
                               std::span<const rhi::ClearValue>(clear_values));

        cmd->bind_pipeline(*pipeline);

        // Dynamic viewport/scissor — the pipeline was created with dynamic
        // viewport and scissor states, so we must set them per-frame.
        cmd->set_viewport(0, 0, swapchain->width(), swapchain->height());
        cmd->set_scissor(0, 0, swapchain->width(), swapchain->height());

        // Draw a full triangle: 3 vertices, no index buffer, no instance count.
        cmd->draw(3);

        cmd->end_render_pass();
        cmd->end();

        // Submit with proper synchronization:
        // - Wait on image_available at the color attachment output stage so
        //   the GPU doesn't write to the image before it's acquired.
        // - Signal render_finished when the draw is done so presentation
        //   can wait on it.
        // - Signal frame_fence so the CPU knows when the frame is complete.
        const std::array<const rhi::Semaphore*, 1> wait_sems{ image_available.get() };
        const std::array<rhi::PipelineStage, 1> wait_stages{
            rhi::PipelineStage::ColorAttachmentOutput
        };
        const std::array<const rhi::Semaphore*, 1> signal_sems{
            render_finished[image_index].get()
        };

        rhi::SubmitInfo submit_info;
        submit_info.wait_semaphores = std::span<const rhi::Semaphore* const>(wait_sems);
        submit_info.wait_stages = std::span<const rhi::PipelineStage>(wait_stages);
        submit_info.signal_semaphores = std::span<const rhi::Semaphore* const>(signal_sems);
        submit_info.signal_fence = frame_fence.get();

        device->submit(*cmd, submit_info);

        // Present: wait on this image's render-complete semaphore before the
        // presentation engine reads it.
        const std::array<const rhi::Semaphore*, 1> present_wait_sems{
            render_finished[image_index].get()
        };
        swapchain->present(image_index,
                           std::span<const rhi::Semaphore* const>(present_wait_sems));

        ++frame_count;

        if (config.max_frames > 0 && frame_count >= config.max_frames) {
            NF_LOG_INFO(LogCategory::RHI, "Reached the requested frame budget ({})",
                        config.max_frames);
            break;
        }
    }

    // --- Shutdown ---------------------------------------------------------
    // The GPU may still be executing the last submission; nothing below may be
    // destroyed while it is in flight.
    device->wait_idle();

    NF_LOG_INFO(LogCategory::RHI, "Rendered {} frames", frame_count);

    // Destroy in reverse dependency order. The unique_ptrs own the GPU
    // objects, so simply clearing them in reverse order is enough.
    cmd.reset();
    frame_fence.reset();
    render_finished.clear();
    image_available.reset();
    framebuffers.clear();
    pipeline.reset();
    render_pass.reset();
    fs.reset();
    vs.reset();
    swapchain.reset();

    device->shutdown();
    device.reset();

    window.destroy();

    return static_cast<i32>(frame_count);
}

} // namespace

int run_sample(int argc, char** argv) {
    const SampleConfig config = parse_args(argc, argv);

    // --- Initialize logging ------------------------------------------------
    Logger& logger = Logger::instance();
    logger.add_sink(Logger::make_console_sink());
    logger.set_min_level(LogLevel::Debug);

    NF_LOG_INFO(LogCategory::Core, "=== NOVAForge Engine — Triangle Sample ===");

    platform_init();

    const i32 frames = run(config);

    platform_shutdown();

    if (frames < 0) {
        NF_LOG_ERROR(LogCategory::Core, "=== Triangle Sample failed ===");
        return 1;
    }

    NF_LOG_INFO(LogCategory::Core, "=== Triangle Sample exited cleanly ===");
    return 0;
}

} // namespace nf::sample::triangle

int main(int argc, char** argv) {
    return nf::sample::triangle::run_sample(argc, argv);
}
