#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Runtime/BuiltinModules.hpp>
#include <NF/Runtime/PlatformerControllerModule.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/TouchInput.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

using namespace nf;

namespace {
struct PlatformerTestInput final : gameplay::IInputSource {
    float horizontal = 0.0f;
    bool jump = false;
    bool action_pressed(std::string_view action) const override { return action == "jump" && jump; }
    float action_axis(std::string_view action) const override { return action == "move_x" ? horizontal : 0.0f; }
};
}

NF_TEST(platformer_controller_moves_stops_and_only_jumps_from_ground) {
    gameplay::register_builtin_modules();
    NF_CHECK(gameplay::builtin_module_registered("PlatformerController"));
    physics::PhysicsWorld physics;
    physics::BodyDesc floor;
    floor.type = physics::BodyType::Static;
    floor.shape = physics::Shape::make_box(Vec3{8.0f, 0.5f, 2.0f});
    floor.position = Vec3{0.0f, -0.5f, 0.0f};
    physics.add_body(floor);
    physics::BodyDesc player;
    player.shape = physics::Shape::make_box(Vec3{0.5f, 0.5f, 0.5f});
    player.position = Vec3{0.0f, 1.0f, 0.0f};
    player.restitution = 0.0f;
    player.allow_sleep = false;
    const auto handle = physics.add_body(player);
    ecs::World world;
    const auto entity = world.create_entity();
    gameplay::GameplayModuleComponent mod;
    mod.module_name = "PlatformerController";
    world.add<gameplay::GameplayModuleComponent>(entity, mod);
    physics::RigidBodyComponent body;
    body.body = handle;
    world.add<physics::RigidBodyComponent>(entity, body);
    for (int i = 0; i < 180; ++i) physics.step(1.0f / 60.0f);
    PlatformerTestInput input;
    gameplay::GameplayContext ctx;
    ctx.world = &world;
    ctx.physics = &physics;
    ctx.input = &input;
    ctx.playing = true;
    ctx.dt = 1.0f / 60.0f;
    gameplay::PlatformerControllerModule controller;
    input.horizontal = 0.75f;
    controller.on_update(ctx);
    NF_CHECK_NEAR(physics.state(handle).linear_velocity.x, 3.0f, 1e-5f);
    input.horizontal = 0.0f;
    input.jump = true;
    controller.on_update(ctx);
    NF_CHECK_NEAR(physics.state(handle).linear_velocity.x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(physics.state(handle).linear_velocity.y, 7.0f, 1e-5f);
    for (int i = 0; i < 12; ++i) { physics.step(ctx.dt); controller.on_update(ctx); }
    const float falling_speed = physics.state(handle).linear_velocity.y;
    NF_CHECK(falling_speed < 6.0f);
    input.jump = false;
    controller.on_update(ctx);
    input.jump = true;
    controller.on_update(ctx);
    NF_CHECK_NEAR(physics.state(handle).linear_velocity.y, falling_speed, 1e-5f);
}

NF_TEST(platformer_template_runs_touch_actions_through_live_runtime) {
    const auto& gpu = test::require_gpu();
    assets::VirtualFileSystem vfs;
    vfs.mount("content://", NF_PLATFORMER_TEMPLATE_CONTENT);
    assets::AssetRegistry registry;
    std::string error;
    NF_CHECK(registry.load(vfs, "content://AssetRegistry.nfreg", error));
    assets::AssetManager manager(vfs, registry);
    runtime::TouchInputSource input;
    runtime::Runtime runtime(vfs, registry, manager, *gpu.device, nullptr);
    runtime.set_mobile_render_defaults();
    NF_CHECK_NEAR(runtime.renderer()->render_scale(), 0.4f, 1e-5f);
    NF_CHECK_EQ(runtime.renderer()->shadow_tile_size(), 256u);
    NF_CHECK_EQ(runtime.renderer()->local_shadow_tile_size(), 128u);
    NF_CHECK(!runtime.renderer()->ssao_enabled());
    NF_CHECK(runtime.load_scene("content://Scenes/Main.nfscene", error));
    runtime.set_playing(true);
    NF_CHECK(input.add_control({"move_x", 0.0f, 0.5f, 0.4f, 0.4f, 1.0f}));
    NF_CHECK(input.add_control({"jump", 0.6f, 0.5f, 0.4f, 0.4f, 1.0f}));
    runtime.set_input_source(&input);
    ecs::Entity player{};
    for (const auto e : runtime.scene()->world().query<gameplay::GameplayModuleComponent>()) {
        const auto* module = runtime.scene()->world().get<gameplay::GameplayModuleComponent>(e);
        if (module && module->module_name == "PlatformerController") { player = e; break; }
    }
    NF_CHECK(player.valid());
    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 140; ++i) runtime.update(dt);
    const auto* transform = runtime.scene()->world().get<scene::Transform>(player);
    NF_CHECK(transform != nullptr);
    const float start_x = transform->world_x;
    NF_CHECK(input.touch_down(1, 0.2f, 0.7f));
    for (int i = 0; i < 20; ++i) runtime.update(dt);
    input.touch_up(1);
    transform = runtime.scene()->world().get<scene::Transform>(player);
    NF_CHECK(transform->world_x > start_x + 0.8f);
    const float ground_y = transform->world_y;
    NF_CHECK(input.touch_down(2, 0.8f, 0.7f));
    for (int i = 0; i < 20; ++i) runtime.update(dt);
    input.touch_up(2);
    transform = runtime.scene()->world().get<scene::Transform>(player);
    NF_CHECK(transform->world_y > ground_y + 0.5f);
    runtime.set_input_source(nullptr);
}
