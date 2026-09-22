// Samples/VehicleDemo/main.cpp — Jolt vehicle driving demo (Phase 17 showcase).
//
// A drivable Jolt arcade car on wheels you can see: chassis + 4 wheel meshes
// posed every tick from JoltWheelState (position, steer angle, spin), dynamic
// obstacle boxes to crash into, and golden gates to collect for score.
// It dogfoods the engine's own gameplay path — VehicleComponent (the ONLY way
// to drive: throttle/steer/brake in [-1, 1]) + VehicleSystem (ECS bridge) —
// so the demo plays exactly what the tests verify.
//
// Controls: W/Up/RT = throttle, S/Down/LT = brake/reverse, A/D/Arrows/Left stick = steer,
//           Space/A = handbrake (rear wheels only), R/Y/Start = reset car to spawn,
//           ESC/Back = quit. Keyboard and Xbox pad share one InputMapper.

#include <NF/Core/Logger.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/Platform/InputSystem.hpp>
#include <NF/Input/InputAction.hpp>
#include <NF/Input/InputContext.hpp>
#include <NF/Input/InputMapper.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/Physics/JoltWorld.hpp>
#include <NF/Physics/VehicleSystem.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace nf::sample::vehicledemo {

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

constexpr float kFixedStep = 1.0f / 60.0f;
constexpr float kRadToDeg = 180.0f / 3.14159265359f;
// Resting spawn (wheels ~0.2m above the plane): dropping the car from height
// lands it with a bounce yaw that reads as the car "veering" on its own.
const Vec3 kSpawn{0.0f, 0.9f, 0.0f};

float rand_range(std::mt19937& rng, float lo, float hi) {
    std::uniform_real_distribution<float> d(lo, hi);
    return d(rng);
}

// Gameplay-facing drive bindings: one context serves keyboard and gamepad.
// Axis1D throttle/brake/steer compose digital keys with analog triggers/stick;
// Bool actions take any held source (§50 actions, not keys).
input::InputMapper make_drive_mapper() {
    input::InputMapper mapper;
    input::InputContext& drive = mapper.create_context("driving", 0);

    input::InputAction throttle("Throttle", input::ActionType::Axis1D);
    throttle.add_key(KeyCode::W);
    throttle.add_key(KeyCode::Up);
    throttle.add_gamepad_axis(GamepadAxis::RightTrigger, input::AxisComponent::None, 1.0f);
    drive.add_action(std::move(throttle));

    input::InputAction brake("Brake", input::ActionType::Axis1D);
    brake.add_key(KeyCode::S);
    brake.add_key(KeyCode::Down);
    brake.add_gamepad_axis(GamepadAxis::LeftTrigger, input::AxisComponent::None, 1.0f);
    drive.add_action(std::move(brake));

    input::InputAction steer("Steer", input::ActionType::Axis1D);
    steer.add_key(KeyCode::A, input::AxisComponent::None, -1.0f);
    steer.add_key(KeyCode::D, input::AxisComponent::None, 1.0f);
    steer.add_key(KeyCode::Left, input::AxisComponent::None, -1.0f);
    steer.add_key(KeyCode::Right, input::AxisComponent::None, 1.0f);
    steer.add_gamepad_axis(GamepadAxis::LeftX, input::AxisComponent::None, 1.0f);
    drive.add_action(std::move(steer));

    input::InputAction handbrake("Handbrake", input::ActionType::Bool);
    handbrake.add_key(KeyCode::Space);
    handbrake.add_gamepad_button(GamepadButton::A);
    drive.add_action(std::move(handbrake));

    input::InputAction reset("Reset", input::ActionType::Bool);
    reset.add_key(KeyCode::R);
    reset.add_gamepad_button(GamepadButton::Y);
    reset.add_gamepad_button(GamepadButton::Start);
    drive.add_action(std::move(reset));

    input::InputAction quit("Quit", input::ActionType::Bool);
    quit.add_key(KeyCode::Escape);
    quit.add_gamepad_button(GamepadButton::Back);
    drive.add_action(std::move(quit));

    return mapper;
}

int run() {
    auto sdir = shader_dir();
    if (sdir.empty()) { NF_LOG_FATAL(LogCategory::Core, "VehicleDemo: shader dir not found"); return -1; }

    platform_init();
    InputSystem::instance().init();

    WindowDesc wdesc{};
    wdesc.width = 1280; wdesc.height = 720;
    wdesc.title = "NOVAForge — Vehicle Demo | WASD/Xbox drive, Space/A handbrake, R reset";
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

    auto make_mat = [&](float r, float g, float b, const char* name) {
        rendering::PBRMaterialParams p{};
        p.base_color[0] = r; p.base_color[1] = g; p.base_color[2] = b; p.base_color[3] = 1.0f;
        p.roughness = 0.45f;
        return renderer.materials().create_instance(*renderer.gbuffer_material(), p, name);
    };
    rendering::MaterialHandle chassis_mat = make_mat(0.15f, 0.45f, 0.95f, "chassis");
    rendering::MaterialHandle cabin_mat = make_mat(0.75f, 0.85f, 0.95f, "cabin");
    rendering::MaterialHandle glass_mat = make_mat(0.04f, 0.07f, 0.12f, "glass");
    rendering::MaterialHandle bumper_mat = make_mat(0.10f, 0.10f, 0.12f, "bumper");
    rendering::MaterialHandle headlight_mat = make_mat(1.0f, 0.95f, 0.75f, "headlight");
    rendering::MaterialHandle taillight_mat = make_mat(0.9f, 0.08f, 0.08f, "taillight");
    rendering::MaterialHandle wheel_mat = make_mat(0.08f, 0.08f, 0.10f, "wheel");
    rendering::MaterialHandle hub_mat = make_mat(0.55f, 0.57f, 0.60f, "hub");
    rendering::MaterialHandle ground_mat = make_mat(0.25f, 0.27f, 0.32f, "ground");
    rendering::MaterialHandle obstacle_mat = make_mat(0.95f, 0.45f, 0.10f, "obstacle");
    rendering::MaterialHandle gate_mat = make_mat(1.0f, 0.8f, 0.15f, "gate");

    // --- Physics: Jolt world, static ground plane, obstacle boxes -----------
    physics::JoltWorld jolt;
    if (!jolt.valid()) { NF_LOG_FATAL(LogCategory::Core, "VehicleDemo: Jolt world invalid"); return -1; }
    {
        physics::BodyDesc ground{};
        ground.type = physics::BodyType::Static;
        ground.shape = physics::Shape::make_plane(Vec3{0.0f, 1.0f, 0.0f});
        jolt.add_body(ground);
    }

    ecs::World world;

    // Visual ground slab (physics ground is the Jolt plane above).
    {
        ecs::Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        auto* t = world.get<scene::Transform>(e);
        t->local_y = -0.35f;
        t->scale_x = 40.0f; t->scale_y = 0.3f; t->scale_z = 40.0f;
        world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, ground_mat, true});
    }

    // Obstacles: Jolt dynamic boxes + synced render entities (a stack to smash).
    struct Obstacle { ecs::Entity entity; physics::JoltBody body; };
    std::vector<Obstacle> obstacles;
    auto add_obstacle = [&](float x, float y, float z, float half) {
        physics::BodyDesc desc{};
        desc.shape = physics::Shape::make_box(Vec3{half, half, half});
        desc.position = Vec3{x, y, z};
        desc.mass = 8.0f;
        physics::JoltBody body = jolt.add_body(desc);
        if (!body.valid()) return;
        ecs::Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        auto* t = world.get<scene::Transform>(e);
        t->local_x = x; t->local_y = y; t->local_z = z;
        t->scale_x = t->scale_y = t->scale_z = half * 2.0f;
        world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, obstacle_mat, true});
        obstacles.push_back({e, body});
    };
    add_obstacle(7.0f, 0.5f, -4.0f, 0.5f);
    add_obstacle(7.0f, 1.5f, -4.0f, 0.5f);
    add_obstacle(7.0f, 2.5f, -4.0f, 0.5f);
    add_obstacle(-6.0f, 0.5f, 5.0f, 0.5f);
    add_obstacle(-5.0f, 0.5f, 6.0f, 0.5f);
    add_obstacle(0.0f, 0.5f, -10.0f, 0.75f);
    add_obstacle(-9.0f, 0.5f, -8.0f, 0.5f);

    // --- The car: VehicleComponent + chassis/wheel render entities ----------
    ecs::Entity car = world.create_entity();
    world.add<scene::Transform>(car, scene::Transform{});
    {
        auto* t = world.get<scene::Transform>(car);
        t->local_x = kSpawn.x; t->local_y = kSpawn.y; t->local_z = kSpawn.z;
    }
    physics::VehicleComponent vc; // default JoltVehicleConfig: front = +Z
    // Stability tune for the demo: wider track and less twitchy steering than
    // the raw defaults so full-lock at speed slides instead of rolling, plus a
    // firmer, better-damped suspension that keeps the chassis level over the
    // obstacle boxes at full throttle. Tire grip just past 1.0 (a real tire
    // profile holds more than its contact patch alone would).
    vc.config.track_half_width = 1.0f;
    vc.config.max_steer_deg = 22.0f;
    vc.config.suspension_frequency_hz = 2.0f;
    vc.config.suspension_damping = 0.85f;
    vc.config.tire_peak_friction = 1.3f;
    vc.config.tire_limit_friction = 1.05f;
    world.add<physics::VehicleComponent>(car, vc);
    {
        // Chassis visual matches the default config half extents (0.9/0.5/2.0).
        world.add<rendering::MeshComponent>(car, rendering::MeshComponent{cube_handle, chassis_mat, true});
        auto* t = world.get<scene::Transform>(car);
        t->scale_x = 1.8f; t->scale_y = 1.0f; t->scale_z = 4.0f;
    }
    // --- Body panels: plain Transform+Mesh parts posed rigidly from the
    // --- chassis state every step (Transform v0.1 does not propagate parent
    // --- rotation, so hierarchy parenting would leave the cabin behind in
    // --- corners — explicit offsets rotated by the chassis quaternion instead).
    struct BodyPanel { ecs::Entity entity; Vec3 offset; };
    std::vector<BodyPanel> panels;
    auto add_panel = [&](rendering::MaterialHandle mat, Vec3 offset, Vec3 size) {
        ecs::Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        auto* t = world.get<scene::Transform>(e);
        t->scale_x = size.x; t->scale_y = size.y; t->scale_z = size.z;
        world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, mat, true});
        panels.push_back({e, offset});
    };
    add_panel(cabin_mat, Vec3{0.0f, 0.75f, -0.3f}, Vec3{1.5f, 0.55f, 2.0f});      // cabin
    add_panel(glass_mat, Vec3{0.0f, 0.70f, 0.75f}, Vec3{1.3f, 0.40f, 0.15f});     // windshield
    add_panel(glass_mat, Vec3{0.0f, 0.70f, -1.30f}, Vec3{1.3f, 0.40f, 0.15f});    // rear glass
    add_panel(bumper_mat, Vec3{0.0f, -0.30f, 2.00f}, Vec3{1.9f, 0.30f, 0.30f});   // front bumper
    add_panel(bumper_mat, Vec3{0.0f, -0.30f, -2.00f}, Vec3{1.9f, 0.30f, 0.30f});  // rear bumper
    add_panel(headlight_mat, Vec3{-0.55f, -0.05f, 2.00f}, Vec3{0.30f, 0.20f, 0.10f});
    add_panel(headlight_mat, Vec3{0.55f, -0.05f, 2.00f}, Vec3{0.30f, 0.20f, 0.10f});
    add_panel(taillight_mat, Vec3{-0.55f, -0.05f, -2.00f}, Vec3{0.30f, 0.20f, 0.10f});
    add_panel(taillight_mat, Vec3{0.55f, -0.05f, -2.00f}, Vec3{0.30f, 0.20f, 0.10f});
    std::vector<ecs::Entity> wheels;
    std::vector<ecs::Entity> hubs;
    for (int i = 0; i < 4; ++i) {
        ecs::Entity w = world.create_entity();
        world.add<scene::Transform>(w, scene::Transform{});
        auto* t = world.get<scene::Transform>(w);
        t->scale_x = 0.32f; t->scale_y = 0.72f; t->scale_z = 0.72f; // tire
        world.add<rendering::MeshComponent>(w, rendering::MeshComponent{cube_handle, wheel_mat, true});
        wheels.push_back(w);
        ecs::Entity h = world.create_entity();
        world.add<scene::Transform>(h, scene::Transform{});
        auto* ht = world.get<scene::Transform>(h);
        ht->scale_x = 0.34f; ht->scale_y = 0.30f; ht->scale_z = 0.30f; // rim
        world.add<rendering::MeshComponent>(h, rendering::MeshComponent{cube_handle, hub_mat, true});
        hubs.push_back(h);
    }
    float wheel_spin[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    physics::VehicleSystem vehicles(jolt);
    vehicles.startup(world);
    if (vehicles.vehicle_count() != 1) {
        NF_LOG_FATAL(LogCategory::Core, "VehicleDemo: car did not spawn");
        return -1;
    }

    // --- Golden gates: drive through to score -------------------------------
    constexpr int kGates = 5;
    std::vector<ecs::Entity> gates;
    std::mt19937 rng(777);
    for (int i = 0; i < kGates; ++i) {
        ecs::Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        auto* t = world.get<scene::Transform>(e);
        t->local_x = rand_range(rng, -14.0f, 14.0f);
        t->local_y = 1.0f;
        t->local_z = rand_range(rng, -14.0f, 14.0f);
        t->scale_x = t->scale_y = t->scale_z = 1.1f;
        world.add<rendering::MeshComponent>(e, rendering::MeshComponent{cube_handle, gate_mat, true});
        gates.push_back(e);
    }

    renderer.set_directional_light(rendering::DirectionalLight{
        rendering::Vec3{-0.5f, -1.0f, -0.3f}, rendering::Vec3{1.0f, 1.0f, 1.0f}, 1.0f, true});
    renderer.set_ambient(0.35f);

    auto image_available = device->create_semaphore();
    auto frame_fence = device->create_fence(true);
    auto cmd = device->create_command_buffer();
    std::vector<std::unique_ptr<rhi::Semaphore>> render_finished(swapchain->image_count());
    for (auto& s : render_finished) s = device->create_semaphore();

    int score = 0;
    int resets = 0;
    float time = 0.0f;
    float physics_acc = 0.0f;
    Timer timer; timer.start();
    Clock title_clock;

    NF_LOG_INFO(LogCategory::Core,
                "VehicleDemo: entering loop (WASD/Xbox drive, Space/A brake, R reset)");

    input::InputMapper drive_mapper = make_drive_mapper();
    bool pad_connected = false;

    while (!window.should_close()) {
        float dt = timer.tick();
        if (dt > 0.05f) dt = 0.05f; // clamp long frames
        time += dt;

        InputSystem::instance().begin_frame();
        window.poll_events();
        if (window.should_close()) break;

        auto& input = InputSystem::instance();
        drive_mapper.update(input.state());
        if (drive_mapper.is_pressed("Quit")) break;
        if (drive_mapper.just_pressed("Reset")) {
            vehicles.reset(car, world, kSpawn);
            physics_acc = 0.0f;
            resets++;
            NF_LOG_INFO(LogCategory::Core, "VehicleDemo: car reset");
        }

        // --- Drive intents: the ONLY channel into the vehicle ---------------
        // Values come from the action map (keys + pad compose into one action).
        const float throttle = drive_mapper.get_axis("Throttle");
        const float brake = drive_mapper.get_axis("Brake");
        const float steer = drive_mapper.get_axis("Steer");
        const float handbrake = drive_mapper.is_pressed("Handbrake") ? 1.0f : 0.0f;
        if (auto* drive = world.get<physics::VehicleComponent>(car)) {
            drive->throttle = throttle;
            drive->steer = steer;
            drive->brake = brake;
            drive->handbrake = handbrake;
        }

        // --- Fixed-step physics: inputs, world step, pose writeback ---------
        physics_acc += dt;
        int substeps = 0;
        while (physics_acc >= kFixedStep && substeps < 5) {
            vehicles.update(world); // arcade inputs -> Jolt, chassis -> Transform
            jolt.step(kFixedStep);
            physics_acc -= kFixedStep;
            substeps++;
        }
        if (substeps == 5) physics_acc = 0.0f; // spiral-of-death guard

        // Chassis truth for this frame (pose every visual off this one read).
        auto* drive_comp = world.get<physics::VehicleComponent>(car);
        const physics::JoltBodyState chassis = drive_comp->vehicle->chassis_state();
        float chassis_rx = 0.0f, chassis_ry = 0.0f, chassis_rz = 0.0f;
        scene::euler_xyz_degrees_from_quat(chassis.rotation, chassis_rx, chassis_ry,
                                           chassis_rz);

        // --- Body panels: rigid offsets rotated by the chassis quaternion ---
        for (const BodyPanel& p : panels) {
            auto* t = world.get<scene::Transform>(p.entity);
            if (t == nullptr) continue;
            const Vec3 wp = chassis.position + chassis.rotation.rotate(p.offset);
            t->local_x = wp.x; t->local_y = wp.y; t->local_z = wp.z;
            t->rot_x = chassis_rx; t->rot_y = chassis_ry; t->rot_z = chassis_rz;
            t->dirty = true;
        }

        // --- Wheels: world-space pose + steer + spin (hubs copy the wheel) --
        if (auto* drive = world.get<physics::VehicleComponent>(car)) {
            if (drive->vehicle != nullptr) {
                const std::vector<physics::JoltWheelState> states = drive->vehicle->wheel_states();
                for (usize i = 0; i < states.size() && i < wheels.size(); ++i) {
                    auto* t = world.get<scene::Transform>(wheels[i]);
                    if (t == nullptr) continue;
                    t->local_x = states[i].position.x;
                    t->local_y = states[i].position.y;
                    t->local_z = states[i].position.z;
                    wheel_spin[i] += states[i].angular_velocity * dt;
                    t->rot_x = wheel_spin[i] * kRadToDeg;
                    t->rot_y = states[i].steer_angle * kRadToDeg;
                    t->dirty = true;
                    if (auto* ht = world.get<scene::Transform>(hubs[i])) {
                        ht->local_x = t->local_x;
                        ht->local_y = t->local_y;
                        ht->local_z = t->local_z;
                        ht->rot_x = t->rot_x;
                        ht->rot_y = t->rot_y;
                        ht->dirty = true;
                    }
                }
            }
        }

        // --- Obstacles: physics -> render (position + heading) ---------------
        for (const Obstacle& o : obstacles) {
            const physics::JoltBodyState st = jolt.state(o.body);
            auto* t = world.get<scene::Transform>(o.entity);
            if (t == nullptr) continue;
            t->local_x = st.position.x;
            t->local_y = st.position.y;
            t->local_z = st.position.z;
            scene::euler_xyz_degrees_from_quat(st.rotation, t->rot_x, t->rot_y, t->rot_z);
            t->dirty = true;
        }

        // --- Gates: spin, collect on XZ proximity to the chassis -------------
        for (auto g : gates) {
            auto* t = world.get<scene::Transform>(g);
            t->rot_y += dt * 90.0f;
            const float dx = t->local_x - chassis.position.x;
            const float dz = t->local_z - chassis.position.z;
            if (dx * dx + dz * dz < 6.25f) {
                score++;
                t->local_x = rand_range(rng, -14.0f, 14.0f);
                t->local_z = rand_range(rng, -14.0f, 14.0f);
                NF_LOG_INFO(LogCategory::Core, "VehicleDemo: gate! score={}", score);
            }
        }

        // Title HUD: speed + score + a per-wheel contact readout (the
        // JoltWheelState path the wheel meshes pose from, surfaced as text).
        if (title_clock.elapsed_seconds() > 0.25) {
            title_clock.reset();
            const bool pad_now = input.gamepad_connected();
            if (pad_now != pad_connected) {
                pad_connected = pad_now;
                NF_LOG_INFO(LogCategory::Platform, "VehicleDemo: gamepad {}",
                            pad_connected ? "connected" : "disconnected");
            }
            char buf[320];
            const float kmh = chassis.linear_velocity.x * chassis.linear_velocity.x +
                              chassis.linear_velocity.z * chassis.linear_velocity.z;
            // Wheels in contact: 4 bits, FL FR RL RR. Airborne reads as '-',
            // which is also the "you left the ground" tell from the suspension.
            const std::vector<physics::JoltWheelState> ws =
                drive_comp->vehicle->wheel_states();
            char wtxt[8] = "----";
            for (usize i = 0; i < ws.size() && i < 4; ++i) {
                wtxt[i] = ws[i].in_contact ? 'O' : '-';
            }
            const char* hand = pad_connected
                                   ? "Xbox drive, A handbrake, Y reset, Back quit"
                                   : "WASD drive, Space handbrake, R reset";
            std::snprintf(buf, sizeof(buf),
                          "NOVAForge — Vehicle Demo | %d km/h | Wheels=%.4s | Gates=%d Resets=%d | %s",
                          static_cast<int>(std::sqrt(kmh) * 3.6f), wtxt, score, resets,
                          hand);
            window.set_title(buf);
        }

        // --- Chase camera: behind the car along its heading ------------------
        // Looks AT the car (not past it) from slightly above: a far lead
        // point tips the view down and pushes the car out of frame.
        const Vec3 forward = chassis.rotation.rotate(Vec3{0.0f, 0.0f, 1.0f});
        const Vec3 cam_pos{chassis.position.x - forward.x * 8.0f,
                           chassis.position.y + 2.8f,
                           chassis.position.z - forward.z * 8.0f};
        rendering::Camera cam{};
        cam.position = {cam_pos.x, cam_pos.y, cam_pos.z};
        cam.target = {chassis.position.x + forward.x * 1.5f, chassis.position.y + 0.8f,
                      chassis.position.z + forward.z * 1.5f};
        cam.aspect = float(window.width()) / float(window.height());
        cam.fov_y_rad = 55.0f * 3.14159265359f / 180.0f;
        cam.near_plane = 0.1f; cam.far_plane = 200.0f;
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

        drive_mapper.end_frame();
        InputSystem::instance().end_frame();
    }

    device->wait_idle();
    renderer.shutdown();
    InputSystem::instance().shutdown();
    platform_shutdown();
    NF_LOG_INFO(LogCategory::Core, "VehicleDemo exited score={} resets={}", score, resets);
    return 0;
}

} // namespace nf::sample::vehicledemo

int main() {
    nf::Logger::instance().add_sink(nf::Logger::make_console_sink());
    nf::Logger::instance().set_min_level(nf::LogLevel::Info);
    return nf::sample::vehicledemo::run();
}
