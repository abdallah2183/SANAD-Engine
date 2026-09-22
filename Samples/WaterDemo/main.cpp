// Samples/WaterDemo/main.cpp — the water surface (design doc 62) as a scene.
//
// What this sample shows:
//   - An animated Gerstner surface: `build_water_mesh` is re-run every frame at
//     the clock's time, replaced in the mesh library, and re-uploaded, so the
//     waves move on screen. The wave field is a pure function of (x, z, t), so
//     the frame at time t is the frame a replay at t draws — nothing here is
//     per-frame state.
//   - An injected shore: a depth that grows with z makes the south edge a beach
//     and the north open ocean, and surf appears as a band where the depth
//     crosses the break depth — the same `ShoreSampler` a heightfield coast
//     would supply.
//   - A buoy that rides the *CPU* field: it sits at `water_height` and tilts by
//     `water_normal`, not by the mesh it happens to be over. The visual vertex
//     is displaced horizontally as well (that is what makes the surface read as
//     water), so the buoy and the crest next to it are deliberately not the
//     same point — the physics query and the geometry disagree on purpose, and
//     this sample is the proof that the disagreement is a design and not a bug.
//
// The foam and the cheap reflection are baked into the mesh's `uv1` channel.
// Until the renderer samples `uv1` (requested in COORDINATION.md, R1 — the same
// channel the terrain's splat layers wait on) the surface renders as a solid
// displaced plane lit by the scene; the data is correct and tested, it is the
// paint that is pending. This sample does not touch the renderer or the
// shaders for that reason.

#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Core/Types.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Water/Water.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace nf::sample::water_demo {

namespace {

#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

/// The Renderer3D pipelines live with the Basic3D sample; this demo reuses the
/// same SPIR-V rather than carrying a shader set of its own, since it draws
/// nothing the Basic3D materials cannot shade.
std::filesystem::path shader_dir() {
    if (const std::string_view configured = NF_BASIC3D_SHADER_DIR;
        !configured.empty() && std::filesystem::exists(configured)) {
        return std::filesystem::path(configured);
    }
    std::error_code ec;
    const std::filesystem::path cur = std::filesystem::current_path(ec);
    for (const std::string_view cand :
         { "Samples/Basic3D/shaders", "../Samples/Basic3D/shaders" }) {
        if (std::filesystem::exists(cur / cand)) return cur / cand;
    }
    return {};
}

struct SampleConfig {
    u32 max_frames = 0;   // 0 = run until the window closes
    bool validation = false;
};

SampleConfig parse_args(const int argc, char** argv) {
    SampleConfig c;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--frames" && i + 1 < argc) c.max_frames = static_cast<u32>(std::atoi(argv[++i]));
        else if (a.rfind("--frames=", 0) == 0) c.max_frames = static_cast<u32>(std::atoi(a.substr(9).data()));
        else if (a == "--validation") c.validation = true;
        else if (a == "--help") {
            std::cout << "NOVAForge Water sample\n"
                         "  --frames N      Render N frames then exit (0 = until closed)\n"
                         "  --validation    Request Vulkan validation layers\n";
        }
    }
    return c;
}

/// The sea this demo is of: a main swell, a cross swell at an angle, and short
/// chop across them. Their sharpness Σ steepness·amplitude·wavenumber sums to
/// ~0.32, well under the 1.0 at which a Gerstner sum folds over itself, so the
/// surface stays a function of position while still throwing crests.
std::vector<water::Wave> ocean_waves() {
    return {
        { 0.60f, 18.0f, 1.60f, { 1.0f, 0.15f }, 0.55f }, // the swell: long, slow
        { 0.35f, 11.0f, 1.30f, { 0.35f, 0.94f }, 0.50f }, // crossing at ~70°
        { 0.18f,  5.5f, 1.00f, { -0.75f, 0.66f }, 0.50f }, // chop, against both
    };
}

/// The shore the south edge is: depth 0 at z = -40 (dry sand) deepening
/// northward, so the surf band sits near the beach and the far half is open
/// ocean. This is the injected seam — swap it for a heightfield coast and the
/// surf follows that coast instead, with nothing here changing.
water::ShoreSampler beach_shore(f32 patch_half, f32 deep_slope) {
    return [patch_half, deep_slope](f32, f32 z) {
        const f32 depth = (z + patch_half) * deep_slope;
        return depth > 0.0f ? depth : 0.0f;
    };
}

/// The tilt that puts an object's up axis along a surface normal, in the
/// Transform's XYZ-euler convention (R = Ry·Rx·Rz, applied to a column vector):
///   R·(0,1,0) = (sin y sin x, cos x, cos y sin x)
/// so rot_x = acos(n.y) and rot_y = atan2(n.x, n.z). Degrees, as the Transform
/// wants. `n.y` is clamped because a normalised vector can drift a hair past 1
/// and acos of that is NaN — a still surface would sink the buoy through the
/// floor instead of leaving it flat.
void align_to_normal(scene::Transform& t, const Vec3& n) {
    const f32 ny = std::clamp(n.y, -1.0f, 1.0f);
    const f32 tilt = std::acos(ny);
    const f32 yaw = std::atan2(n.x, n.z);
    t.rot_x = tilt * 180.0f / PI;
    t.rot_y = yaw * 180.0f / PI;
    t.dirty = true;
}

i32 run(const SampleConfig& cfg) {
    const std::filesystem::path sdir = shader_dir();
    if (sdir.empty()) { NF_LOG_FATAL(LogCategory::Core, "WaterDemo: shader dir not found"); return -1; }

    WindowDesc wdesc{};
    wdesc.width = 1280; wdesc.height = 720;
    wdesc.title = "NOVAForge — Water (Gerstner waves + shore surf)";
    wdesc.vsync = true;
    Window window;
    if (!window.create(wdesc)) { NF_LOG_FATAL(LogCategory::Platform, "Failed to create window"); return -1; }

    auto device = rhi::create_device();
    rhi::DeviceDesc ddesc{};
    ddesc.window_handle = window.native_handle();
    ddesc.enable_validation = cfg.validation;
    if (!device || !device->init(ddesc)) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to init device");
        window.destroy(); return -1;
    }

    rhi::SwapchainDesc sc_desc{};
    sc_desc.width = window.width(); sc_desc.height = window.height();
    sc_desc.format = rhi::Format::B8G8R8A8_UNorm;
    sc_desc.present = rhi::PresentMode::FIFO;
    sc_desc.image_count = 2;
    auto swapchain = device->create_swapchain(sc_desc);
    if (!swapchain) { NF_LOG_FATAL(LogCategory::RHI, "Failed to create swapchain"); device->shutdown(); window.destroy(); return -1; }

    rendering::Renderer3D renderer;
    if (!renderer.init(*device, sdir, window.width(), window.height())) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed to init Renderer3D");
        device->shutdown(); window.destroy(); return -1;
    }

    // --- The water ---------------------------------------------------------
    const std::vector<water::Wave> waves = ocean_waves();
    water::WaterOptions opts;
    opts.level = 0.0f;
    opts.size = 80.0f;
    opts.resolution = 80u;   // one vertex per world unit — smooth at this scale
    opts.time = 0.0f;
    opts.foam_steepness = 0.62f;
    opts.foam_softness = 0.18f;
    opts.shore_break_depth = 2.0f;
    opts.seed = 7u;
    const f32 deep_slope = 0.35f; // depth per world unit northward
    const water::ShoreSampler shore = beach_shore(opts.size * 0.5f, deep_slope);

    rendering::MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);

    water::WaterMeshStats stats{};
    std::unique_ptr<rendering::StaticMesh> water_mesh =
        water::build_water_mesh(waves, opts, shore, &stats);
    if (!water_mesh) { NF_LOG_FATAL(LogCategory::Core, "build_water_mesh returned null"); renderer.shutdown(); device->shutdown(); window.destroy(); return -1; }
    const rendering::StaticMeshHandle water_handle = meshes.add(std::move(water_mesh));

    // The buoy rides the CPU field, so it needs a body. A unit cube scaled down
    // makes a visible marker without importing an asset.
    auto buoy_body = rendering::StaticMesh::create_cube(1.0f);
    const rendering::StaticMeshHandle buoy_handle = meshes.add(std::move(buoy_body));

    if (!meshes.upload_all(*device)) {
        NF_LOG_FATAL(LogCategory::Core, "Failed to upload the water meshes");
        renderer.shutdown(); device->shutdown(); window.destroy(); return -1;
    }
    NF_LOG_INFO(LogCategory::Core,
                "WaterDemo: patch {}x{} ({} verts, {} indices), {} waves, surf where depth ~ {:.1}",
                opts.size, opts.size, stats.vertices, stats.indices, waves.size(),
                opts.shore_break_depth);
    NF_LOG_WARN(LogCategory::Core,
                "WaterDemo: foam and reflection are baked into uv1, which the renderer "
                "does not sample yet (COORDINATION.md R1); the surface is shaded as a "
                "solid plane in the meantime.");

    // --- Materials ---------------------------------------------------------
    rendering::PBRMaterialParams water_params{};
    water_params.base_color[0] = 0.08f;
    water_params.base_color[1] = 0.32f;
    water_params.base_color[2] = 0.58f;
    water_params.metallic = 0.0f;
    water_params.roughness = 0.22f;   // wet: tight specular, no diffuse scatter
    const rendering::MaterialHandle water_mat =
        renderer.materials().create_instance(*renderer.gbuffer_material(), water_params, "water_mat");

    rendering::PBRMaterialParams buoy_params{};
    buoy_params.base_color[0] = 0.85f;
    buoy_params.base_color[1] = 0.22f;
    buoy_params.base_color[2] = 0.14f;
    buoy_params.roughness = 0.55f;
    const rendering::MaterialHandle buoy_mat =
        renderer.materials().create_instance(*renderer.gbuffer_material(), buoy_params, "buoy_mat");

    // --- Scene -------------------------------------------------------------
    ecs::World world;
    const f32 buoy_x = 6.0f;
    const f32 buoy_z = -3.0f;
    const f32 buoy_half = 0.35f; // scaled cube half-extent
    ecs::Entity buoy_entity = ecs::kInvalidEntity;

    {
        // The surface itself. Not a shadow caster: the waves' own geometry would
        // self-shadow into noise, and nothing in this scene is tall enough to
        // cast onto water that reads.
        const ecs::Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        world.add<rendering::MeshComponent>(e,
            rendering::MeshComponent{water_handle, water_mat, false});
    }
    {
        const ecs::Entity e = world.create_entity();
        buoy_entity = e;
        world.add<scene::Transform>(e, scene::Transform{});
        auto* t = world.get<scene::Transform>(e);
        t->local_x = buoy_x;
        t->local_z = buoy_z;
        t->scale_x = buoy_half * 2.0f;
        t->scale_y = buoy_half * 2.0f;
        t->scale_z = buoy_half * 2.0f;
        world.add<rendering::MeshComponent>(e,
            rendering::MeshComponent{buoy_handle, buoy_mat, false});
    }

    // A low sun off the swim direction, so the faces of the swell catch
    // different light and the shape reads. Shadows off: this scene's only
    // shadow receiver would be the water itself.
    renderer.set_directional_light(rendering::DirectionalLight{
        rendering::Vec3{ -0.4f, -0.75f, 0.5f },
        rendering::Vec3{ 1.0f, 0.96f, 0.88f },
        1.1f,
        false
    });
    renderer.set_ambient(0.25f);

    // --- Frame loop --------------------------------------------------------
    auto image_available = device->create_semaphore();
    auto frame_fence = device->create_fence(true);
    auto cmd = device->create_command_buffer();
    std::vector<std::unique_ptr<rhi::Semaphore>> render_finished(swapchain->image_count());
    for (auto& s : render_finished) s = device->create_semaphore();

    // The mesh is rebuilt and re-uploaded every frame, and each upload logs an
    // INFO line. Quiet the logger past the startup report or the log is nothing
    // but upload lines; restored after the loop so the run report is visible.
    Logger::instance().set_min_level(LogLevel::Warn);

    Clock clock;
    u32 frame_count = 0u;
    bool swapchain_lost = false;
    while (!window.should_close() && !swapchain_lost) {
        window.poll_events();
        if (window.should_close()) break;

        // The previous frame's submission is done, so the mesh it drew can be
        // destroyed safely. Rebuild *before* the render extracts handles, and
        // before the acquire, so the new buffers exist when the pass binds them.
        frame_fence->wait();
        frame_fence->reset();

        opts.time = static_cast<f32>(clock.elapsed_seconds());
        std::unique_ptr<rendering::StaticMesh> next =
            water::build_water_mesh(waves, opts, shore, &stats);
        if (next) {
            if (!meshes.replace(water_handle, std::move(next))) {
                NF_LOG_ERROR(LogCategory::Core, "WaterDemo: mesh replace failed");
                break;
            }
            if (!meshes.upload_all(*device)) {
                NF_LOG_ERROR(LogCategory::Core, "WaterDemo: mesh re-upload failed");
                break;
            }
        }

        // Slow orbit, high enough to see both the surf line and the open swell.
        const f32 t = static_cast<f32>(clock.elapsed_seconds()) * 0.12f;
        rendering::Camera cam{};
        cam.position = { std::sin(t) * 26.0f, 9.0f, std::cos(t) * 26.0f };
        cam.target = { 0.0f, 0.0f, 0.0f };
        cam.aspect = static_cast<f32>(window.width()) / static_cast<f32>(window.height());
        cam.fov_y_rad = 55.0f * PI / 180.0f;
        cam.near_plane = 0.1f;
        cam.far_plane = 200.0f;
        rendering::update_camera(cam);

        // The buoy: height and tilt from the CPU field, never from the mesh.
        // The half-extent lifts it so its waterline sits in the surface rather
        // than its centre on it.
        if (auto* bt = world.get<scene::Transform>(buoy_entity)) {
            bt->local_y = water::water_height(buoy_x, buoy_z, waves, opts) + buoy_half;
            align_to_normal(*bt, water::water_normal(buoy_x, buoy_z, waves, opts));
        }

        scene::propagate_transforms(world);

        rendering::RenderWorld render_world;
        runtime::extract_render_objects(world, meshes, render_world);

        const u32 image_index = swapchain->acquire_next_image(*image_available);
        if (image_index == u32_max) { NF_LOG_WARN(LogCategory::RHI, "Swapchain out of date"); swapchain_lost = true; break; }
        rhi::Texture* backbuffer = swapchain->get_texture(image_index);
        if (!backbuffer) break;

        cmd->reset();
        cmd->begin();
        if (!renderer.render(*cmd, render_world, cam, *backbuffer, true)) {
            NF_LOG_ERROR(LogCategory::RHI, "Renderer3D::render failed");
            break;
        }
        cmd->end();

        const std::array<const rhi::Semaphore*, 1> wait_sems{ image_available.get() };
        const std::array<rhi::PipelineStage, 1> wait_stages{ rhi::PipelineStage::ColorAttachmentOutput };
        const std::array<const rhi::Semaphore*, 1> signal_sems{ render_finished[image_index].get() };
        rhi::SubmitInfo submit_info{};
        submit_info.wait_semaphores = std::span<const rhi::Semaphore* const>(wait_sems);
        submit_info.wait_stages = std::span<const rhi::PipelineStage>(wait_stages);
        submit_info.signal_semaphores = std::span<const rhi::Semaphore* const>(signal_sems);
        submit_info.signal_fence = frame_fence.get();
        device->submit(*cmd, submit_info);

        swapchain->present(image_index, std::span<const rhi::Semaphore* const>(signal_sems));

        ++frame_count;
        if (cfg.max_frames > 0u && frame_count >= cfg.max_frames) break;
    }

    device->wait_idle();
    Logger::instance().set_min_level(LogLevel::Info);
    renderer.shutdown();
    NF_LOG_INFO(LogCategory::Core, "WaterDemo rendered {} frames", frame_count);
    return static_cast<i32>(frame_count);
}

} // namespace

int run_sample(int argc, char** argv) {
    const SampleConfig cfg = parse_args(argc, argv);
    Logger& logger = Logger::instance();
    logger.add_sink(Logger::make_console_sink());
    logger.set_min_level(LogLevel::Info);
    NF_LOG_INFO(LogCategory::Core, "=== NOVAForge Engine — Water Sample (Gerstner + shore) ===");
    platform_init();
    const i32 frames = run(cfg);
    platform_shutdown();
    if (frames < 0) { NF_LOG_ERROR(LogCategory::Core, "=== Water Sample failed ==="); return 1; }
    NF_LOG_INFO(LogCategory::Core, "=== Water Sample exited cleanly ({} frames) ===", frames);
    return 0;
}

} // namespace nf::sample::water_demo

int main(int argc, char** argv) {
    return nf::sample::water_demo::run_sample(argc, argv);
}
