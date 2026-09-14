#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>

namespace nf::sample::basic3d {

#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

std::filesystem::path shader_dir() {
    if (auto configured = std::string_view(NF_BASIC3D_SHADER_DIR); !configured.empty() && std::filesystem::exists(configured)) {
        return std::filesystem::path(configured);
    }
    std::error_code ec;
    auto cur = std::filesystem::current_path(ec);
    for (auto cand : {std::string_view("Samples/Basic3D/shaders"), std::string_view("../Samples/Basic3D/shaders")}) {
        if (std::filesystem::exists(cur / cand)) return cur / cand;
    }
    return {};
}

struct SampleConfig { u32 max_frames = 0; bool validation = false; };

SampleConfig parse_args(int argc, char** argv) {
    SampleConfig c;
    for (int i=1; i<argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--frames" && i+1 < argc) c.max_frames = static_cast<u32>(std::atoi(argv[++i]));
        else if (a.rfind("--frames=", 0) == 0) c.max_frames = static_cast<u32>(std::atoi(a.substr(9).data()));
        else if (a == "--validation") c.validation = true;
    }
#ifdef _MSC_VER
    char* buf = nullptr; size_t sz = 0;
    if (_dupenv_s(&buf, &sz, "NF_BASIC3D_FRAMES") == 0 && buf) { c.max_frames = static_cast<u32>(std::atoi(buf)); std::free(buf); }
    char* buf2 = nullptr; size_t sz2 = 0;
    if (_dupenv_s(&buf2, &sz2, "NF_BASIC3D_VALIDATION") == 0 && buf2) { c.validation = true; std::free(buf2); }
#else
    if (auto e = std::getenv("NF_BASIC3D_FRAMES")) c.max_frames = static_cast<u32>(std::atoi(e));
    if (std::getenv("NF_BASIC3D_VALIDATION")) c.validation = true;
#endif
    return c;
}

int run(const SampleConfig& cfg) {
    auto sdir = shader_dir();
    if (sdir.empty()) { NF_LOG_FATAL(LogCategory::Core, "Basic3D: shader dir not found"); return -1; }

    WindowDesc wdesc{}; wdesc.width=1280; wdesc.height=720; wdesc.title="NOVAForge — Basic3D (Depth/GBuffer/Lighting)"; wdesc.vsync=true;
    Window window;
    if (!window.create(wdesc)) { NF_LOG_FATAL(LogCategory::Platform, "Failed to create window"); return -1; }

    auto device = rhi::create_device();
    rhi::DeviceDesc ddesc{}; ddesc.window_handle = window.native_handle(); ddesc.enable_validation = cfg.validation;
    if (!device || !device->init(ddesc)) { NF_LOG_FATAL(LogCategory::RHI, "Failed to init device"); window.destroy(); return -1; }

    rhi::SwapchainDesc sc_desc{}; sc_desc.width=window.width(); sc_desc.height=window.height(); sc_desc.format=rhi::Format::B8G8R8A8_UNorm; sc_desc.present=rhi::PresentMode::FIFO; sc_desc.image_count=2;
    auto swapchain = device->create_swapchain(sc_desc);
    if (!swapchain) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create swapchain"); device->shutdown(); window.destroy(); return -1; }

    rendering::Renderer3D renderer;
    if (!renderer.init(*device, sdir, window.width(), window.height())) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to init Renderer3D");
        device->shutdown(); window.destroy(); return -1;
    }

    rendering::MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);

    auto cube = rendering::StaticMesh::create_cube(1.0f);
    rendering::StaticMeshHandle cube_handle = meshes.add(std::move(cube));
    if (!meshes.upload_all(*device)) {
        NF_LOG_FATAL(LogCategory::Core, "Failed to upload meshes");
        renderer.shutdown(); device->shutdown(); window.destroy(); return -1;
    }

    rendering::PBRMaterialParams mat_params{};
    mat_params.base_color[0] = 0.8f;
    mat_params.base_color[1] = 0.8f;
    mat_params.base_color[2] = 0.8f;
    mat_params.roughness = 0.4f;
    rendering::MaterialHandle cube_mat = renderer.materials().create_instance(
        *renderer.gbuffer_material(), mat_params, "cube_mat");

    ecs::World game_world;

    for (int i=0; i<3; ++i) {
        ecs::Entity e = game_world.create_entity();
        game_world.add<scene::Transform>(e, scene::Transform{});
        auto* t = game_world.get<scene::Transform>(e);
        t->local_x = float(i*2 - 2); t->local_y = 0.0f; t->local_z = 0.0f;
        game_world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, cube_mat, true});
    }
    {
        // One distant entity that gets frustum culled
        ecs::Entity e = game_world.create_entity();
        game_world.add<scene::Transform>(e, scene::Transform{});
        game_world.get<scene::Transform>(e)->local_x = 100.0f;
        game_world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, cube_mat, true});
    }

    // Set lights
    renderer.set_directional_light(rendering::DirectionalLight{
        rendering::Vec3{-0.5f, -1.0f, -0.3f},
        rendering::Vec3{1.0f, 1.0f, 1.0f},
        1.0f,
        true
    });
    renderer.set_ambient(0.2f);

    auto image_available = device->create_semaphore();
    auto frame_fence = device->create_fence(true);
    auto cmd = device->create_command_buffer();
    std::vector<std::unique_ptr<rhi::Semaphore>> render_finished(swapchain->image_count());
    for (auto& s : render_finished) s = device->create_semaphore();

    NF_LOG_INFO(LogCategory::Core, "Basic3D: all assets created, entering render loop");

    Clock clock;
    u32 frame_count = 0;
    while (!window.should_close()) {
        window.poll_events();
        if (window.should_close()) break;

        frame_fence->wait();
        frame_fence->reset();

        float t = static_cast<float>(clock.elapsed_seconds()) * 0.3f;
        rendering::Camera cam{};
        cam.position = {std::sin(t) * 5.0f, 2.0f, std::cos(t) * 5.0f};
        cam.target = {0.0f, 0.0f, 0.0f};
        cam.aspect = float(window.width()) / float(window.height());
        cam.fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
        cam.near_plane = 0.1f;
        cam.far_plane = 100.0f;
        rendering::update_camera(cam);

        scene::propagate_transforms(game_world);

        rendering::RenderWorld render_world;
        nf::runtime::extract_render_objects(game_world, meshes, render_world);

        u32 image_index = swapchain->acquire_next_image(*image_available);
        if (image_index == u32_max) break;

        rhi::Texture* backbuffer = swapchain->get_texture(image_index);
        if (!backbuffer) break;

        cmd->reset();
        cmd->begin();
        if (!renderer.render(*cmd, render_world, cam, *backbuffer, true)) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D::render failed");
            break;
        }
        cmd->end();

        const std::array<const rhi::Semaphore*, 1> wait_sems{image_available.get()};
        const std::array<rhi::PipelineStage, 1> wait_stages{rhi::PipelineStage::ColorAttachmentOutput};
        const std::array<const rhi::Semaphore*, 1> signal_sems{render_finished[image_index].get()};
        rhi::SubmitInfo submit_info{};
        submit_info.wait_semaphores = std::span<const rhi::Semaphore* const>(wait_sems);
        submit_info.wait_stages = std::span<const rhi::PipelineStage>(wait_stages);
        submit_info.signal_semaphores = std::span<const rhi::Semaphore* const>(signal_sems);
        submit_info.signal_fence = frame_fence.get();
        device->submit(*cmd, submit_info);

        swapchain->present(image_index, std::span<const rhi::Semaphore* const>(signal_sems));

        ++frame_count;
        if (cfg.max_frames > 0 && frame_count >= cfg.max_frames) break;
    }

    device->wait_idle();
    renderer.shutdown();
    NF_LOG_INFO(LogCategory::Core, "Basic3D rendered {} frames", frame_count);
    return 0;
}

} // namespace nf::sample::basic3d

int main(int argc, char** argv) {
    auto cfg = nf::sample::basic3d::parse_args(argc, argv);
    nf::Logger::instance().add_sink(nf::Logger::make_console_sink());
    nf::Logger::instance().set_min_level(nf::LogLevel::Debug);
    nf::platform_init();
    int ret = nf::sample::basic3d::run(cfg);
    nf::platform_shutdown();
    return ret;
}
