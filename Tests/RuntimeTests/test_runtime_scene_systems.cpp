// Tests/RuntimeTests/test_runtime_scene_systems.cpp — are the wired systems
// really driven by the frame?
//
// VFX particles, cloth, the character mover and the AI world budget are all
// unit-tested in isolation. This file is the Rule 0 proof for Phase 25: a
// scene that merely declares `Particles:` / `Cloth:` / `Character:` ends up
// built, stepped by Runtime::update() and observable — with no test driving
// the subsystems by hand. Headless like the destruction reachability suite:
// NF_SKIP when no Vulkan device is available.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Vfx/Components.hpp>
#include <NF/AI/AIWorld.hpp>
#include <NF/Audio/Components.hpp>

#include <filesystem>
#include <memory>
#include <string>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::scene;

namespace {

constexpr float kDt = 1.0f / 60.0f;

struct HeadlessRuntime {
    std::filesystem::path tmp;
    VirtualFileSystem vfs;
    AssetRegistry registry;
    AssetManager manager;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<Runtime> runtime;

    HeadlessRuntime() : manager(vfs, registry) {
        tmp = std::filesystem::temp_directory_path() / "nf_runtime_scene_systems";
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp);
        vfs.mount("content://", tmp);

        device = rhi::create_device();
        if (!device) {
            return;
        }
        rhi::DeviceDesc desc{};
        desc.window_handle = nullptr;
        desc.enable_validation = false;
        if (!device->init(desc)) {
            device.reset();
            return;
        }
        runtime = std::make_unique<Runtime>(vfs, registry, manager, *device, nullptr);
    }

    ~HeadlessRuntime() {
        runtime.reset();
        if (device) {
            device->wait_idle();
            device->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }

    HeadlessRuntime(const HeadlessRuntime&) = delete;
    HeadlessRuntime& operator=(const HeadlessRuntime&) = delete;

    bool ok() const { return static_cast<bool>(runtime); }
};

/// Static floor slab (top face at y = 0) for the character landing test.
ecs::Entity add_floor(ecs::World& w) {
    ecs::Entity floor = w.create_entity();
    Transform t{};
    t.local_y = -1.0f;
    w.add<Transform>(floor, t);
    physics::RigidBodyComponent rb;
    rb.type = physics::BodyType::Static;
    w.add<physics::RigidBodyComponent>(floor, rb);
    physics::ColliderComponent col;
    col.shape = physics::Shape::make_box(Vec3{20.0f, 1.0f, 20.0f});
    w.add<physics::ColliderComponent>(floor, col);
    return floor;
}

} // namespace

NF_TEST(particles_line_round_trips_through_scene_text) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_particles_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("ParticlesRoundTrip");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<Transform>(e, Transform{});
    vfx::ParticleComponent pc;
    pc.config.rate = 120.0f;
    pc.config.lifetime = 2.0f;
    pc.config.velocity = Vec3{1.0f, 2.0f, 3.0f};
    pc.config.max_particles = 512;
    pc.enabled = false;
    scene.world().add<vfx::ParticleComponent>(e, pc);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/p.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/p.nfscene");
    NF_CHECK(result.success);
    const auto* loaded =
        result.scene->world().get<vfx::ParticleComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK_NEAR(loaded->config.rate, 120.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->config.lifetime, 2.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->config.velocity.x, 1.0f, 1e-5f);
        NF_CHECK_EQ(loaded->config.max_particles, 512u);
        NF_CHECK(!loaded->enabled);
    }
    std::filesystem::remove_all(tmp);
}

NF_TEST(particles_line_rejects_invalid_values) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_particles_bad";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        "# SANAD Scene v1\n"
        "version: 1\n"
        "name: Bad\n"
        "entity_count: 1\n"
        "---\n"
        "entity: 0:0\n"
        "  Name: A\n"
        "  Particles: rate=-5.0 lifetime=1.5\n";
    NF_CHECK(vfs.write_text("content://Scenes/bad.nfscene", text).ok);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/bad.nfscene");
    NF_CHECK(result.success);
    NF_CHECK_EQ(result.scene->world().query<vfx::ParticleComponent>().size(), static_cast<size_t>(0));
    NF_CHECK(!result.warnings.empty());
    std::filesystem::remove_all(tmp);
}

NF_TEST(cloth_line_round_trips_through_scene_text) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_cloth_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("ClothRoundTrip");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<Transform>(e, Transform{});
    physics::ClothComponent cc;
    cc.config.res_x = 8;
    cc.config.res_z = 6;
    cc.config.stiffness = 0.5f;
    scene.world().add<physics::ClothComponent>(e, cc);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/c.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/c.nfscene");
    NF_CHECK(result.success);
    const auto* loaded =
        result.scene->world().get<physics::ClothComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK_EQ(loaded->config.res_x, 8);
        NF_CHECK_EQ(loaded->config.res_z, 6);
        NF_CHECK_NEAR(loaded->config.stiffness, 0.5f, 1e-5f);
    }
    std::filesystem::remove_all(tmp);
}

NF_TEST(character_line_round_trips_through_scene_text) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_character_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("CharacterRoundTrip");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<Transform>(e, Transform{});
    physics::CharacterComponent ch;
    ch.config.max_speed = 8.0f;
    ch.config.jump_speed = 5.0f;
    ch.wish_dir = Vec3{9.0f, 9.0f, 9.0f}; // live input: must NOT persist
    ch.jump = true;
    ch.grounded = true;
    scene.world().add<physics::CharacterComponent>(e, ch);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/ch.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/ch.nfscene");
    NF_CHECK(result.success);
    const auto* loaded = result.scene->world().get<physics::CharacterComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK_NEAR(loaded->config.max_speed, 8.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->config.jump_speed, 5.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->wish_dir.x, 0.0f, 1e-6f);
        NF_CHECK(!loaded->jump);
        NF_CHECK(!loaded->grounded);
    }
    std::filesystem::remove_all(tmp);
}

NF_TEST(audio_min_max_distance_round_trips_through_scene_text) {
    // Regression: the inspector edited min/max for months while the loader
    // silently dropped them — every tuned falloff reverted on reopen.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_audio_range";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("AudioRange");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<Transform>(e, Transform{});
    audio::AudioComponent aud;
    aud.buffer_name = "content://Audio/shot.wav";
    aud.spatial = true;
    aud.spatial_settings.min_distance = 2.0f;
    aud.spatial_settings.max_distance = 30.0f;
    scene.world().add<audio::AudioComponent>(e, std::move(aud));

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/a.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/a.nfscene");
    NF_CHECK(result.success);
    const auto* loaded =
        result.scene->world().get<audio::AudioComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK_NEAR(loaded->spatial_settings.min_distance, 2.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->spatial_settings.max_distance, 30.0f, 1e-5f);
    }
    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_particles_are_stepped_by_update) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Emitting");
    ecs::Entity e = s->world().create_entity();
    s->world().add<Transform>(e, Transform{});
    vfx::ParticleComponent pc;
    pc.config.rate = 600.0f;
    s->world().add<vfx::ParticleComponent>(e, std::move(pc));

    env.runtime->adopt_scene(std::move(s), "content://Scenes/Emitting.nfscene");
    NF_CHECK_EQ(env.runtime->particle_emitter_count(), static_cast<size_t>(1));
    NF_CHECK(env.runtime->particle_system(env.runtime->scene()->world().all_entities().front()) !=
             nullptr);

    for (int i = 0; i < 30; ++i) {
        env.runtime->update(kDt);
    }
    // 600/s over 0.5 s with a 1.5 s lifetime: hundreds alive. Zero would mean
    // update() never stepped the emitter.
    NF_CHECK(env.runtime->particles_alive() > 100u);
}

NF_TEST(scene_cloth_falls_under_gravity) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Sheet");
    ecs::Entity e = s->world().create_entity();
    Transform t{};
    t.local_y = 2.0f;
    s->world().add<Transform>(e, t);
    physics::ClothComponent cc;
    cc.config.res_x = 6;
    cc.config.res_z = 6;
    s->world().add<physics::ClothComponent>(e, std::move(cc));

    env.runtime->adopt_scene(std::move(s), "content://Scenes/Sheet.nfscene");
    NF_CHECK_EQ(env.runtime->cloth_count(), static_cast<size_t>(1));

    const physics::Cloth* cloth =
        env.runtime->cloth(env.runtime->scene()->world().all_entities().front());
    NF_CHECK(cloth != nullptr);
    if (cloth == nullptr) {
        return;
    }
    const int center = cloth->index(3, 3);
    const float y0 = cloth->position(center).y;
    for (int i = 0; i < 30; ++i) {
        env.runtime->update(kDt);
    }
    // Free fall with no colliders: the center must have dropped.
    NF_CHECK(cloth->position(center).y < y0 - 0.05f);
}

NF_TEST(scene_character_lands_grounded_and_walks) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Hero");
    add_floor(s->world());
    ecs::Entity hero = s->world().create_entity();
    Transform t{};
    t.local_y = 2.0f;
    s->world().add<Transform>(hero, t);
    physics::CharacterComponent ch;
    s->world().add<physics::CharacterComponent>(hero, std::move(ch));

    env.runtime->adopt_scene(std::move(s), "content://Scenes/Hero.nfscene");
    NF_CHECK_EQ(env.runtime->character_count(), static_cast<size_t>(1));

    // Fall until grounded (budgeted: a loop without one cannot tell "never
    // lands" from "was not given the chance").
    ecs::Entity live_hero = env.runtime->scene()->world().all_entities().back();
    bool landed = false;
    for (int i = 0; i < 300; ++i) {
        env.runtime->update(kDt);
        live_hero = env.runtime->edit_scene()->world().all_entities().back();
        if (env.runtime->character_grounded(live_hero)) {
            landed = true;
            break;
        }
    }
    NF_CHECK(landed);
    if (!landed) {
        return;
    }
    const auto* tr = env.runtime->scene()->world().get<Transform>(live_hero);
    NF_CHECK(tr != nullptr);
    if (tr != nullptr) {
        // Resting on the slab top (y = 0) as a 0.4-radius sphere.
        NF_CHECK(tr->local_y > 0.15f && tr->local_y < 0.8f);
    }

    // Walk: wish +X for a second through the real frame path.
    auto* comp = env.runtime->edit_scene()->world().get<physics::CharacterComponent>(live_hero);
    NF_CHECK(comp != nullptr);
    if (comp == nullptr) {
        return;
    }
    comp->wish_dir = Vec3{1.0f, 0.0f, 0.0f};
    const float x0 = tr != nullptr ? tr->local_x : 0.0f;
    for (int i = 0; i < 60; ++i) {
        env.runtime->update(kDt);
    }
    const auto* tr2 = env.runtime->scene()->world().get<Transform>(live_hero);
    NF_CHECK(tr2 != nullptr);
    if (tr2 != nullptr) {
        NF_CHECK(tr2->local_x > x0 + 0.5f);
    }
}

NF_TEST(scene_ai_world_plans_every_update) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Crowd");
    ecs::Entity e = s->world().create_entity();
    s->world().add<Transform>(e, Transform{});
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Crowd.nfscene");
    NF_CHECK(env.runtime->ai_world() != nullptr);

    ai::AIActor actor;
    actor.position = Vec3{0.0f, 0.0f, 0.0f};
    const u32 actor_id = env.runtime->ai_world()->add_actor(actor);
    ai::AIFocus focus;
    focus.position = Vec3{0.0f, 0.0f, 0.0f};
    focus.radius = 30.0f;
    env.runtime->ai_world()->add_focus(focus);
    // Rates accrue fractionally (20/s earns 0.33 on the first 1/60 plan and
    // grants nothing), so open the budget: the tier assertion below is about
    // placement, not about budget pacing (paced pacing has its own suite).
    env.runtime->ai_world()->set_budget(120.0f, 120.0f);

    env.runtime->update(kDt);
    // At the focus with budget to spare: the whole stack, granted.
    NF_CHECK(env.runtime->ai_world()->tier_of(actor_id) == ai::SimTier::FullAI);
    NF_CHECK(env.runtime->ai_world()->wants_tick(actor_id));
}

NF_TEST(disabled_systems_never_bind) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Quiet");
    ecs::Entity a = s->world().create_entity();
    s->world().add<Transform>(a, Transform{});
    vfx::ParticleComponent pc;
    pc.enabled = false;
    s->world().add<vfx::ParticleComponent>(a, std::move(pc));
    ecs::Entity b = s->world().create_entity();
    s->world().add<Transform>(b, Transform{});
    physics::ClothComponent cc;
    cc.enabled = false;
    s->world().add<physics::ClothComponent>(b, std::move(cc));
    ecs::Entity c = s->world().create_entity();
    s->world().add<Transform>(c, Transform{});
    physics::CharacterComponent ch;
    ch.enabled = false;
    s->world().add<physics::CharacterComponent>(c, std::move(ch));

    env.runtime->adopt_scene(std::move(s), "content://Scenes/Quiet.nfscene");
    NF_CHECK_EQ(env.runtime->particle_emitter_count(), static_cast<size_t>(0));
    NF_CHECK_EQ(env.runtime->cloth_count(), static_cast<size_t>(0));
    NF_CHECK_EQ(env.runtime->character_count(), static_cast<size_t>(0));
    env.runtime->update(kDt);
    NF_CHECK_EQ(env.runtime->particles_alive(), static_cast<size_t>(0));
}
