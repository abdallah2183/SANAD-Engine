// NOVAForgeEditor — native editor shell (Phase 4: Editor Foundation).
//
// Window + Vulkan device + swapchain + Runtime viewport (offscreen via
// Runtime::render_offscreen) + Dear ImGui docking panels. With --frames N the
// shell additionally runs a deterministic automation script that exercises
// outliner/inspector/assets/save/undo/play and logs machine-checkable proof
// lines, then exits (automation never overwrites the opened scene file).

#include <NF/Editor/AssetBrowser.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/UiRenderer.hpp>
#include <NF/Editor/UiShell.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/MaterialAsset.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>

#include <imgui.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

struct EditorConfig {
    std::string scene_path = "content://Scenes/Example.nfscene";
    uint32_t max_frames = 0;
    bool validation = false;
    bool headless = false;
};

EditorConfig parse_args(int argc, char** argv) {
    EditorConfig c;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value_of = [&](const std::string& prefix) -> std::string {
            if (arg.rfind(prefix, 0) == 0) {
                return arg.substr(prefix.size());
            }
            return {};
        };
        if (arg == "--scene" && i + 1 < argc) {
            c.scene_path = argv[++i];
        } else if (!value_of("--scene=").empty()) {
            c.scene_path = value_of("--scene=");
        } else if (arg == "--frames" && i + 1 < argc) {
            c.max_frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (!value_of("--frames=").empty()) {
            c.max_frames = static_cast<uint32_t>(std::atoi(value_of("--frames=").c_str()));
        } else if (arg == "--validation") {
            c.validation = true;
        } else if (arg == "--headless") {
            c.headless = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("NOVAForgeEditor (Phase 5)\n"
                        "  --scene <logical>   Scene to open (default content://Scenes/Example.nfscene)\n"
                        "  --frames N          Run N frames then exit (0 = interactive until close)\n"
                        "  --validation        Enable Vulkan validation\n"
                        "  --headless          No window (logic + offscreen viewport only)\n");
            std::exit(0);
        }
    }
    return c;
}

std::filesystem::path find_project_root() {
    std::filesystem::path root = std::filesystem::current_path();
    for (int i = 0; i < 5; ++i) {
        if (std::filesystem::exists(root / "Engine") && std::filesystem::exists(root / "Content")) {
            break;
        }
        const auto parent = root.parent_path();
        if (parent == root) {
            break;
        }
        root = parent;
    }
    if (!std::filesystem::exists(root / "Content")) {
        root = std::filesystem::current_path();
    }
    return root;
}

// Builds a picking/render camera from the scene's active camera.
nf::editor::ViewCamera view_camera_from_scene(const nf::scene::Scene* scene, float aspect) {
    nf::editor::ViewCamera cam;
    cam.aspect = (aspect > 0.0f) ? aspect : 16.0f / 9.0f;
    if (scene == nullptr) {
        return cam;
    }
    const auto& world = scene->world();
    for (auto e : world.query<nf::runtime::CameraComponent>()) {
        const auto* c = world.get<nf::runtime::CameraComponent>(e);
        if (c == nullptr || !c->is_active) {
            continue;
        }
        cam.fov_y_deg = c->fov_y;
        cam.near_plane = c->near_plane;
        cam.far_plane = c->far_plane;
        if (const auto* t = world.get<nf::scene::Transform>(e)) {
            cam.px = t->world_x;
            cam.py = t->world_y;
            cam.pz = t->world_z;
        }
        const float d2 = cam.px * cam.px + cam.py * cam.py + cam.pz * cam.pz;
        if (d2 < 0.25f) {
            cam.px = 0.0f;
            cam.py = 2.0f;
            cam.pz = 5.0f;
        }
        return cam;
    }
    return cam;
}

// Resolves the ImGui overlay shader directory (compiled define first,
// then well-known build/source locations).
std::filesystem::path resolve_imgui_shader_dir() {
#ifdef NF_EDITOR_IMGUI_SHADER_DIR
    {
        const std::filesystem::path p = NF_EDITOR_IMGUI_SHADER_DIR;
        if (std::filesystem::exists(p / "imgui_vert.spv")) {
            return p;
        }
    }
#endif
    const std::array<std::filesystem::path, 4> candidates{
        std::filesystem::path("build/DebugNinja/Shaders/EditorImGui"),
        std::filesystem::path("build/debug/Shaders/EditorImGui"),
        std::filesystem::path("Editor/shaders"),
        std::filesystem::path("../Editor/shaders"),
    };
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c / "imgui_vert.spv")) {
            return c;
        }
    }
    return {};
}

// Minimal solid-color BMP writer for automation artifacts (no encoder dep).
std::vector<uint8_t> make_bmp_solid(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    const int stride = ((w * 3 + 3) / 4) * 4;
    std::vector<uint8_t> out(static_cast<size_t>(54) + static_cast<size_t>(stride) * static_cast<size_t>(h),
                             0);
    auto put32 = [&](size_t off, uint32_t v) {
        out[off] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    auto put16 = [&](size_t off, uint16_t v) {
        out[off] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, static_cast<uint32_t>(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(h));
    put16(26, 1);
    put16(28, 24);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = out.data() + 54 + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(stride);
        for (int x = 0; x < w; ++x) {
            row[x * 3] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
    }
    return out;
}

std::optional<nf::ecs::Entity> find_first_mesh(nf::ecs::World* world) {
    if (world == nullptr) {
        return std::nullopt;
    }
    const auto meshes = world->query<nf::runtime::MeshComponent>();
    if (meshes.empty()) {
        return std::nullopt;
    }
    return meshes.front();
}

} // namespace

int main(int argc, char** argv) {
    const EditorConfig cfg = parse_args(argc, argv);

    nf::Logger::instance().add_sink(nf::Logger::make_console_sink());
    nf::Logger::instance().set_min_level(nf::LogLevel::Info);

    // Editor console captures everything from here on (bounded, filterable).
    nf::editor::ConsoleBuffer console;
    nf::Logger::instance().add_sink(nf::editor::make_console_sink(console));

    NF_LOG_INFO(nf::LogCategory::Editor, "=== NOVAForge Editor (Phase 4) ===");
    NF_LOG_INFO(nf::LogCategory::Editor, "Scene: {} Frames: {} Validation: {} Headless: {}", cfg.scene_path,
                cfg.max_frames, cfg.validation, cfg.headless);

    nf::platform_init();
    nf::JobSystem::instance().init(0);

    nf::assets::VirtualFileSystem vfs;
    const std::filesystem::path project_root = find_project_root();
    auto mount = [&](const std::string& logical, const std::filesystem::path& physical) {
        std::error_code ec;
        std::filesystem::create_directories(physical, ec);
        auto r = vfs.mount(logical, physical);
        if (!r.ok) {
            NF_LOG_WARN(nf::LogCategory::Editor, "VFS mount failed {} -> {}: {}", logical,
                        physical.string(), r.error);
        }
    };
    mount("engine://", project_root / "Engine");
    mount("project://", project_root);
    mount("content://", project_root / "Content");
    mount("cache://", project_root / "Cache");

    nf::assets::AssetRegistry registry;
    {
        std::string err;
        auto r = vfs.exists("content://AssetRegistry.nfreg");
        if (r.ok && r.value) {
            if (!registry.load(vfs, "content://AssetRegistry.nfreg", err)) {
                NF_LOG_WARN(nf::LogCategory::Editor, "Registry load failed: {}", err);
            }
        }
        NF_LOG_INFO(nf::LogCategory::Editor, "AssetRegistry: {} entries", registry.size());
    }

    nf::Window window;
    if (!cfg.headless) {
        nf::WindowDesc wdesc{};
        wdesc.width = 1280;
        wdesc.height = 720;
        wdesc.title = "NOVAForge Editor";
        wdesc.vsync = true;
        if (!window.create(wdesc)) {
            NF_LOG_ERROR(nf::LogCategory::Editor, "Failed to create window");
            nf::JobSystem::instance().shutdown();
            nf::platform_shutdown();
            return 1;
        }
    }

    auto device = nf::rhi::create_device();
    if (!device) {
        NF_LOG_ERROR(nf::LogCategory::Editor, "Failed to create device");
        return 1;
    }
    nf::rhi::DeviceDesc ddesc{};
    ddesc.window_handle = cfg.headless ? nullptr : window.native_handle();
    ddesc.enable_validation = cfg.validation;
    if (!device->init(ddesc)) {
        NF_LOG_ERROR(nf::LogCategory::Editor, "Failed to init device");
        return 1;
    }
    NF_LOG_INFO(nf::LogCategory::Editor, "Device: {} validation={}", device->backend_name(),
                device->validation_enabled() ? "on" : "off");

    std::unique_ptr<nf::rhi::Swapchain> swapchain;
    if (!cfg.headless) {
        nf::rhi::SwapchainDesc sc{};
        sc.width = window.width();
        sc.height = window.height();
        sc.format = nf::rhi::Format::B8G8R8A8_UNorm;
        sc.present = nf::rhi::PresentMode::FIFO;
        sc.image_count = 2;
        swapchain = device->create_swapchain(sc);
        if (!swapchain) {
            NF_LOG_ERROR(nf::LogCategory::Editor, "Failed to create swapchain");
            device->shutdown();
            return 1;
        }
    }

    int exit_code = 0;
    uint32_t frame_count = 0;
    {
        nf::assets::AssetManager manager(vfs, registry, device.get());
        nf::runtime::Runtime runtime(vfs, registry, manager, *device, swapchain.get());
        nf::editor::EditorApp app(vfs, registry, manager, console);
        app.attach_runtime(&runtime);

        std::string err;
        if (!app.open_scene(cfg.scene_path, err)) {
            NF_LOG_ERROR(nf::LogCategory::Editor, "Failed to open scene '{}': {}", cfg.scene_path, err);
            return 1;
        }
        {
            const auto rows = app.outliner_rows();
            std::string labels;
            for (const auto& r : rows) {
                if (!labels.empty()) {
                    labels += ", ";
                }
                labels += r.label;
            }
            NF_LOG_INFO(nf::LogCategory::Editor, "Outliner: {} entities [{}]", rows.size(), labels);
        }
        {
            const auto entries = app.browser_entries();
            size_t meshes = 0;
            bool has_cube = false;
            for (const auto& e : entries) {
                if (e.type == nf::assets::AssetType::Mesh) {
                    ++meshes;
                }
                if (e.logical_path.find("cube.nfmesh") != std::string::npos) {
                    has_cube = true;
                }
            }
            NF_LOG_INFO(nf::LogCategory::Editor, "Asset Browser: {} entries, {} meshes, cube.nfmesh={}",
                        entries.size(), meshes, has_cube ? "visible" : "MISSING");
        }

        // ImGui context + docking + Win32 backend (windowed mode only).
        nf::editor::UiInitResult ui;
        nf::editor::UiRenderer ui_renderer;
        if (!cfg.headless) {
            ui = nf::editor::ui_init(window.native_handle());
            NF_LOG_INFO(nf::LogCategory::Editor, "ImGui {} win32={} font={}", ui.context_ok ? "ready" : "FAILED",
                        ui.win32_ok ? "ok" : "off", ui.font_used);
            if (!ui.context_ok) {
                return 1;
            }
            window.set_message_hook(nf::editor::ui_handle_win32_message);
            const std::filesystem::path ui_shaders = resolve_imgui_shader_dir();
            if (ui_shaders.empty() || !ui_renderer.init(*device, ui_shaders)) {
                NF_LOG_ERROR(nf::LogCategory::Editor, "UiRenderer init failed (shader dir '{}')",
                             ui_shaders.string());
                return 1;
            }
            NF_LOG_INFO(nf::LogCategory::Editor, "UiRenderer ready (shader dir '{}')",
                        ui_shaders.string());
        }

        // Viewport offscreen target (never the swapchain directly).
        nf::editor::ViewportState vp_state{1280, 720};
        nf::editor::ViewportResources vp_res;
        if (!cfg.headless) {
            vp_state.width = window.width();
            vp_state.height = window.height();
        } else {
            vp_state.width = 640;
            vp_state.height = 360;
        }
        app.viewport() = vp_state;

        auto image_available = cfg.headless ? nullptr : device->create_semaphore();
        auto frame_fence = device->create_fence(true);
        auto cmd_view = device->create_command_buffer();
        auto cmd_main = cfg.headless ? nullptr : device->create_command_buffer();
        std::vector<std::unique_ptr<nf::rhi::Semaphore>> render_finished;
        // UI overlay pass (Load over the scene render, then present) plus one
        // framebuffer per swapchain image. The pass object outlives swapchain
        // recreates (same format); framebuffers do not.
        std::unique_ptr<nf::rhi::RenderPass> ui_pass;
        std::vector<std::unique_ptr<nf::rhi::Framebuffer>> ui_fbs;
        auto rebuild_ui_fbs = [&]() -> bool {
            ui_fbs.clear();
            if (cfg.headless || !swapchain || !ui_pass) {
                return true;
            }
            for (uint32_t i = 0; i < swapchain->image_count(); ++i) {
                nf::rhi::Texture* tex = swapchain->get_texture(i);
                if (tex == nullptr) {
                    return false;
                }
                const std::array<nf::rhi::Texture*, 1> cols{tex};
                auto fb = device->create_framebuffer(*ui_pass, std::span<nf::rhi::Texture* const>(cols),
                                                     nullptr);
                if (!fb) {
                    return false;
                }
                ui_fbs.push_back(std::move(fb));
            }
            return ui_fbs.size() == swapchain->image_count();
        };
        if (!cfg.headless && swapchain) {
            for (uint32_t i = 0; i < swapchain->image_count(); ++i) {
                render_finished.push_back(device->create_semaphore());
            }
            nf::rhi::ColorAttachment ui_ca{};
            ui_ca.format = swapchain->format();
            ui_ca.blend_enabled = true;
            ui_ca.src_color = nf::rhi::BlendFactor::SrcAlpha;
            ui_ca.dst_color = nf::rhi::BlendFactor::OneMinusSrcAlpha;
            const std::array<nf::rhi::ColorAttachment, 1> ui_atts{ui_ca};
            nf::rhi::RenderPassDesc ui_rpd{};
            ui_rpd.color_attachments = std::span<const nf::rhi::ColorAttachment>(ui_atts);
            ui_rpd.color_load = nf::rhi::RenderPassDesc::ColorLoad::Load;
            ui_rpd.present_source = true;
            ui_pass = device->create_render_pass(ui_rpd);
            if (!ui_pass || !rebuild_ui_fbs()) {
                NF_LOG_ERROR(nf::LogCategory::Editor, "UI overlay pass setup failed");
                return 1;
            }
        }
        nf::rhi::BufferDesc rb_desc{};
        rb_desc.size = static_cast<nf::usize>(vp_state.width) * vp_state.height * 4;
        rb_desc.usage = nf::rhi::BufferUsage::TransferDst;
        rb_desc.memory = nf::rhi::MemoryUsage::GPUToCPU;
        auto readback = device->create_buffer(rb_desc);
        uint32_t viewport_lit = 0;
        uint32_t viewport_red = 0;
        uint32_t viewport_blue = 0;
        uint32_t lit_single_cube = 0;
        bool force_measure = false;

        const bool automation = (cfg.max_frames != 0);
        bool auto_failed = false;
        // `detail` is reported only on failure, so the OK line keeps its
        // historical shape ("Automation: <what> OK") for existing greps while a
        // failure now says why instead of just that it happened.
        auto auto_check = [&](bool ok, const std::string& what, const std::string& detail = {}) {
            if (ok) {
                NF_LOG_INFO(nf::LogCategory::Editor, "Automation: {} OK", what);
                return;
            }
            auto_failed = true;
            if (detail.empty()) {
                NF_LOG_WARN(nf::LogCategory::Editor, "Automation: {} FAILED", what);
            } else {
                NF_LOG_WARN(nf::LogCategory::Editor, "Automation: {} FAILED — {}", what, detail);
            }
        };

        // Steps below are indexed by frame number, but imports commit on worker
        // threads and therefore finish on wall-clock time. Headless frames have
        // no sleep, so they run ~20x faster than vsync'd ones and a fixed frame
        // budget can expire before a worker finishes — the harness would pass
        // windowed and fail headless for reasons that have nothing to do with
        // the code under test. Pump until the real condition holds instead,
        // bounded so a genuine failure still fails.
        auto pump_until = [&](auto&& ready, int max_iters = 500) -> bool {
            for (int i = 0; i < max_iters; ++i) {
                if (ready()) return true;
                app.process_one_import();
                manager.update();
                runtime.update(0.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return ready();
        };

        nf::Clock clock;
#ifdef _WIN32
        bool prev_key[256] = {};
        auto key_edge = [&](int vk) -> bool {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool edge = down && !prev_key[vk & 0xFF];
            prev_key[vk & 0xFF] = down;
            return edge;
        };
        auto key_down = [&](int vk) -> bool { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
#else
        auto key_edge = [&](int) -> bool { return false; };
        auto key_down = [&](int) -> bool { return false; };
#endif

        std::string last_title;
        while (true) {
            if (!cfg.headless) {
                window.poll_events();
                if (window.should_close()) {
                    break;
                }
                if (swapchain &&
                    (window.width() != swapchain->width() || window.height() != swapchain->height())) {
                    device->wait_idle();
                    nf::rhi::SwapchainDesc sc{};
                    sc.width = window.width();
                    sc.height = window.height();
                    sc.format = nf::rhi::Format::B8G8R8A8_UNorm;
                    sc.present = nf::rhi::PresentMode::FIFO;
                    sc.image_count = 2;
                    auto fresh = device->create_swapchain(sc);
                    if (fresh) {
                        swapchain = std::move(fresh);
                        runtime.on_swapchain_resized(swapchain.get());
                        render_finished.clear();
                        for (uint32_t i = 0; i < swapchain->image_count(); ++i) {
                            render_finished.push_back(device->create_semaphore());
                        }
                        if (!rebuild_ui_fbs()) {
                            NF_LOG_ERROR(nf::LogCategory::Editor, "UI framebuffers lost on resize");
                            exit_code = 1;
                            break;
                        }
                    }
                    vp_state.width = window.width();
                    vp_state.height = window.height();
                    app.viewport() = vp_state;
                }
            }

            const float dt = static_cast<float>(clock.elapsed_seconds());
            clock.reset();
            app.tick(dt);

            // Hot reload: poll watched asset files every frame (a handful of
            // file stats — microseconds — and it closes the blind window
            // between watching a fresh import and the next poll; scenes are
            // never watched).
            app.poll_hot_reload();

            // Import queue: one job per frame (see ImportQueue: synchronous,
            // deterministic, no worker threads in v0.1).
            if (const nf::editor::ImportJob* finished = app.process_one_import()) {
                if (finished->state == nf::editor::ImportJob::State::Done) {
                    NF_LOG_INFO(nf::LogCategory::Editor, "Import done: {} -> {}", finished->src_absolute,
                                finished->dst_logical);
                } else {
                    NF_LOG_WARN(nf::LogCategory::Editor, "Import failed: {} ({})",
                                finished->src_absolute, finished->error);
                }
            }

            // Interactive shortcuts (windowed interactive runs).
            if (!cfg.headless && !automation) {
                if (key_edge('W')) {
                    app.set_gizmo_mode(nf::editor::GizmoMode::Translate);
                }
                if (key_edge('E')) {
                    app.set_gizmo_mode(nf::editor::GizmoMode::Rotate);
                }
                if (key_edge('R')) {
                    app.set_gizmo_mode(nf::editor::GizmoMode::Scale);
                }
                if (key_down(VK_CONTROL) && key_edge('Z')) {
                    std::string e;
                    if (!app.undo(e)) {
                        NF_LOG_WARN(nf::LogCategory::Editor, "Undo: {}", e);
                    }
                }
                if (key_down(VK_CONTROL) && key_edge('Y')) {
                    std::string e;
                    if (!app.redo(e)) {
                        NF_LOG_WARN(nf::LogCategory::Editor, "Redo: {}", e);
                    }
                }
                if (key_down(VK_CONTROL) && key_edge('S')) {
                    std::string e;
                    if (!app.save(e)) {
                        NF_LOG_ERROR(nf::LogCategory::Editor, "Save failed: {}", e);
                    } else {
                        NF_LOG_INFO(nf::LogCategory::Editor, "Save: OK {}", app.scene_path());
                    }
                }
                if (key_edge(VK_DELETE) && app.selection().has_selection()) {
                    app.request_delete(app.selection().primary());
                }
            }

            runtime.update(dt);

            // Viewport: offscreen Runtime render (never swapchain-direct).
            device->wait_idle();
            if (!nf::editor::ensure_viewport_target(*device, vp_state, vp_res)) {
                NF_LOG_ERROR(nf::LogCategory::Editor, "Viewport target creation failed");
                exit_code = 1;
                break;
            }
            cmd_view->reset();
            cmd_view->begin();
            runtime.render_offscreen(*vp_res.target, *cmd_view);
            // Periodic readback proof that the viewport holds a real scene
            // (plus on-demand automation measures via force_measure).
            const bool measure = (frame_count == 10) || (automation && frame_count % 30 == 0) ||
                                 force_measure;
            force_measure = false;
            if (measure && readback &&
                readback->size() >= static_cast<nf::usize>(vp_state.width) * vp_state.height * 4) {
                cmd_view->copy_texture_to_buffer(*vp_res.target, *readback, 0, 0, vp_state.width,
                                                 vp_state.height, 0);
            }
            cmd_view->end();
            {
                nf::rhi::SubmitInfo si{};
                si.signal_fence = frame_fence.get();
                frame_fence->reset();
                device->submit(*cmd_view, si);
                frame_fence->wait();
            }
            if (measure && readback) {
                if (const auto* px = static_cast<const uint8_t*>(readback->map())) {
                    uint32_t lit = 0;
                    uint32_t red = 0;
                    uint32_t blue = 0;
                    const size_t n =
                        static_cast<size_t>(vp_state.width) * static_cast<size_t>(vp_state.height);
                    for (size_t i = 0; i < n; ++i) {
                        if (px[i * 4] > 10 || px[i * 4 + 1] > 10 || px[i * 4 + 2] > 10) {
                            ++lit;
                        }
                        if (px[i * 4] > px[i * 4 + 1] + 40 && px[i * 4] > px[i * 4 + 2] + 40) {
                            ++red;
                        }
                        if (px[i * 4 + 2] > px[i * 4] + 40 && px[i * 4 + 2] > px[i * 4 + 1] + 40) {
                            ++blue;
                        }
                    }
                    readback->unmap();
                    viewport_lit = lit;
                    viewport_red = red;
                    viewport_blue = blue;
                    NF_LOG_INFO(nf::LogCategory::Editor, "Editor viewport: cube lit pixels = {}", lit);
                } else {
                    readback->unmap();
                }
            }

            // ImGui panels (windowed only): record first, submit with the frame.
            const ImDrawData* ui_draw = nullptr;
            nf::editor::UiIntents ui_in;
            bool have_ui_intents = false;
            if (!cfg.headless) {
                ImGui::GetIO().DeltaTime = (dt > 0.0f) ? dt : (1.0f / 60.0f);
                ui_renderer.set_viewport_texture(
                    (vp_res.view != nullptr) ? vp_res.view.get() : nullptr,
                    (vp_res.sampler != nullptr) ? vp_res.sampler.get() : nullptr);
                nf::editor::UiFrameStats fst{};
                fst.validation_on = device->validation_enabled();
                fst.validation_errors = nf::rhi::validation_error_count();
                fst.alive_objects = device->alive_objects();
                fst.viewport_lit = viewport_lit;
                ui_in = nf::editor::ui_frame(app, fst);
                have_ui_intents = true;
                nf::editor::ui_end_frame();
                ui_draw = ImGui::GetDrawData();
            }

            // Present the windowed scene render (same scene, swapchain path),
            // then the UI overlay Load pass (scene preserved, UI drawn over).
            bool ui_draw_failed = false;
            if (!cfg.headless && swapchain && cmd_main) {
                frame_fence->wait();
                frame_fence->reset();
                const uint32_t image_index = swapchain->acquire_next_image(*image_available);
                if (image_index != 0xFFFFFFFF && image_index < swapchain->image_count()) {
                    nf::rhi::Texture* target = swapchain->get_texture(image_index);
                    cmd_main->reset();
                    cmd_main->begin();
                    // The viewport target was written (or read back) on the
                    // viewport command buffer; transition from whatever layout
                    // it is actually in so the UI pass samples valid data.
                    // None = use the tracked layout (covers both the plain
                    // and the readback-measured frames).
                    cmd_main->barrier_texture(*vp_res.target, nf::rhi::ImageUsage::None,
                                              nf::rhi::ImageUsage::Sampled);
                    runtime.render(image_index, *cmd_main);
                    bool ui_ok = true;
                    if (target != nullptr && ui_pass && image_index < ui_fbs.size() && ui_fbs[image_index] &&
                        ui_draw != nullptr) {
                        // Make the scene writes visible to the Load that follows.
                        cmd_main->barrier_texture(*target, nf::rhi::ImageUsage::ColorAtt,
                                                  nf::rhi::ImageUsage::ColorAtt);
                        cmd_main->begin_render_pass(*ui_pass, *ui_fbs[image_index],
                                                    std::span<const nf::rhi::ClearValue>{});
                        ui_ok = ui_renderer.render(*cmd_main, *ui_pass, ui_draw, swapchain->width(),
                                                   swapchain->height());
                        cmd_main->end_render_pass();
                    } else if (ui_draw == nullptr) {
                        ui_ok = false;
                    }
                    if (!ui_ok) {
                        ui_draw_failed = true;
                        NF_LOG_ERROR(nf::LogCategory::Editor, "UI overlay render failed");
                    }
                    cmd_main->end();
                    nf::rhi::SubmitInfo si{};
                    const std::array<const nf::rhi::Semaphore*, 1> wait_sems{image_available.get()};
                    const std::array<nf::rhi::PipelineStage, 1> stages{
                        nf::rhi::PipelineStage::ColorAttachmentOutput};
                    const std::array<const nf::rhi::Semaphore*, 1> sig_sems{
                        render_finished[image_index].get()};
                    si.wait_semaphores = std::span<const nf::rhi::Semaphore* const>(wait_sems);
                    si.wait_stages = std::span<const nf::rhi::PipelineStage>(stages);
                    si.signal_semaphores = std::span<const nf::rhi::Semaphore* const>(sig_sems);
                    si.signal_fence = frame_fence.get();
                    device->submit(*cmd_main, si);
                    frame_fence->wait();
                    if (ui_draw_failed) {
                        auto_failed = true;
                    }
                    swapchain->present(image_index,
                                       std::span<const nf::rhi::Semaphore* const>(sig_sems));

                    // One-frame UI-over-scene proof (automation): composite the
                    // SAME draw data over a fresh scene render into a transient
                    // offscreen target (never touching the presented image) and
                    // require it to differ from the pure scene render. Same
                    // format on both sides, so the comparison is exact.
                    if (automation && frame_count == 10 && ui_draw != nullptr && !ui_draw_failed &&
                        readback) {
                        const uint32_t pw = swapchain->width();
                        const uint32_t ph = swapchain->height();
                        auto pf_cmd = device->create_command_buffer();
                        auto pf_fence = device->create_fence(false);
                        nf::rhi::TextureDesc ptd{};
                        ptd.width = pw;
                        ptd.height = ph;
                        ptd.format = nf::rhi::Format::R8G8B8A8_UNorm;
                        ptd.usage = nf::rhi::ImageUsage::ColorAtt | nf::rhi::ImageUsage::TransferSrc;
                        auto pf_target = device->create_texture(ptd);
                        nf::rhi::ColorAttachment pf_ca{};
                        pf_ca.format = nf::rhi::Format::R8G8B8A8_UNorm;
                        pf_ca.blend_enabled = true;
                        pf_ca.src_color = nf::rhi::BlendFactor::SrcAlpha;
                        pf_ca.dst_color = nf::rhi::BlendFactor::OneMinusSrcAlpha;
                        const std::array<nf::rhi::ColorAttachment, 1> pf_atts{pf_ca};
                        nf::rhi::RenderPassDesc pf_rpd{};
                        pf_rpd.color_attachments = std::span<const nf::rhi::ColorAttachment>(pf_atts);
                        pf_rpd.color_load = nf::rhi::RenderPassDesc::ColorLoad::Load;
                        pf_rpd.present_source = false;
                        auto pf_pass = device->create_render_pass(pf_rpd);
                        nf::rhi::BufferDesc pf_bd{};
                        pf_bd.size = static_cast<nf::usize>(pw) * ph * 4;
                        pf_bd.usage = nf::rhi::BufferUsage::TransferDst;
                        pf_bd.memory = nf::rhi::MemoryUsage::GPUToCPU;
                        auto pf_rb = device->create_buffer(pf_bd);
                        bool proof_ok = false;
                        size_t proof_diff = 0;
                        if (pf_cmd && pf_fence && pf_target && pf_pass && pf_rb) {
                            const std::array<nf::rhi::Texture*, 1> pf_cols{pf_target.get()};
                            auto pf_fb = device->create_framebuffer(
                                *pf_pass, std::span<nf::rhi::Texture* const>(pf_cols), nullptr);
                            if (pf_fb) {
                                pf_cmd->begin();
                                runtime.render_offscreen(*pf_target, *pf_cmd);
                                pf_cmd->barrier_texture(*pf_target, nf::rhi::ImageUsage::ColorAtt,
                                                        nf::rhi::ImageUsage::ColorAtt);
                                pf_cmd->begin_render_pass(*pf_pass, *pf_fb,
                                                          std::span<const nf::rhi::ClearValue>{});
                                proof_ok = ui_renderer.render(*pf_cmd, *pf_pass, ui_draw, pw, ph);
                                pf_cmd->end_render_pass();
                                pf_cmd->copy_texture_to_buffer(*pf_target, *pf_rb, 0, 0, pw, ph, 0);
                                pf_cmd->end();
                                device->submit(*pf_cmd,
                                               nf::rhi::SubmitInfo{.signal_fence = pf_fence.get()});
                                if (pf_fence->wait()) {
                                    const auto* sp =
                                        static_cast<const uint8_t*>(pf_rb->map());
                                    const auto* vp =
                                        (readback->size() >= static_cast<nf::usize>(pw) * ph * 4)
                                            ? static_cast<const uint8_t*>(readback->map())
                                            : nullptr;
                                    if (sp != nullptr && vp != nullptr) {
                                        const size_t n =
                                            static_cast<size_t>(pw) * static_cast<size_t>(ph);
                                        for (size_t i = 0; i < n; ++i) {
                                            const int dr = static_cast<int>(sp[i * 4]) -
                                                           static_cast<int>(vp[i * 4]);
                                            const int dg = static_cast<int>(sp[i * 4 + 1]) -
                                                           static_cast<int>(vp[i * 4 + 1]);
                                            const int db = static_cast<int>(sp[i * 4 + 2]) -
                                                           static_cast<int>(vp[i * 4 + 2]);
                                            if (dr > 4 || dr < -4 || dg > 4 || dg < -4 || db > 4 ||
                                                db < -4) {
                                                ++proof_diff;
                                            }
                                        }
                                    }
                                    if (sp != nullptr) {
                                        pf_rb->unmap();
                                    }
                                    if (vp != nullptr) {
                                        readback->unmap();
                                    }
                                }
                            }
                        }
                        device->wait_idle();
                        NF_LOG_INFO(nf::LogCategory::Editor,
                                    "Editor UI overlay: {} pixels differ from scene-only", proof_diff);
                        auto_check(proof_ok && proof_diff > 5000, "UI overlay drew over scene");
                    }
                }
            }

            // Intent processing for the recorded ImGui frame (windowed only).
            if (!cfg.headless && have_ui_intents) {
                if (ui_in.open_scene_dialog_confirm && !ui_in.open_scene_path.empty()) {
                    std::string e;
                    if (!app.open_scene(ui_in.open_scene_path, e)) {
                        NF_LOG_ERROR(nf::LogCategory::Editor, "Open scene failed: {}", e);
                    }
                }
                if (ui_in.save_as_confirm && !ui_in.save_as_path.empty()) {
                    std::string e;
                    if (!app.save_as(ui_in.save_as_path, e)) {
                        NF_LOG_ERROR(nf::LogCategory::Editor, "Save As failed: {}", e);
                    }
                }
                if (ui_in.viewport_drop && !ui_in.dropped_mesh_path.empty()) {
                    auto entries = app.browser_entries();
                    for (const auto& en : entries) {
                        if (en.logical_path == ui_in.dropped_mesh_path) {
                            std::string e;
                            if (!app.drop_mesh_asset(en, e)) {
                                NF_LOG_ERROR(nf::LogCategory::Editor, "Drop mesh failed: {}", e);
                            }
                            break;
                        }
                    }
                }
                if (ui_in.viewport_pick) {
                    const float aspect = (vp_state.height != 0)
                                             ? (static_cast<float>(vp_state.width) /
                                                static_cast<float>(vp_state.height))
                                             : 16.0f / 9.0f;
                    const nf::editor::ViewCamera vc =
                        view_camera_from_scene(runtime.scene(), aspect);
                    const nf::ecs::Entity hit = app.pick(vc, ui_in.pick_ndc_x, ui_in.pick_ndc_y);
                    if (hit.valid()) {
                        app.selection().set_single(hit);
                    }
                }
            }

            // Dirty flag in the window title.
            if (!cfg.headless) {
                const std::string title = "NOVAForge Editor — " + app.status().scene_label;
                if (title != last_title) {
                    window.set_title(title);
                    last_title = title;
                }
            }

            // --- Deterministic automation proof (only with --frames) --------
            if (automation) {
                const uint32_t f = frame_count;
                if (f == 2) {
                    auto hit = find_first_mesh(app.world());
                    auto_check(hit.has_value(), "Inspector target (mesh entity)");
                    if (hit.has_value()) {
                        app.selection().set_single(*hit);
                        if (const auto* t = app.world()->get<nf::scene::Transform>(*hit)) {
                            NF_LOG_INFO(nf::LogCategory::Editor,
                                        "Inspector: transform pos=({},{},{})", t->local_x, t->local_y,
                                        t->local_z);
                        }
                    }
                }
                if (f == 4) {
                    std::string e;
                    bool ok = false;
                    if (app.selection().has_selection()) {
                        const auto sel = app.selection().primary();
                        ok = app.rename_entity(sel, "EditedCube", e);
                        if (ok) {
                            nf::editor::TransformEdit te = nf::editor::read_transform(
                                *app.world(), sel);
                            te.px += 0.5f;
                            ok = app.set_transform(sel, te, e);
                        }
                    } else {
                        e = "no selection";
                    }
                    auto_check(ok, "Inspector transform+rename edit");
                }
                if (f == 6) {
                    std::string e;
                    const bool ok = app.undo(e);
                    auto_check(ok, "Undo");
                }
                if (f == 8) {
                    std::string e;
                    const bool ok = app.redo(e);
                    auto_check(ok, "Redo");
                }
                if (f == 15) {
                    std::string e;
                    auto_check(app.play(e), "Play (edit snapshot)");
                }
                if (f == 25) {
                    std::string e;
                    auto_check(app.stop(e), "Stop (edit world intact)");
                }
                if (f == 30) {
                    std::string e;
                    const bool ok = app.save_copy("cache://EditorAutomation.nfscene", e);
                    auto_check(ok, "Save copy");
                    if (ok) {
                        auto lr = nf::runtime::load_scene_from_vfs(vfs, "cache://EditorAutomation.nfscene");
                        auto_check(lr.success, "Reload saved copy");
                        if (lr.success) {
                            std::string diff;
                            auto_check(nf::editor::scenes_equal_structure(
                                           *runtime.scene(), *lr.scene, diff),
                                       "Save/Load structure round-trip");
                        }
                    }
                }
                if (f == 40) {
                    app.browser().filter_text = "cube";
                    const auto filtered = app.browser_entries();
                    auto_check(!filtered.empty(), "Asset filter 'cube'");
                    const nf::editor::AssetEntry* mesh = nullptr;
                    for (const auto& en : filtered) {
                        if (en.type == nf::assets::AssetType::Mesh) {
                            mesh = &en;
                            break;
                        }
                    }
                    std::string e;
                    bool ok = (mesh != nullptr) && app.drop_mesh_asset(*mesh, e);
                    auto_check(ok, "Drag mesh to viewport (create entity)");
                    if (ok) {
                        const size_t n = app.status().entity_count;
                        std::string ue;
                        const bool uok = app.undo(ue);
                        auto_check(uok && app.status().entity_count + 1 == n, "Undo drop");
                    }
                }
                if (f == 50) {
                    nf::editor::MaterialEdit red;
                    red.base_color[0] = 0.9f;
                    red.base_color[1] = 0.1f;
                    red.base_color[2] = 0.1f;
                    red.base_color[3] = 1.0f;
                    std::string e;
                    auto_check(app.set_material_params("content://Materials/Default", red, e),
                               "Material edit red");
                }
                if (f == 62) {
                    // f==60 measured after the red edit: require red dominance.
                    auto_check(viewport_red > 1000, "Material edit visible in viewport");
                    std::string e;
                    bool ok = app.save_material("content://Materials/Default",
                                                "cache://EditorMaterialProof.nfmat", e);
                    auto_check(ok, "Material save copy");
                    if (ok) {
                        auto rd = vfs.read_text("cache://EditorMaterialProof.nfmat");
                        bool red_ok = false;
                        if (rd.ok) {
                            nf::rendering::MaterialAsset reparsed;
                            std::string perr;
                            if (nf::rendering::MaterialAsset::load_from_text(rd.value, reparsed,
                                                                             perr)) {
                                red_ok = reparsed.params.base_color[0] > 0.8f;
                            }
                        }
                        auto_check(red_ok, "Material save reload");
                    }
                }
                if (f == 63) {
                    std::string e;
                    auto_check(app.undo(e), "Material edit undo");
                    nf::rendering::PBRMaterialParams back{};
                    runtime.material_params("content://Materials/Default", back);
                    auto_check(back.base_color[0] < 0.85f, "Material undo restores gray");
                }
                // --- Phase 5: import + hot reload + prefab proofs ---------------
                if (f == 70) {
                    // External red texture (outside content://) -> import.
                    const auto bmp = make_bmp_solid(8, 8, 255, 0, 0);
                    auto tr = vfs.resolve("content://Textures");
                    std::string e;
                    size_t job = 0;
                    bool ok = false;
                    if (tr.ok) {
                        const std::string abs =
                            (std::filesystem::temp_directory_path() / "nf_auto_tex.bmp").string();
                        {
                            std::ofstream o(abs, std::ios::binary | std::ios::trunc);
                            o.write(reinterpret_cast<const char*>(bmp.data()),
                                    static_cast<std::streamsize>(bmp.size()));
                            ok = static_cast<bool>(o);
                        }
                        if (ok) {
                            ok = app.import_file(abs, "content://Textures", true, job, e);
                        }
                    } else {
                        e = tr.error;
                    }
                    auto_check(ok, "Texture import submit");
                }
                if (f == 72) {
                    // The import commits on a worker thread, so wait for it to
                    // register instead of assuming it lands within a frame.
                    const bool imported = pump_until([&] {
                        return registry.find_by_path("content://Textures/nf_auto_tex.bmp") != nullptr;
                    });
                    std::string e;
                    const bool ok =
                        imported && app.set_material_albedo("content://Materials/Default",
                                                            "content://Textures/nf_auto_tex.bmp", e);
                    auto_check(ok, "Imported texture albedo assign",
                               imported ? e : std::string("import never registered the texture"));
                }
                if (f == 74) {
                    force_measure = true;
                }
                if (f == 75) {
                    auto_check(viewport_red > 1000, "Imported texture visible in viewport",
                               "red pixels = " + std::to_string(viewport_red));
                    // Overwrite the SOURCE on disk: hot reload must pick it up.
                    const auto bmp = make_bmp_solid(8, 8, 0, 0, 255);
                    auto wr = vfs.write_bytes("content://Textures/nf_auto_tex.bmp",
                                              std::span<const uint8_t>(bmp));
                    std::string e;
                    auto_check(wr.ok, "External texture overwrite", e);
                    if (wr.ok) {
                        // Detection is a poll, so allow a bounded window instead
                        // of a single attempt: a coarse filesystem timestamp
                        // must not be able to make this flaky.
                        size_t n = 0;
                        for (int i = 0; i < 60 && n == 0; ++i) {
                            n = app.poll_hot_reload();
                            if (n == 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            }
                        }
                        auto_check(n > 0, "Hot reload picked up texture",
                                   "watcher reported no change");
                    }
                }
                if (f == 76) {
                    force_measure = true;
                }
                if (f == 77) {
                    auto_check(viewport_blue > 1000, "Hot-reloaded texture visible",
                               "blue pixels = " + std::to_string(viewport_blue));
                    std::string e;
                    auto_check(app.set_material_albedo("content://Materials/Default", "", e),
                               "Albedo unbind restores scalar", e);
                }
                if (f == 84) {
                    // External procedural cube -> import as mesh (3.0 so the
                    // later shrink is unmistakable next to the 1.0 scene cube).
                    auto cube = nf::rendering::StaticMesh::create_cube(3.0f);
                    auto ma = nf::assets::MeshAsset::from_static_mesh(
                        *cube, nf::assets::AssetId::generate(), "content://Meshes/auto_cube.nfmesh");
                    std::vector<uint8_t> bytes;
                    ma->save_to_bytes(bytes);
                    const std::string abs =
                        (std::filesystem::temp_directory_path() / "nf_auto_cube.nfmesh").string();
                    bool ok = false;
                    std::string e;
                    {
                        std::ofstream o(abs, std::ios::binary | std::ios::trunc);
                        o.write(reinterpret_cast<const char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size()));
                        ok = static_cast<bool>(o);
                    }
                    if (ok) {
                        size_t job = 0;
                        ok = app.import_file(abs, "content://Meshes", true, job, e);
                    }
                    auto_check(ok, "Mesh import submit");
                }
                if (f == 86) {
                    // Single-cube baseline first (measured fresh at f==87).
                    lit_single_cube = viewport_lit;
                    // The mesh import commits on a worker thread. Wait for it to
                    // register before dropping, otherwise the drop races the
                    // import and every dependent prefab step fails behind it.
                    pump_until([&] {
                        return registry.find_by_path("content://Meshes/nf_auto_cube.nfmesh") != nullptr;
                    });
                    // Drop the imported mesh; pump below makes it resident.
                    std::string e;
                    bool ok = false;
                    if (const auto* meta = registry.find_by_path("content://Meshes/nf_auto_cube.nfmesh")) {
                        nf::editor::AssetEntry en;
                        en.logical_path = meta->logical_path;
                        en.type = nf::assets::AssetType::Mesh;
                        en.id = meta->id;
                        en.has_id = true;
                        ok = app.drop_mesh_asset(en, e);
                    } else {
                        e = "imported mesh missing from registry";
                    }
                    auto_check(ok, "Drop imported mesh", e);
                }
                if (f == 88) {
                    // The 3.0 cube must be GPU-resident before it can cover
                    // pixels: pump (bounded) instead of assuming one frame.
                    if (const auto* meta =
                            registry.find_by_path("content://Meshes/nf_auto_cube.nfmesh")) {
                        for (int i = 0; i < 120; ++i) {
                            manager.update();
                            runtime.update(0.0f);
                            auto h = manager.find(meta->id);
                            if (h && h->state == nf::assets::AssetState::Ready) {
                                break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }
                    }
                    force_measure = true; // fresh pixels incl. the big cube
                }
                if (f == 90) {
                    // Measured at f==89: 1.0 + 3.0 cubes vs the f==60
                    // single-cube baseline — unmistakable growth proves the
                    // imported mesh rendered (shrink direction is covered by
                    // the hotreload_mesh_rebuilds_live unit test).
                    auto_check(viewport_lit > lit_single_cube * 2,
                               "Hot-loaded mesh visibly larger",
                               "lit = " + std::to_string(viewport_lit) + ", baseline = " +
                                   std::to_string(lit_single_cube));
                }
                if (f == 92) {
                    // GPU picking, end-to-end through the editor's own entry
                    // point. By now a cube sits under the camera, so the
                    // viewport centre must resolve to an entity while an empty
                    // corner must not — that pair is what distinguishes real
                    // coverage from "always returns something".
                    const auto& vps = app.viewport();
                    const float aspect = (vps.height != 0)
                                             ? (static_cast<float>(vps.width) /
                                                static_cast<float>(vps.height))
                                             : 16.0f / 9.0f;
                    const nf::editor::ViewCamera vc =
                        view_camera_from_scene(runtime.scene(), aspect);
                    const nf::ecs::Entity centre = app.pick(vc, 0.0f, 0.0f);
                    auto_check(centre.valid(), "GPU pick hits the viewport centre",
                               "no entity resolved under the centre");
                    const nf::ecs::Entity corner = app.pick(vc, -0.97f, 0.97f);
                    auto_check(!corner.valid(), "GPU pick misses the empty corner",
                               "corner reported entity " + std::to_string(corner.id));
                    auto_check(runtime.gpu_picking_available(), "GPU picker is active",
                               "picker unavailable — CPU fallback in use");
                }
                if (f == 102) {
                    // Prefab round-trip on the dropped entity's subtree.
                    std::string e;
                    nf::ecs::Entity dropped = app.stack().last_target();
                    bool ok = dropped.valid() &&
                              app.create_prefab(dropped, "content://Prefabs/AutoProof.nfscene", e);
                    auto_check(ok, "Prefab create from entity",
                               dropped.valid() ? e
                                               : std::string("no dropped entity — the drop step failed"));
                    if (ok) {
                        ok = app.instantiate_prefab("content://Prefabs/AutoProof.nfscene",
                                                    nf::ecs::kInvalidEntity, e);
                    }
                    auto_check(ok, "Prefab instantiate", e);
                }
                if (f == 104) {
                    std::string e;
                    nf::ecs::Entity inst = app.stack().last_target();
                    bool ok = false;
                    if (inst.valid()) {
                        nf::editor::TransformEdit te =
                            nf::editor::read_transform(*app.world(), inst);
                        te.px = 33.0f;
                        if (app.set_transform(inst, te, e)) {
                            ok = app.revert_prefab(inst, e);
                        }
                    } else {
                        e = "no instance to diverge";
                    }
                    auto_check(ok, "Prefab revert restores template", e);
                }
                if (f == 110) {
                    // Tidy up automation artifacts (files + registry); the
                    // in-memory scene is discarded at exit anyway.
                    const char* paths[] = {"content://Textures/nf_auto_tex.bmp",
                                           "content://Meshes/nf_auto_cube.nfmesh",
                                           "content://Prefabs/AutoProof.nfscene"};
                    bool all_gone = true;
                    std::string leftover;
                    for (const char* p : paths) {
                        if (const auto* meta = registry.find_by_path(p)) {
                            const nf::assets::AssetId gone = meta->id;
                            manager.unload(gone);
                            registry.remove(gone);
                        }
                        if (auto rp = vfs.resolve(p); rp.ok) {
                            std::error_code ec;
                            std::filesystem::remove(rp.value, ec);
                        }
                        // Cooked mirror for mesh/texture imports.
                        const std::string cooked =
                            std::string("cache://") + (p + std::string("content://").size());
                        if (auto rc = vfs.resolve(cooked); rc.ok) {
                            std::error_code ec;
                            std::filesystem::remove(rc.value, ec);
                        }
                        auto still = vfs.exists(p);
                        if (still.ok && still.value) {
                            all_gone = false;
                            leftover += std::string(leftover.empty() ? "" : ", ") + p;
                        }
                    }
                    auto_check(all_gone, "Automation artifacts cleaned",
                               leftover.empty() ? std::string{}
                                                : "still present: " + leftover);
                }
            }

            ++frame_count;
            if (cfg.max_frames != 0 && frame_count >= cfg.max_frames) {
                break;
            }
            if (!cfg.headless) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        NF_LOG_INFO(nf::LogCategory::Editor, "Editor ran {} frames (entities={}, dirty={}, automation={})",
                    frame_count, app.status().entity_count, app.dirty() ? "yes" : "no",
                    automation ? (auto_failed ? "FAILED" : "OK") : "off");
        NF_LOG_INFO(nf::LogCategory::RHI, "Validation errors: {}", nf::rhi::validation_error_count());
        if (automation && auto_failed) {
            NF_LOG_ERROR(nf::LogCategory::Editor, "Automation proof failed");
            exit_code = 1;
        }

        device->wait_idle();
        ui_renderer.shutdown();
        ui_pass.reset();
        ui_fbs.clear();
        if (!cfg.headless) {
            nf::editor::ui_shutdown();
        }
        vp_res.reset();
        readback.reset();
        runtime.shutdown();
        manager.clear();
        device->wait_idle();
    }

    swapchain.reset();
    device->wait_idle();
    const uint32_t alive = device->alive_objects();
    NF_LOG_INFO(nf::LogCategory::RHI, "Alive RHI objects before shutdown: {}", alive);
    if (alive != 0) {
        NF_LOG_ERROR(nf::LogCategory::RHI, "Leaked RHI objects detected: {}", alive);
        exit_code = 1;
    }
    device->shutdown();
    if (cfg.validation && nf::rhi::validation_error_count() != 0) {
        NF_LOG_ERROR(nf::LogCategory::RHI, "Exiting with validation errors present");
        exit_code = 1;
    }
    if (!cfg.headless) {
        window.destroy();
    }
    nf::JobSystem::instance().shutdown();
    nf::platform_shutdown();
    NF_LOG_INFO(nf::LogCategory::Editor, "=== NOVAForge Editor exited (code {}) ===", exit_code);
    return exit_code;
}
