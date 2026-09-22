#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/Terrain.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/TimeOfDay.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

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

struct SampleConfig {
    u32 max_frames = 0;
    bool validation = false;
    std::string heightmap_path; // --heightmap: imported raster for the terrain
    // Timelapse (design doc §64): 0 = off, and the scene keeps its static
    // light. > 0 is seconds per full 24h cycle, driven by TimeOfDay so the sun,
    // the sky, and the scene lighting are the one derived view of one clock.
    float day_length_seconds = 0.0f;
    float start_hour = 8.0f; // dawn: the cycle begins where the light is interesting
    bool help = false;      // --help: print usage and stop before the window
};

SampleConfig parse_args(int argc, char** argv) {
    SampleConfig c;
    for (int i=1; i<argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--help") {
            c.help = true;
            std::cout << "NOVAForge Basic3D (GBuffer + CSM + local shadows)\n"
                         "  --frames N          Render N frames then exit (0 = until closed)\n"
                         "  --validation        Request Vulkan validation layers\n"
                         "  --heightmap <path>  PGM heightmap instead of procedural noise\n"
                         "  --timelapse [secs]  Run a 24h day cycle; secs per day (bare = 120)\n"
                         "  --start-hour H      Where the timelapse opens, 0..24 (default 8)\n";
        }
        else if (a == "--frames" && i+1 < argc) c.max_frames = static_cast<u32>(std::atoi(argv[++i]));
        else if (a.rfind("--frames=", 0) == 0) c.max_frames = static_cast<u32>(std::atoi(a.substr(9).data()));
        else if (a == "--validation") c.validation = true;
        else if (a == "--heightmap" && i+1 < argc) c.heightmap_path = argv[++i];
        else if (a.rfind("--heightmap=", 0) == 0) c.heightmap_path = std::string(a.substr(12));
        // --timelapse with no value means a 120s day; a value is seconds per
        // 24h. A following arg is only consumed when it parses as a positive
        // number, so `--timelapse --frames 60` still means "120s day". --start-hour
        // sets where the cycle opens.
        else if (a == "--timelapse") {
            const float v = (i+1 < argc) ? static_cast<float>(std::atof(argv[i+1])) : 0.0f;
            if (v > 0.0f) { c.day_length_seconds = v; ++i; }
            else c.day_length_seconds = 120.0f;
        }
        else if (a.rfind("--timelapse=", 0) == 0) c.day_length_seconds = static_cast<float>(std::atof(a.substr(12).data()));
        else if (a == "--start-hour" && i+1 < argc) c.start_hour = static_cast<float>(std::atof(argv[++i]));
        else if (a.rfind("--start-hour=", 0) == 0) c.start_hour = static_cast<float>(std::atof(a.substr(12).data()));
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

    WindowDesc wdesc{}; wdesc.width=1280; wdesc.height=720; wdesc.title="NOVAForge — Basic3D (GBuffer + CSM + Local Shadows)"; wdesc.vsync=true;
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
    // A floor the local lights can throw a shadow onto: 24 units square, top at
    // y = -0.55 so the cubes (bottoms at -0.5) sit just above it with no
    // z-fighting along the contact line.
    auto floor = rendering::StaticMesh::create_cube(1.0f);
    rendering::StaticMeshHandle floor_handle = meshes.add(std::move(floor));

    // Layered terrain (design doc 59): three height bands — sand at the foot,
    // grass on the slopes, rock above the treeline — blended through the uv1
    // channel the mesh layout already carries. The bands are only decoration
    // until the lighting pass reads uv1, but the terrain is also the ground the
    // scene stands on: the cubes below sample terrain_height to plant
    // themselves on it instead of floating above or sinking through.
    rendering::TerrainOptions terrain_opt;
    terrain_opt.resolution = 97;   // 96 quads per side
    terrain_opt.size = 48.0f;      // well past the 24-unit floor underneath
    terrain_opt.height_scale = 2.0f;
    terrain_opt.noise_frequency = 0.06f;
    terrain_opt.seed = 17;
    std::vector<rendering::TerrainLayer> terrain_layers(3u);
    terrain_layers[0u].texture_slot = 0u; // sand: the low ground
    terrain_layers[0u].min_height = -1.0f;
    terrain_layers[0u].max_height = 0.5f;
    terrain_layers[1u].texture_slot = 1u; // grass: the middle band
    terrain_layers[1u].min_height = 0.5f;
    terrain_layers[1u].max_height = 1.4f;
    terrain_layers[2u].texture_slot = 2u; // rock: the peaks
    terrain_layers[2u].min_height = 1.4f;
    terrain_layers[2u].max_height = 3.0f;

    // The heights can come from an imported raster instead of the noise (design
    // doc 59: heightmaps) — `--heightmap some.png` swaps the source wholesale.
    // Everything downstream reads the same field through terrain_height — the
    // splat bands, the cubes planted on the ground, the camera that rides it —
    // so the switch is one property rather than a second pipeline. The field
    // lives in this scope on purpose: terrain_height is called per frame for
    // the camera, so the pointer has to outlive the render loop.
    rendering::HeightField height_field;
    if (!cfg.heightmap_path.empty()) {
        std::string heightmap_err;
        height_field = rendering::build_heightfield_from_image(cfg.heightmap_path, heightmap_err);
        if (height_field.ok()) {
            terrain_opt.heightmap = &height_field;
            NF_LOG_INFO(LogCategory::Core, "Basic3D: terrain source = heightmap '{}' ({}x{})",
                        cfg.heightmap_path, height_field.width, height_field.height);
        } else {
            // A demo, not a tool: an unreadable file falls back to the noise
            // terrain rather than ending the run, and the reason is logged so
            // the silence is not mistaken for a flat heightmap.
            NF_LOG_ERROR(LogCategory::Core, "Basic3D: heightmap '{}' unusable ({}) — using procedural noise",
                         cfg.heightmap_path, heightmap_err);
        }
    } else {
        NF_LOG_INFO(LogCategory::Core, "Basic3D: terrain source = procedural noise (seed {})", terrain_opt.seed);
    }
    NF_LOG_INFO(LogCategory::Core, "Basic3D: terrain height at origin = {:.3f}",
                rendering::terrain_height(0.0f, 0.0f, terrain_opt));

    auto terrain = rendering::build_terrain_mesh(terrain_opt, terrain_layers, "Basic3DTerrain");
    rendering::StaticMeshHandle terrain_handle = meshes.add(std::move(terrain));

    // Small emissive bulb riding the point light's position, so the moving
    // light is visible as a thing in the scene and not only as the bright side
    // of a cube.
    auto bulb = rendering::StaticMesh::create_sphere(0.18f);
    rendering::StaticMeshHandle bulb_handle = meshes.add(std::move(bulb));
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

    rendering::PBRMaterialParams floor_params{};
    floor_params.base_color[0] = 0.32f;
    floor_params.base_color[1] = 0.35f;
    floor_params.base_color[2] = 0.4f;
    floor_params.roughness = 0.9f;
    rendering::MaterialHandle floor_mat = renderer.materials().create_instance(
        *renderer.gbuffer_material(), floor_params, "floor_mat");

    constexpr rendering::Vec3 k_point_color{1.0f, 0.65f, 0.32f};
    rendering::PBRMaterialParams bulb_params{};
    bulb_params.base_color[0] = k_point_color.x;
    bulb_params.base_color[1] = k_point_color.y;
    bulb_params.base_color[2] = k_point_color.z;
    bulb_params.emission[0] = k_point_color.x;
    bulb_params.emission[1] = k_point_color.y;
    bulb_params.emission[2] = k_point_color.z;
    bulb_params.emission_strength = 2.0f;
    rendering::MaterialHandle bulb_mat = renderer.materials().create_instance(
        *renderer.gbuffer_material(), bulb_params, "bulb_mat");

    // Alpha below 1 is the whole selection rule for the forward pass, so this
    // is the only thing that makes the pane transparent. Roughness low and
    // metallic zero reads as glass; the emission stays off so the pane is lit
    // by the scene rather than glowing.
    rendering::PBRMaterialParams glass_params{};
    glass_params.base_color[0] = 0.55f;
    glass_params.base_color[1] = 0.75f;
    glass_params.base_color[2] = 0.95f;
    glass_params.base_color[3] = 0.45f;
    glass_params.metallic = 0.0f;
    glass_params.roughness = 0.08f;
    rendering::MaterialHandle glass_mat = renderer.materials().create_instance(
        *renderer.gbuffer_material(), glass_params, "glass_mat");

    ecs::World game_world;

    // Filled in once the lights exist; updated every frame below.
    ecs::Entity bulb_entity = ecs::kInvalidEntity;
    u32 point_light_index = u32_max;

    for (int i=0; i<3; ++i) {
        ecs::Entity e = game_world.create_entity();
        game_world.add<scene::Transform>(e, scene::Transform{});
        auto* t = game_world.get<scene::Transform>(e);
        t->local_x = float(i*2 - 2); t->local_z = 0.0f;
        // Plant on the terrain: its own half-extent above the ground the noise
        // put there, so a cube on a rise stands on the rise rather than
        // hovering at the flat-floor height the sample used before.
        t->local_y = rendering::terrain_height(t->local_x, t->local_z, terrain_opt) + 0.5f;
        game_world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, cube_mat, true});
    }
    {
        // One distant entity that gets frustum culled
        ecs::Entity e = game_world.create_entity();
        game_world.add<scene::Transform>(e, scene::Transform{});
        game_world.get<scene::Transform>(e)->local_x = 100.0f;
        game_world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, cube_mat, true});
    }

    {
        // The flat floor the local shadows land on, kept under the terrain so
        // the shadow demo does not depend on the noise field: the terrain is
        // decorative above it, and its troughs do not punch holes in the pool
        // the spot light windows.
        ecs::Entity e = game_world.create_entity();
        game_world.add<scene::Transform>(e, scene::Transform{});
        auto* t = game_world.get<scene::Transform>(e);
        t->local_x = 0.0f; t->local_y = -1.55f; t->local_z = 0.0f;
        t->scale_x = 24.0f; t->scale_y = 2.0f; t->scale_z = 24.0f;
        game_world.add<rendering::MeshComponent>(e, rendering::MeshComponent{floor_handle, floor_mat, true});
    }
    {
        // The terrain itself: drawn over the floor, carrying its layer splat in
        // uv1. It is not a shadow caster (the resolution makes the depth pass
        // expensive for a ground the cubes' shadows do not need), and it is not
        // the receiver the spot light windows either — that is the floor.
        ecs::Entity e = game_world.create_entity();
        game_world.add<scene::Transform>(e, scene::Transform{});
        game_world.add<rendering::MeshComponent>(e,
            rendering::MeshComponent{terrain_handle, floor_mat, true});
    }
    {
        // The emissive bulb. Not a shadow caster: it rides the light, and a
        // shadow of the light source itself under the light is noise.
        bulb_entity = game_world.create_entity();
        game_world.add<scene::Transform>(bulb_entity, scene::Transform{});
        game_world.add<rendering::MeshComponent>(bulb_entity,
            rendering::MeshComponent{bulb_handle, bulb_mat, false});
    }
    {
        // A transparent pane the deferred path cannot draw: the gbuffer holds
        // one surface per pixel, so this cube never reaches it. The renderer
        // routes it to the forward pass over the lit image instead — an alpha
        // below 1 is the entire selection rule, and the pane is still lit and
        // shadowed by the same BRDF the gbuffer surfaces were.
        //
        // It sits behind the middle cube so the orbiting camera sees it both
        // alone and over an opaque surface, and so the orbiting point light
        // sweeps through it while the spot's cone falls across it — the case
        // the forward shader has to survive: a shadow lookup through a
        // projector while the surface behind it is already lit.
        ecs::Entity e = game_world.create_entity();
        game_world.add<scene::Transform>(e, scene::Transform{});
        auto* t = game_world.get<scene::Transform>(e);
        t->local_x = 0.0f; t->local_y = 0.9f; t->local_z = -2.0f;
        t->scale_x = 1.6f; t->scale_y = 1.6f; t->scale_z = 1.6f;
        game_world.add<rendering::MeshComponent>(e,
            rendering::MeshComponent{cube_handle, glass_mat, true});
    }

    // --- Time of day (design doc §64) -------------------------------------
    // Off by default: the static light below is what the automation pixel
    // checks measure, and a moving sun would repaint them. With a day length,
    // the cycle drives the sun, the sky palette, and the ambient in one place —
    // the renderer's directional light and sky are the derived view of this
    // clock, so the shadows and the sky cannot disagree about where the sun is.
    std::optional<rendering::TimeOfDay> tod;
    if (cfg.day_length_seconds > 0.0f) {
        tod.emplace();
        tod->set_time_hours(cfg.start_hour);
        tod->set_day_length_seconds(cfg.day_length_seconds);
        NF_LOG_INFO(LogCategory::Core,
                    "Basic3D: timelapse on — 24h over {:.0f}s starting at {:.1f}:00",
                    cfg.day_length_seconds, cfg.start_hour);
    }

    // Set lights. The static sun is the default look; the cycle replaces it
    // per frame below when timelapse is on.
    renderer.set_directional_light(rendering::DirectionalLight{
        rendering::Vec3{-0.5f, -1.0f, -0.3f},
        rendering::Vec3{1.0f, 1.0f, 1.0f},
        1.0f,
        true
    });
    renderer.set_ambient(0.2f);

    // Phase 21: local lights that cast. The point light orbits the scene so
    // its shadow visibly sweeps the floor; the spot is fixed overhead so its
    // cone and the cubes' shadows inside it stay put to compare against. Both
    // opt into shadows — the default is off, because a point light costs six
    // depth renders.
    //
    // Intensity is large because lighting.frag applies a 1/dist^2 falloff:
    // ~30 at ~4 units works out to roughly the radiant contribution the
    // unit-intensity directional light gives the same surface.
    rendering::PointLight point;
    point.position = {3.5f, 2.5f, 0.0f};
    point.color = k_point_color;
    point.intensity = 30.0f;
    point.radius = 12.0f;
    point.shadows_enabled = true;
    point.shadow_distance = 10.0f;
    point_light_index = renderer.add_point_light(point);

    rendering::SpotLight spot;
    spot.position = {0.0f, 5.0f, 0.0f};
    spot.direction = {0.0f, -1.0f, 0.0f};
    spot.color = {0.45f, 0.6f, 1.0f};
    spot.intensity = 40.0f;
    spot.inner_angle_rad = 0.35f;
    spot.outer_angle_rad = 0.6f;
    // The light is 5.55 above the floor; a reach of 8 windows the pool just
    // past the contact and stops the projector from spending its depth range
    // on empty floor beyond it, which is what blurred the cubes' shadows
    // before the reach was a per-light property.
    spot.range = 8.0f;
    spot.shadows_enabled = true;
    renderer.add_spot_light(spot);

    auto image_available = device->create_semaphore();
    auto frame_fence = device->create_fence(true);
    auto cmd = device->create_command_buffer();
    std::vector<std::unique_ptr<rhi::Semaphore>> render_finished(swapchain->image_count());
    for (auto& s : render_finished) s = device->create_semaphore();

    NF_LOG_INFO(LogCategory::Core, "Basic3D: all assets created, entering render loop");

    Clock clock;
    float prev_seconds = static_cast<float>(clock.elapsed_seconds());
    u32 frame_count = 0;
    while (!window.should_close()) {
        window.poll_events();
        if (window.should_close()) break;

        frame_fence->wait();
        frame_fence->reset();

        const float now_seconds = static_cast<float>(clock.elapsed_seconds());
        if (tod) {
            // The clock is the only input; the sun, the sky, and the ambient
            // are derived from it, so the same wall time replays the same day.
            tod->advance(now_seconds - prev_seconds);
            renderer.set_directional_light(tod->make_light());
            renderer.set_sky(tod->make_sky());
            // Nights read as nights: the ambient drops with the sun so the
            // moonlit side of a cube is the only side carrying detail.
            renderer.set_ambient(tod->is_day() ? 0.2f : 0.06f);
        }
        prev_seconds = now_seconds;

        float t = now_seconds * 0.3f;
        rendering::Camera cam{};
        cam.position = {std::sin(t) * 5.0f, 2.0f, std::cos(t) * 5.0f};
        // The terrain rolls up to ~2 units, so an orbit at a fixed 2.0 would
        // bury the camera in a hill on the far side. Ride the ground the same
        // way the cubes do, plus the height the framing wants.
        const float ground_y = rendering::terrain_height(cam.position.x, cam.position.z, terrain_opt);
        cam.position.y = std::max(2.0f, ground_y + 1.5f);
        cam.target = {0.0f, 0.0f, 0.0f};
        cam.aspect = float(window.width()) / float(window.height());
        cam.fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
        cam.near_plane = 0.1f;
        cam.far_plane = 100.0f;
        rendering::update_camera(cam);

        // The point light orbits at a third of the camera's angular speed, so
        // the two motions read independently. The renderer hands back the index
        // at add time; the light list is per-frame persistent state, so this
        // mutates the same light rather than appending a new one each frame.
        const rendering::Vec3 point_pos{
            std::sin(t * 0.33f) * 3.5f, 2.5f, std::cos(t * 0.33f) * 3.5f};
        renderer.point_light(point_light_index).position = point_pos;
        if (auto* bt = game_world.get<scene::Transform>(bulb_entity)) {
            bt->local_x = point_pos.x;
            bt->local_y = point_pos.y;
            bt->local_z = point_pos.z;
            bt->dirty = true;
        }

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
    if (cfg.help) return 0; // usage only — do not open a window the user did not ask for
    nf::Logger::instance().add_sink(nf::Logger::make_console_sink());
    nf::Logger::instance().set_min_level(nf::LogLevel::Debug);
    nf::platform_init();
    int ret = nf::sample::basic3d::run(cfg);
    nf::platform_shutdown();
    return ret;
}
