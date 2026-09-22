// Tests/RuntimeTests/test_runtime_scene_destruction.cpp — is destruction
// reachable from a scene?
//
// test_runtime_destruction.cpp proves the seam works when a test drives it by
// hand: enable, register, bind, blast, step. It cannot prove what this file is
// for. Before adopt_scene cooked and bound a DestructibleComponent, the whole
// Phase 19 chain — enable → cook → bind → damage → step → draw — was reachable
// only from tests. No sample, no editor and no game entry point called any of
// it, which by Rule 0 means it was dead.
//
// So every assertion here goes through the real path: a scene is built or
// parsed, adopted by the runtime, and update() is stepped. Nothing calls
// enable_destruction, register_fracture_asset, bind_destructible or
// apply_damage directly. The assertions read what that path produced — live
// shards, a missing entity, objects in the render world — rather than the
// presence of an API.
//
// Headless like the sibling suite: the device exists because the Runtime
// constructor asks for one, and the whole suite NF_SKIPs when no Vulkan is
// available rather than reporting a pass that verified nothing.

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
#include <NF/Physics/PhysicsWorld.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::scene;
using namespace nf::physics;

namespace {

constexpr float kDt = 1.0f / 60.0f;

/// Floor + crate + falling ball. The crate is a static box so the engine solver
/// generates a contact when the ball lands on it, and the same static goes into
/// the debris world so a shard of one object can rest against another. The
/// crate carries no MeshComponent: a shard's geometry comes from the fracture
/// asset, and an intact crate that does not draw makes every object after the
/// blast unambiguously a shard.
///
/// The crate floats 5 above the floor so a shard has room to fall — a spawn at
/// y ~= 5 with the floor's top face at 0 is what makes the "it fell" assertion
/// below meaningful rather than a shard that cannot move before it lands.
struct DestructionScene {
    std::unique_ptr<Scene> scene;
    ecs::Entity crate{};
    ecs::Entity ball{};
};

/// @param threshold the crate's damage_threshold; 1000 makes it unbreakable.
/// @param collider_box false gives the crate a sphere so the loader path's
/// "unsupported shape" branch has coverage.
DestructionScene build_scene(f32 threshold, bool collider_box = true,
                             bool destructible_enabled = true, f32 ball_mass = 5.0f) {
    DestructionScene out;
    auto s = std::make_unique<Scene>("SceneDestruction");
    auto& w = s->world();

    ecs::Entity floor = w.create_entity();
    w.add<Transform>(floor, Transform{});
    w.get<Transform>(floor)->local_y = -1.0f;
    w.add<RigidBodyComponent>(floor, RigidBodyComponent{BodyType::Static});
    w.add<ColliderComponent>(floor, ColliderComponent{Shape::make_box(Vec3{20.0f, 1.0f, 20.0f})});

    out.crate = w.create_entity();
    Transform crate_t;
    crate_t.local_y = 5.0f;
    w.add<Transform>(out.crate, std::move(crate_t));
    w.add<RigidBodyComponent>(out.crate, RigidBodyComponent{BodyType::Static});
    w.add<ColliderComponent>(out.crate, collider_box
        ? ColliderComponent{Shape::make_box(Vec3{0.5f, 0.5f, 0.5f})}
        : ColliderComponent{Shape::make_sphere(0.5f)});
    DestructibleComponent d;
    d.damage_threshold = threshold;
    d.enabled = destructible_enabled;
    w.add<DestructibleComponent>(out.crate, d);

    // Directly above the crate's centre, so the fall and the impact point are
    // deterministic and the blast lands in the middle of the object.
    out.ball = w.create_entity();
    Transform ball_t;
    ball_t.local_y = 10.5f;
    w.add<Transform>(out.ball, std::move(ball_t));
    RigidBodyComponent rb{BodyType::Dynamic};
    rb.mass = ball_mass;
    w.add<RigidBodyComponent>(out.ball, rb);
    w.add<ColliderComponent>(out.ball, ColliderComponent{Shape::make_sphere(0.5f)});

    out.scene = std::move(s);
    return out;
}

/// Owns the device + runtime so a test body is one block and teardown order is
/// fixed (runtime before device).
struct HeadlessRuntime {
    std::filesystem::path tmp;
    VirtualFileSystem vfs;
    AssetRegistry registry;
    AssetManager manager;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<Runtime> runtime;

    HeadlessRuntime() : manager(vfs, registry) {
        tmp = std::filesystem::temp_directory_path() / "nf_runtime_scene_destruction";
        std::filesystem::create_directories(tmp);
        vfs.mount("content://", tmp);

        device = rhi::create_device();
        if (!device) return;
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

    /// True only when the runtime is live. Tests that need it NF_SKIP rather
    /// than pass on a machine with no Vulkan.
    bool ok() const { return static_cast<bool>(runtime); }
};

/// Steps update() until `predicate` holds or the frame budget runs out. The
/// budget is what makes a "never happened" assertion meaningful: a loop without
/// one cannot distinguish "did not break" from "was not given the chance".
template<typename Pred>
u32 step_until(Runtime& rt, Pred predicate, u32 max_frames) {
    for (u32 i = 0u; i < max_frames; ++i) {
        rt.update(kDt);
        if (predicate()) {
            return i + 1u;
        }
    }
    return max_frames;
}

} // namespace

NF_TEST(destructible_component_round_trips_through_scene_text) {
    // The whole component is a build spec for an asset that is never stored on
    // disk, so a save/load must reproduce the exact same fracture. A round trip
    // that loses the seed or the threshold silently changes the pile of debris
    // the scene produces.
    Scene scene("RoundTrip");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<Transform>(e, Transform{});
    DestructibleComponent d;
    d.chunks = 7u;
    d.seed = 0x0BADF00Du;
    d.strength = 40.0f;
    d.damage_threshold = 3.5f;
    d.blast_radius = 1.25f;
    d.enabled = false;
    scene.world().add<DestructibleComponent>(e, d);

    const std::string text = serialize_scene_to_text(scene);
    NF_CHECK(text.find("Destructible:") != std::string::npos);
    NF_CHECK(text.find("enabled=false") != std::string::npos);

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "nf_destructible_roundtrip.nfscene";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        NF_CHECK(out.good());
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        NF_CHECK(out.good());
    }

    SceneLoadResult loaded = load_scene_from_physical(file);
    NF_CHECK(loaded.success);
    NF_CHECK(loaded.scene);
    if (!loaded.scene) return;

    const DestructibleComponent* back = loaded.scene->world().get<DestructibleComponent>(e);
    NF_CHECK(back != nullptr);
    if (back == nullptr) return;
    NF_CHECK_EQ(back->chunks, 7u);
    NF_CHECK_EQ(back->seed, 0x0BADF00Du);
    NF_CHECK_NEAR(back->strength, 40.0f, 1e-6f);
    NF_CHECK_NEAR(back->damage_threshold, 3.5f, 1e-6f);
    NF_CHECK_NEAR(back->blast_radius, 1.25f, 1e-6f);
    NF_CHECK(!back->enabled);

    // A component written by the runtime must parse again — the writer above is
    // the one adopt_scene reads back after a save.
    const std::string again = serialize_scene_to_text(*loaded.scene);
    NF_CHECK(again == text);

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

NF_TEST(scene_destructible_is_bound_when_the_scene_is_adopted) {
    // The adoption path is what makes the feature reachable: no game code
    // called enable_destruction here, and the runtime still ends up with a
    // cooked asset and a binding.
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    DestructionScene s = build_scene(8.0f);
    env.runtime->adopt_scene(std::move(s.scene), "content://Scenes/SceneDestruction.nfscene");

    NF_CHECK(env.runtime->destruction_enabled());
    NF_CHECK_EQ(env.runtime->destructible_count(), static_cast<size_t>(1u));
    NF_CHECK_EQ(env.runtime->fracture_asset_count(), 1u);
    // The level's floor is in the debris world, so a shard lands on it.
    NF_CHECK_EQ(env.runtime->debris_static_count(), static_cast<size_t>(2u));
}

NF_TEST(scene_destructible_needs_a_box_collider) {
    // Only the box is cuttable for v0.1. A sphere collider is skipped rather
    // than substituted, because fracturing a box in place of what the player
    // sees is a silent change of shape.
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    DestructionScene s = build_scene(8.0f, /*collider_box=*/false);
    env.runtime->adopt_scene(std::move(s.scene), "content://Scenes/SceneDestruction.nfscene");

    // Nothing was bindable, so the scene must not have spun up a second physics
    // world for nothing.
    NF_CHECK(!env.runtime->destruction_enabled());
    NF_CHECK_EQ(env.runtime->destructible_count(), static_cast<size_t>(0u));
    NF_CHECK_EQ(env.runtime->fracture_asset_count(), 0u);
}

NF_TEST(scene_destructible_disabled_is_never_bound) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    DestructionScene s = build_scene(8.0f, true, /*destructible_enabled=*/false);
    env.runtime->adopt_scene(std::move(s.scene), "content://Scenes/SceneDestruction.nfscene");

    NF_CHECK(!env.runtime->destruction_enabled());
    NF_CHECK_EQ(env.runtime->destructible_count(), static_cast<size_t>(0u));
}

NF_TEST(impact_through_update_shatters_a_scene_destructible) {
    // The Rule 0 test for this file. A ball falls onto a crate the scene
    // declared breakable; update() alone has to produce the shards, retire the
    // crate, and put the debris in the render world.
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    DestructionScene s = build_scene(8.0f);
    const ecs::Entity crate = s.crate;
    env.runtime->adopt_scene(std::move(s.scene), "content://Scenes/SceneDestruction.nfscene");
    NF_CHECK(env.runtime->edit_scene()->world().is_alive(crate));

    // ~0.96 s of free fall before contact, so the budget has to cover it.
    const u32 frames = step_until(*env.runtime,
        [&]() { return env.runtime->active_debris() > static_cast<size_t>(0u); },
        300u);
    NF_CHECK(frames > 0u);
    NF_CHECK(frames < 300u);
    NF_CHECK(env.runtime->active_debris() > static_cast<size_t>(0u));
    NF_CHECK(env.runtime->bonds_shattered_total() > 0u);

    // The object is now its shards: the entity is gone, and with it the mesh
    // and the static collider, so nothing draws or collides where the crate
    // was. This is the assertion a "retire" implementation that only cleared
    // the binding would fail.
    NF_CHECK(!env.runtime->edit_scene()->world().is_alive(crate));
    NF_CHECK_EQ(env.runtime->destructible_count(), static_cast<size_t>(0u));

    // The shards reach the render world through the same build_render_world
    // the renderer uses, so they are drawn rather than merely simulated. An
    // id of 0 means "not an entity": debris is visible but never pickable.
    const std::vector<Runtime::DebrisSample> samples = env.runtime->debris_samples();
    const rendering::RenderWorld drawn = env.runtime->render_world_snapshot();
    NF_CHECK_EQ(drawn.objects.size(), samples.size());
    bool any_shard = false;
    for (size_t i = 0u; i < drawn.objects.size(); ++i) {
        NF_CHECK_EQ(drawn.objects[i].id, 0u);
        NF_CHECK(drawn.objects[i].mesh_handle.valid());
        if (drawn.objects[i].visible) {
            any_shard = true;
        }
    }
    NF_CHECK(any_shard);

    // The shards are in the debris world, not the scene: stepping further moves
    // them, which is what proves update() drives them (see the sibling suite).
    const float first_y = samples.empty() ? 0.0f : samples.front().position.y;
    step_until(*env.runtime,
        [&]() { return env.runtime->debris_samples().front().position.y < first_y - 0.5f; },
        120u);
    NF_CHECK(env.runtime->debris_samples().front().position.y < first_y - 0.5f);
}

NF_TEST(subthreshold_impact_leaves_the_destructible_standing) {
    // The same fall against a crate whose threshold no contact can reach. The
    // ball lands, the solver generates its manifolds, and nothing breaks: a
    // threshold that never fires is what makes a slam distinguishable from a
    // stack. The frame budget is what makes this an assertion rather than a
    // guess — 180 frames is three times the fall time the breaking test needed.
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    DestructionScene s = build_scene(1000.0f);
    const ecs::Entity crate = s.crate;
    env.runtime->adopt_scene(std::move(s.scene), "content://Scenes/SceneDestruction.nfscene");

    step_until(*env.runtime, [&]() { return false; }, 180u);

    NF_CHECK(env.runtime->edit_scene()->world().is_alive(crate));
    NF_CHECK_EQ(env.runtime->active_debris(), static_cast<size_t>(0u));
    NF_CHECK_EQ(env.runtime->bonds_shattered_total(), 0u);
    NF_CHECK_EQ(env.runtime->destructible_count(), static_cast<size_t>(1u));
}

NF_TEST(shattering_a_scene_destructible_is_deterministic) {
    // Two runtimes, the same scene and the same frames: the shard ids and the
    // frame the object came apart must match. update() reads no wall clock —
    // physics is on a fixed clock, the debris world on a second one, and the
    // fracture planes come from the seed — so a difference here is a bug in
    // the ordering of the impact scan, not in the physics.
    auto run = [](HeadlessRuntime& env) {
        DestructionScene s = build_scene(8.0f);
        const ecs::Entity crate = s.crate;
        env.runtime->adopt_scene(std::move(s.scene), "content://Scenes/SceneDestruction.nfscene");
        const u32 frames = step_until(*env.runtime,
            [&]() { return env.runtime->active_debris() > static_cast<size_t>(0u); },
            300u);
        for (int i = 0; i < 30; ++i) {
            env.runtime->update(kDt);
        }
        struct Result {
            u32 frames = 0u;
            std::vector<u32> ids;
        } result;
        result.frames = frames;
        for (const Runtime::DebrisSample& sample : env.runtime->debris_samples()) {
            result.ids.push_back(sample.id);
        }
        // Retire is part of the result: a crate that only sometimes disappears
        // is a determinism bug too.
        result.ids.push_back(env.runtime->edit_scene()->world().is_alive(crate) ? 1u : 0u);
        return result;
    };

    HeadlessRuntime a;
    HeadlessRuntime b;
    if (!a.ok() || !b.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }

    const auto ra = run(a);
    const auto rb = run(b);
    NF_CHECK(ra.frames > 0u);
    NF_CHECK_EQ(ra.frames, rb.frames);
    NF_CHECK_EQ(ra.ids.size(), rb.ids.size());
    for (size_t i = 0u; i < ra.ids.size() && i < rb.ids.size(); ++i) {
        NF_CHECK_EQ(ra.ids[i], rb.ids[i]);
    }
}
