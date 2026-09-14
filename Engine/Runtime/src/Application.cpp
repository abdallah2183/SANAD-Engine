#include <NF/Runtime/Application.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/RHI/RHI.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <span>
#include <thread>

namespace nf::runtime {

Application::Application(const ApplicationConfig& config) : m_config(config) {}

Application::~Application() = default;

int Application::run() {
    Logger::instance().add_sink(Logger::make_console_sink());
    Logger::instance().set_min_level(LogLevel::Info);

    NF_LOG_INFO(LogCategory::Core, "=== NOVAForge Runtime ===");
    NF_LOG_INFO(LogCategory::Core, "Scene: {} Frames: {} Validation: {} Headless: {}",
                m_config.scene_path, m_config.max_frames, m_config.validation, m_config.headless);

    platform_init();
    JobSystem::instance().init(0);

    // VFS setup
    assets::VirtualFileSystem vfs;
    // Auto-detect project root as current path's parent with Engine/
    std::filesystem::path project_root = std::filesystem::current_path();
    for (int i = 0; i < 5; ++i) {
        if (std::filesystem::exists(project_root / "Engine") && std::filesystem::exists(project_root / "Content")) {
            break;
        }
        auto parent = project_root.parent_path();
        if (parent == project_root) {
            break;
        }
        project_root = parent;
    }
    // Fallback to executable dir's parent
    if (!std::filesystem::exists(project_root / "Content")) {
        project_root = std::filesystem::current_path();
    }
    auto setup_mount = [&](const std::string& logical, const std::filesystem::path& physical) {
        std::error_code ec;
        std::filesystem::create_directories(physical, ec);
        auto r = vfs.mount(logical, physical);
        if (!r.ok) {
            NF_LOG_WARN(LogCategory::Core, "VFS mount failed {} -> {}: {}", logical, physical.string(), r.error);
        } else {
            NF_LOG_INFO(LogCategory::Core, "VFS mount {} -> {}", logical, physical.string());
        }
    };
    setup_mount("engine://", project_root / "Engine");
    setup_mount("project://", project_root);
    setup_mount("content://", project_root / "Content");
    setup_mount("cache://", project_root / "Cache");

    // Asset Registry
    assets::AssetRegistry registry;
    std::string reg_err;
    bool has_registry = false;
    {
        auto r = vfs.exists("content://AssetRegistry.nfreg");
        if (r.ok && r.value) {
            if (registry.load(vfs, "content://AssetRegistry.nfreg", reg_err)) {
                NF_LOG_INFO(LogCategory::Core, "AssetRegistry loaded: {} entries", registry.size());
                has_registry = true;
            } else {
                NF_LOG_WARN(LogCategory::Core, "Failed to load registry: {}", reg_err);
            }
        } else {
            // Try cache://
            auto r2 = vfs.exists("cache://AssetRegistry.nfreg");
            if (r2.ok && r2.value && registry.load(vfs, "cache://AssetRegistry.nfreg", reg_err)) {
                NF_LOG_INFO(LogCategory::Core, "AssetRegistry loaded from cache: {} entries", registry.size());
                has_registry = true;
            }
        }
    }
    if (!has_registry) {
        NF_LOG_INFO(LogCategory::Core, "No AssetRegistry found, starting with empty registry");
    }

    // Window (unless headless)
    Window window;
    if (!m_config.headless) {
        WindowDesc wdesc{};
        wdesc.width = m_config.width;
        wdesc.height = m_config.height;
        wdesc.title = m_config.title;
        wdesc.vsync = m_config.vsync;
        if (!window.create(wdesc)) {
            NF_LOG_ERROR(LogCategory::Platform, "Failed to create window");
            JobSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }
        window.set_event_callback([&](WindowEvent ev, u32 w, u32 h) {
            if (ev == WindowEvent::Resize) {
                NF_LOG_INFO(LogCategory::Platform, "Window resized to {}x{}", w, h);
            }
        });
    }

    // Device and swapchain
    auto device = rhi::create_device();
    if (!device) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create device");
        if (!m_config.headless) {
            window.destroy();
        }
        JobSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }
    rhi::DeviceDesc ddesc{};
    ddesc.window_handle = m_config.headless ? nullptr : window.native_handle();
    ddesc.enable_validation = m_config.validation;
    if (!device->init(ddesc)) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to init device");
        if (!m_config.headless) {
            window.destroy();
        }
        JobSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }
    if (m_config.validation && !device->validation_enabled()) {
        NF_LOG_WARN(LogCategory::RHI, "Validation requested but layer unavailable; continuing without it");
    }

    std::unique_ptr<rhi::Swapchain> swapchain;
    if (!m_config.headless) {
        rhi::SwapchainDesc sc_desc{};
        sc_desc.width = window.width();
        sc_desc.height = window.height();
        sc_desc.format = rhi::Format::B8G8R8A8_UNorm;
        sc_desc.present = m_config.vsync ? rhi::PresentMode::FIFO : rhi::PresentMode::Immediate;
        sc_desc.image_count = 2;
        swapchain = device->create_swapchain(sc_desc);
        if (!swapchain) {
            NF_LOG_ERROR(LogCategory::RHI, "Failed to create swapchain");
            device->shutdown();
            window.destroy();
            JobSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }
    }

    // Everything GPU-owned (AssetManager caches, Runtime renderer/meshes,
    // per-frame cmd/semaphores) lives inside this scope and is destroyed
    // before swapchain/device shutdown below. Destroying them after
    // vkDestroyDevice would leak and trip validation.
    uint32_t frame_count = 0;
    {
        assets::AssetManager asset_manager(vfs, registry, device.get());
        Runtime runtime(vfs, registry, asset_manager, *device, swapchain.get());

        std::string scene_err;
        bool scene_ok = true;
        if (!m_config.scene_path.empty()) {
            scene_ok = runtime.load_scene(m_config.scene_path, scene_err);
            if (!scene_ok) {
                NF_LOG_ERROR(LogCategory::Core, "Failed to load scene '{}': {}", m_config.scene_path, scene_err);
                // Missing scene is a hard failure for the sample: exit non-zero
                // after cleaning up GPU resources below.
                device->wait_idle();
                runtime.shutdown();
                asset_manager.clear();
                swapchain.reset();
                device->shutdown();
                if (!m_config.headless) {
                    window.destroy();
                }
                JobSystem::instance().shutdown();
                platform_shutdown();
                return 1;
            }
        }

        auto image_available = device->create_semaphore();
        auto frame_fence = device->create_fence(true);
        auto cmd = device->create_command_buffer();
        std::vector<std::unique_ptr<rhi::Semaphore>> render_finished;
        if (!m_config.headless && swapchain) {
            render_finished.reserve(swapchain->image_count());
            for (uint32_t i = 0; i < swapchain->image_count(); ++i) {
                render_finished.push_back(device->create_semaphore());
            }
        }
        if (!m_config.headless && (!image_available || !frame_fence || !cmd)) {
            NF_LOG_ERROR(LogCategory::RHI, "Failed to create per-frame sync objects");
            device->wait_idle();
            runtime.shutdown();
            asset_manager.clear();
            swapchain.reset();
            device->shutdown();
            window.destroy();
            JobSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }

        auto recreate_swapchain = [&]() -> bool {
            if (m_config.headless) {
                return false;
            }
            device->wait_idle();
            rhi::SwapchainDesc sc_desc{};
            sc_desc.width = window.width();
            sc_desc.height = window.height();
            if (sc_desc.width == 0 || sc_desc.height == 0) {
                return false;
            }
            sc_desc.format = rhi::Format::B8G8R8A8_UNorm;
            sc_desc.present = m_config.vsync ? rhi::PresentMode::FIFO : rhi::PresentMode::Immediate;
            sc_desc.image_count = 2;
            auto fresh = device->create_swapchain(sc_desc);
            if (!fresh) {
                NF_LOG_ERROR(LogCategory::RHI, "Swapchain recreation failed ({}x{})", sc_desc.width,
                             sc_desc.height);
                return false;
            }
            swapchain = std::move(fresh);
            runtime.on_swapchain_resized(swapchain.get());
            render_finished.clear();
            render_finished.reserve(swapchain->image_count());
            for (uint32_t i = 0; i < swapchain->image_count(); ++i) {
                render_finished.push_back(device->create_semaphore());
            }
            NF_LOG_INFO(LogCategory::RHI, "Swapchain recreated ({}x{})", swapchain->width(),
                        swapchain->height());
            return true;
        };

        Clock clock;
        while (true) {
            if (!m_config.headless) {
                window.poll_events();
                if (window.should_close()) {
                    break;
                }
                // Resize: recreate swapchain + renderer resolution targets.
                if (swapchain &&
                    (window.width() != swapchain->width() || window.height() != swapchain->height())) {
                    NF_LOG_INFO(LogCategory::RHI, "Resize detected: window {}x{} vs swapchain {}x{}",
                                window.width(), window.height(), swapchain->width(), swapchain->height());
                    if (!recreate_swapchain()) {
                        NF_LOG_ERROR(LogCategory::RHI, "Failed to handle resize; exiting");
                        break;
                    }
                }
            }

            const float dt = static_cast<float>(clock.elapsed_seconds());
            clock.reset();

            runtime.update(dt);

            if (!m_config.headless && swapchain) {
                frame_fence->wait();
                frame_fence->reset();

                const uint32_t image_index = swapchain->acquire_next_image(*image_available);
                if (image_index == 0xFFFFFFFF) {
                    NF_LOG_WARN(LogCategory::RHI, "Swapchain out of date; recreating");
                    if (!recreate_swapchain()) {
                        break;
                    }
                    continue;
                }
                if (image_index >= swapchain->image_count()) {
                    NF_LOG_ERROR(LogCategory::RHI, "Invalid swapchain image index {}", image_index);
                    break;
                }

                // Windowed Runtime: Scene → AssetManager → Renderer3D
                // (Depth → GBuffer → Lighting → Tonemap), submit, present.
                cmd->reset();
                cmd->begin();
                runtime.render(image_index, *cmd);
                cmd->end();

                rhi::SubmitInfo submit_info{};
                const std::array<const rhi::Semaphore*, 1> wait_sems{image_available.get()};
                const std::array<rhi::PipelineStage, 1> wait_stages{rhi::PipelineStage::ColorAttachmentOutput};
                const std::array<const rhi::Semaphore*, 1> signal_sems{
                    render_finished[image_index].get()};
                submit_info.wait_semaphores = std::span<const rhi::Semaphore* const>(wait_sems);
                submit_info.wait_stages = std::span<const rhi::PipelineStage>(wait_stages);
                submit_info.signal_semaphores = std::span<const rhi::Semaphore* const>(signal_sems);
                submit_info.signal_fence = frame_fence.get();
                device->submit(*cmd, submit_info);
                swapchain->present(image_index, std::span<const rhi::Semaphore* const>(signal_sems));
            } else {
                // Headless: no swapchain, no PRESENT. Update only.
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            ++frame_count;
            if (m_config.max_frames > 0 && frame_count >= m_config.max_frames) {
                break;
            }
            if (m_config.headless && frame_count >= (m_config.max_frames != 0 ? m_config.max_frames : 10)) {
                break;
            }
        }

        NF_LOG_INFO(LogCategory::Core, "Rendered {} frames (meshes={})", frame_count, runtime.mesh_count());
        if (runtime.renderer() != nullptr) {
            const auto& stats = runtime.renderer()->last_stats();
            NF_LOG_INFO(LogCategory::Core, "Last frame: extracted={} visible={} draws={}", stats.extracted,
                        stats.visible, stats.draw_calls);
        }
        if (m_config.validation) {
            const uint32_t verrs = rhi::validation_error_count();
            NF_LOG_INFO(LogCategory::RHI, "Validation errors: {}", verrs);
            if (verrs != 0) {
                NF_LOG_ERROR(LogCategory::RHI, "Validation failed with {} errors", verrs);
            }
        }
        // Explicit GPU cleanup while the device is still alive.
        // Per-frame objects (cmd/semaphores) are still alive here; they die at
        // scope end. Runtime/AssetManager GPU resources are freed now.
        device->wait_idle();
        runtime.shutdown();
        asset_manager.clear();
        device->wait_idle();
        NF_LOG_INFO(LogCategory::Core, "Shutting down runtime after {} frames", frame_count);
    }
    // All GPU objects from the scope above are gone (Runtime, AssetManager,
    // per-frame cmd/semaphores). Only the swapchain remains.
    swapchain.reset();
    device->wait_idle();
    uint32_t leaked = 0;
    {
        const uint32_t alive = device->alive_objects();
        NF_LOG_INFO(LogCategory::RHI, "Alive RHI objects before shutdown: {}", alive);
        if (alive != 0) {
            NF_LOG_ERROR(LogCategory::RHI, "Leaked RHI objects detected: {}", alive);
            leaked = alive;
        }
    }
    device->shutdown();
    if (!m_config.headless) {
        window.destroy();
    }
    JobSystem::instance().shutdown();
    platform_shutdown();

    if (m_config.validation && rhi::validation_error_count() != 0) {
        NF_LOG_ERROR(LogCategory::RHI, "Exiting with validation errors present");
        return 1;
    }
    if (leaked != 0) {
        NF_LOG_ERROR(LogCategory::RHI, "Exiting with leaked RHI objects present");
        return 1;
    }
    NF_LOG_INFO(LogCategory::Core, "=== NOVAForge Runtime exited cleanly ===");
    return 0;
}

} // namespace nf::runtime
