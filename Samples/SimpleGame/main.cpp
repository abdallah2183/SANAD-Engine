// Samples/SimpleGame/main.cpp — Cube Collector: simple playable game using NOVAForge engine
// Uses: NFCore, NFPlatform (Window + InputSystem), NFRHI, NFRendering, NFEcs, NFScene
// Controls: WASD / Arrows = move, R = restart after game over, ESC = quit
// Goal: collect golden cubes, avoid the red enemy. 5 coins = next level (enemy faster).

#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/Platform/InputSystem.hpp>
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
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace nf::sample::simplegame {

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

struct GameState {
    int score = 0;
    int lives = 3;
    int level = 1;
    int coins_to_next = 5;
    bool game_over = false;
    bool won_flash = false;
    float invincible_timer = 2.0f; // spawn protection
    float time = 0.0f;
    float enemy_phase = 2.6f; // start enemy away from the player
};

float rand_range(std::mt19937& rng, float lo, float hi) {
    std::uniform_real_distribution<float> d(lo, hi);
    return d(rng);
}

int run() {
    auto sdir = shader_dir();
    if (sdir.empty()) { NF_LOG_FATAL(LogCategory::Core, "SimpleGame: shader dir not found"); return -1; }

    platform_init();
    InputSystem::instance().init();

    WindowDesc wdesc{};
    wdesc.width = 1280; wdesc.height = 720;
    wdesc.title = "NOVAForge — Cube Collector | WASD/Arrows move, collect gold, avoid red!";
    wdesc.vsync = true;
    Window window;
    if (!window.create(wdesc)) { NF_LOG_FATAL(LogCategory::Platform, "Failed to create window"); return -1; }

    auto device = rhi::create_device();
    rhi::DeviceDesc ddesc{}; ddesc.window_handle = window.native_handle(); ddesc.enable_validation = false;
    if (!device || !device->init(ddesc)) { NF_LOG_FATAL(LogCategory::RHI, "Failed to init device"); window.destroy(); return -1; }

    rhi::SwapchainDesc sc_desc{};
    sc_desc.width = window.width(); sc_desc.height = window.height();
    sc_desc.format = rhi::Format::B8G8R8A8_UNorm;
    sc_desc.present = rhi::PresentMode::FIFO; sc_desc.image_count = 2;
    auto swapchain = device->create_swapchain(sc_desc);
    if (!swapchain) { NF_LOG_FATAL(LogCategory::RHI, "Failed swapchain"); device->shutdown(); window.destroy(); return -1; }

    rendering::Renderer3D renderer;
    if (!renderer.init(*device, sdir, window.width(), window.height())) {
        NF_LOG_FATAL(LogCategory::RHI, "Failed Renderer3D");
        device->shutdown(); window.destroy(); return -1;
    }

    rendering::MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);
    auto cube = rendering::StaticMesh::create_cube(1.0f);
    rendering::StaticMeshHandle cube_handle = meshes.add(std::move(cube));
    if (!meshes.upload_all(*device)) {
        NF_LOG_FATAL(LogCategory::Core, "mesh upload failed");
        renderer.shutdown(); device->shutdown(); window.destroy(); return -1;
    }

    // Materials: player green, coin gold, enemy red, ground gray
    auto make_mat = [&](float r, float g, float b, const char* name) {
        rendering::PBRMaterialParams p{};
        p.base_color[0] = r; p.base_color[1] = g; p.base_color[2] = b; p.base_color[3] = 1.0f;
        p.roughness = 0.45f;
        return renderer.materials().create_instance(*renderer.gbuffer_material(), p, name);
    };
    rendering::MaterialHandle player_mat = make_mat(0.2f, 0.8f, 0.3f, "player");
    rendering::MaterialHandle coin_mat = make_mat(1.0f, 0.8f, 0.15f, "coin");
    rendering::MaterialHandle enemy_mat = make_mat(0.9f, 0.15f, 0.15f, "enemy");
    rendering::MaterialHandle ground_mat = make_mat(0.25f, 0.27f, 0.32f, "ground");

    ecs::World world;

    // Ground
    {
        ecs::Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        auto* t = world.get<scene::Transform>(e);
        t->local_x = 0; t->local_y = -0.65f; t->local_z = 0;
        t->scale_x = 22.0f; t->scale_y = 0.3f; t->scale_z = 22.0f;
        world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, ground_mat, true});
    }

    // Player
    ecs::Entity player = world.create_entity();
    world.add<scene::Transform>(player, scene::Transform{});
    world.get<scene::Transform>(player)->local_y = 0.5f;
    world.add<rendering::MeshComponent>(player, rendering::MeshComponent{cube_handle, player_mat, true});

    // Coins (5)
    constexpr int kCoins = 5;
    std::vector<ecs::Entity> coins;
    std::mt19937 rng(12345);
    for (int i = 0; i < kCoins; ++i) {
        ecs::Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        auto* t = world.get<scene::Transform>(e);
        t->local_x = rand_range(rng, -8.0f, 8.0f);
        t->local_y = 0.5f;
        t->local_z = rand_range(rng, -8.0f, 8.0f);
        t->scale_x = t->scale_y = t->scale_z = 0.5f;
        world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, coin_mat, true});
        coins.push_back(e);
    }

    // Enemy
    ecs::Entity enemy = world.create_entity();
    world.add<scene::Transform>(enemy, scene::Transform{});
    world.get<scene::Transform>(enemy)->local_y = 0.5f;
    world.get<scene::Transform>(enemy)->local_z = -3.0f;
    world.add<rendering::MeshComponent>(enemy, rendering::MeshComponent{cube_handle, enemy_mat, true});

    renderer.set_directional_light(rendering::DirectionalLight{
        rendering::Vec3{-0.5f, -1.0f, -0.3f}, rendering::Vec3{1.0f, 1.0f, 1.0f}, 1.0f, true});
    renderer.set_ambient(0.35f);

    auto image_available = device->create_semaphore();
    auto frame_fence = device->create_fence(true);
    auto cmd = device->create_command_buffer();
    std::vector<std::unique_ptr<rhi::Semaphore>> render_finished(swapchain->image_count());
    for (auto& s : render_finished) s = device->create_semaphore();

    GameState gs;
    Timer timer; timer.start();
    Clock title_clock;
    const float kArena = 9.0f;

    auto reset_game = [&]() {
        gs.score = 0; gs.lives = 3; gs.level = 1; gs.coins_to_next = 5;
        gs.game_over = false; gs.invincible_timer = 2.0f; gs.enemy_phase = 2.6f;
        auto* pt = world.get<scene::Transform>(player);
        pt->local_x = 0; pt->local_z = 0; pt->local_y = 0.5f;
        for (auto c : coins) {
            auto* t = world.get<scene::Transform>(c);
            t->local_x = rand_range(rng, -8.0f, 8.0f);
            t->local_z = rand_range(rng, -8.0f, 8.0f);
        }
    };

    NF_LOG_INFO(LogCategory::Core, "SimpleGame: entering loop (WASD to move)");

    while (!window.should_close()) {
        float dt = timer.tick();
        if (dt > 0.05f) dt = 0.05f; // clamp long frames
        gs.time += dt;

        InputSystem::instance().begin_frame();
        window.poll_events();
        if (window.should_close()) break;

        auto& input = InputSystem::instance();
        if (input.is_key_down(KeyCode::Escape)) break;
        if (input.is_key_pressed(KeyCode::R) && gs.game_over) reset_game();

        if (!gs.game_over) {
            // --- Player movement ---
            float mx = 0, mz = 0;
            if (input.is_key_down(KeyCode::A) || input.is_key_down(KeyCode::Left)) mx -= 1.0f;
            if (input.is_key_down(KeyCode::D) || input.is_key_down(KeyCode::Right)) mx += 1.0f;
            if (input.is_key_down(KeyCode::W) || input.is_key_down(KeyCode::Up)) mz -= 1.0f;
            if (input.is_key_down(KeyCode::S) || input.is_key_down(KeyCode::Down)) mz += 1.0f;
            float len = std::sqrt(mx * mx + mz * mz);
            if (len > 0.01f) { mx /= len; mz /= len; }
            const float speed = 6.0f;
            auto* pt = world.get<scene::Transform>(player);
            pt->local_x += mx * speed * dt;
            pt->local_z += mz * speed * dt;
            if (pt->local_x < -kArena) pt->local_x = -kArena;
            if (pt->local_x > kArena) pt->local_x = kArena;
            if (pt->local_z < -kArena) pt->local_z = -kArena;
            if (pt->local_z > kArena) pt->local_z = kArena;
            // Hop animation
            pt->local_y = 0.5f + ((len > 0.01f) ? std::abs(std::sin(gs.time * 10.0f)) * 0.15f : 0.0f);

            if (gs.invincible_timer > 0) gs.invincible_timer -= dt;

            // --- Coins: spin + bob, collect on proximity ---
            for (auto c : coins) {
                auto* t = world.get<scene::Transform>(c);
                t->rot_y += dt * 120.0f;
                t->local_y = 0.5f + std::sin(gs.time * 3.0f + t->local_x) * 0.2f;
                float dx = t->local_x - pt->local_x;
                float dz = t->local_z - pt->local_z;
                if (dx * dx + dz * dz < 1.0f) {
                    gs.score++;
                    gs.coins_to_next--;
                    t->local_x = rand_range(rng, -8.0f, 8.0f);
                    t->local_z = rand_range(rng, -8.0f, 8.0f);
                    if (gs.coins_to_next <= 0) {
                        gs.level++;
                        gs.coins_to_next = 5;
                        NF_LOG_INFO(LogCategory::Core, "Level up! now level {}", gs.level);
                    }
                }
            }

            // --- Enemy patrol ---
            float enemy_speed = 1.0f + (gs.level - 1) * 0.45f;
            gs.enemy_phase += dt * enemy_speed;
            auto* et = world.get<scene::Transform>(enemy);
            et->local_x = std::sin(gs.enemy_phase) * 8.0f;
            et->local_z = -3.0f + std::cos(gs.enemy_phase * 0.7f) * 4.0f;
            et->rot_y += dt * 60.0f;

            float edx = et->local_x - pt->local_x;
            float edz = et->local_z - pt->local_z;
            if (gs.invincible_timer <= 0 && (edx * edx + edz * edz) < 1.25f) {
                gs.lives--;
                gs.invincible_timer = 2.0f;
                pt->local_x = 0; pt->local_z = 5.0f;
                NF_LOG_INFO(LogCategory::Core, "Ouch! lives left {}", gs.lives);
                if (gs.lives <= 0) {
                    gs.game_over = true;
                    NF_LOG_INFO(LogCategory::Core, "GAME OVER score={} level={}", gs.score, gs.level);
                }
            }
        }

        // Title update ~4x/sec
        if (title_clock.elapsed_seconds() > 0.25) {
            title_clock.reset();
            char buf[256];
            if (gs.game_over)
                std::snprintf(buf, sizeof(buf), "NOVAForge — GAME OVER! Score=%d Level=%d | press R to restart, ESC to quit", gs.score, gs.level);
            else
                std::snprintf(buf, sizeof(buf), "NOVAForge — Cube Collector | Score=%d Lives=%d Level=%d | WASD/Arrows move", gs.score, gs.lives, gs.level);
            window.set_title(buf);
        }

        // --- Camera follows player ---
        auto* pt = world.get<scene::Transform>(player);
        rendering::Camera cam{};
        cam.position = {pt->local_x * 0.7f, 7.0f, pt->local_z + 7.5f};
        cam.target = {pt->local_x, 0.0f, pt->local_z};
        cam.aspect = float(window.width()) / float(window.height());
        cam.fov_y_rad = 55.0f * 3.14159265359f / 180.0f;
        cam.near_plane = 0.1f; cam.far_plane = 100.0f;
        rendering::update_camera(cam);

        scene::propagate_transforms(world);
        rendering::RenderWorld render_world;
        nf::runtime::extract_render_objects(world, meshes, render_world);

        frame_fence->wait(); frame_fence->reset();
        u32 image_index = swapchain->acquire_next_image(*image_available);
        if (image_index == u32_max) break;
        rhi::Texture* backbuffer = swapchain->get_texture(image_index);
        if (!backbuffer) break;

        cmd->reset(); cmd->begin();
        if (!renderer.render(*cmd, render_world, cam, *backbuffer, true)) {
            NF_LOG_ERROR(LogCategory::RHI, "render failed"); break;
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

        InputSystem::instance().end_frame();
    }

    device->wait_idle();
    renderer.shutdown();
    InputSystem::instance().shutdown();
    platform_shutdown();
    NF_LOG_INFO(LogCategory::Core, "SimpleGame exited score={} level={}", gs.score, gs.level);
    return 0;
}

} // namespace nf::sample::simplegame

int main() {
    nf::Logger::instance().add_sink(nf::Logger::make_console_sink());
    nf::Logger::instance().set_min_level(nf::LogLevel::Info);
    return nf::sample::simplegame::run();
}
