// Tests/RuntimeTests/test_runtime_destruction.cpp — does the Runtime actually
// drive destruction?
//
// The arithmetic destruction world is tested in DestructionTests and the Jolt
// sink in PhysicsTests. Neither can answer the question this file exists for:
// when a scene holds a breakable entity, does a blast issued through the
// runtime really spawn shards into a world that update() steps (Rule 0 — a
// green suite proves nothing about integration, and unreachable code is dead)?
// Every assertion below is therefore about motion and reachability, not about
// the presence of an API.
//
// Headless like the other runtime tests: destruction is pure CPU + Jolt, so the
// device is only here because the Runtime constructor asks for one. The whole
// suite skips itself when no GPU is available rather than reporting a pass that
// verified nothing.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Destruction/FractureAsset.hpp>
#include <NF/Destruction/FractureMath.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::scene;
using namespace nf::physics;
using namespace nf::destruction;

namespace {

constexpr float kDt = 1.0f / 60.0f;

/// A cube of side 2 centred at the origin: volume 8, six faces of area 4. The
/// same source the sink suite fractures, so the two suites agree about what the
/// asset looks like before it breaks.
FracturePiece unit_cube() {
    FracturePiece p;
    p.vertices = {
        Vec3{-1.0f, -1.0f, -1.0f}, Vec3{ 1.0f, -1.0f, -1.0f},
        Vec3{ 1.0f,  1.0f, -1.0f}, Vec3{-1.0f,  1.0f, -1.0f},
        Vec3{-1.0f, -1.0f,  1.0f}, Vec3{ 1.0f, -1.0f,  1.0f},
        Vec3{ 1.0f,  1.0f,  1.0f}, Vec3{-1.0f,  1.0f,  1.0f},
    };
    p.indices = {
        0u, 2u, 1u,  0u, 3u, 2u,   1u, 6u, 5u,  1u, 2u, 6u,
        5u, 7u, 4u,  5u, 6u, 7u,   4u, 3u, 0u,  4u, 7u, 3u,
        4u, 0u, 1u,  4u, 1u, 5u,   3u, 6u, 2u,  3u, 7u, 6u,
    };
    return p;
}

/// One blast strong enough to take every bond of the four-piece asset in a
/// single call, at the object's own centre.
DamageEvent centre_blast() {
    DamageEvent event;
    event.world_point = Vec3{0.0f, 5.0f, 0.0f};
    event.radius = 10.0f;
    event.impulse = 200.0f;
    return event;
}

/// A scene with a floor and one breakable crate floating above it. The floor is
/// a static box so the debris world gets a collider shards actually land on;
/// the crate carries no RigidBodyComponent, because the engine solver would
/// compete with Jolt over the same volume (see Runtime::enable_destruction).
std::unique_ptr<Scene> crate_scene() {
    auto scene = std::make_unique<Scene>("DestructionTest");
    auto& w = scene->world();

    ecs::Entity floor = w.create_entity();
    w.add<Transform>(floor, Transform{});
    w.get<Transform>(floor)->local_y = -1.0f;
    w.add<RigidBodyComponent>(floor, RigidBodyComponent{BodyType::Static});
    w.add<ColliderComponent>(floor, ColliderComponent{Shape::make_box(Vec3{20.0f, 1.0f, 20.0f})});

    ecs::Entity crate = w.create_entity();
    Transform t;
    t.local_y = 5.0f;
    w.add<Transform>(crate, std::move(t));
    return scene;
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
        tmp = std::filesystem::temp_directory_path() / "nf_runtime_destruction";
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

/// Registers a four-piece fracture asset and returns its index.
u32 register_four_piece(Runtime& runtime) {
    FractureParams params;
    params.seed = 0x5EEDBEEFu;
    params.target_chunks = 4u;
    params.strength_per_area = 1.0f;
    FractureAsset asset;
    build_fracture_asset(unit_cube(), params, asset);
    return runtime.register_fracture_asset(asset);
}

} // namespace

NF_TEST(runtime_destruction_is_off_by_default) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    env.runtime->adopt_scene(crate_scene(), "content://Scenes/Destruction.nfscene");

    NF_CHECK(!env.runtime->destruction_enabled());
    NF_CHECK_EQ(env.runtime->active_debris(), static_cast<size_t>(0u));
    NF_CHECK_EQ(env.runtime->bonds_shattered_total(), 0u);
    NF_CHECK_EQ(env.runtime->step_destruction(kDt), 0u);
    // Damage on an unbound entity in a disabled runtime is a no-op, not an
    // error — a gameplay module may fire before the feature is turned on.
    NF_CHECK_EQ(env.runtime->apply_damage(ecs::Entity{1u, 0u}, centre_blast()), 0u);
    NF_CHECK(env.runtime->debris_samples().empty());
}

NF_TEST(runtime_destruction_enabled_adds_the_scenes_statics) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    env.runtime->adopt_scene(crate_scene(), "content://Scenes/Destruction.nfscene");

    Runtime::DestructionConfig config;
    NF_CHECK(env.runtime->enable_destruction(config));
    NF_CHECK(env.runtime->destruction_enabled());
    // The floor: one static box in the debris world, so a shard lands on it
    // rather than falling to infinity. This is the collider that makes the
    // "shards came to rest" assertion below meaningful.
    NF_CHECK_EQ(env.runtime->debris_static_count(), static_cast<size_t>(1u));
    // Idempotent: enabling twice does not duplicate the level geometry.
    NF_CHECK(env.runtime->enable_destruction(config));
    NF_CHECK_EQ(env.runtime->debris_static_count(), static_cast<size_t>(1u));
}

NF_TEST(runtime_destruction_blast_spawns_shards_that_update_steps) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    env.runtime->adopt_scene(crate_scene(), "content://Scenes/Destruction.nfscene");
    NF_CHECK(env.runtime->enable_destruction(Runtime::DestructionConfig{}));

    const u32 asset_index = register_four_piece(*env.runtime);
    NF_CHECK(asset_index != kInvalidChunk);

    auto& w = env.runtime->edit_scene()->world();
    const ecs::Entity crate = w.all_entities()[1u];
    NF_CHECK(env.runtime->bind_destructible(crate, asset_index));
    NF_CHECK_EQ(env.runtime->destructible_count(), static_cast<size_t>(1u));

    // A bad index is a caller error, not a silent default.
    NF_CHECK(!env.runtime->bind_destructible(crate, kInvalidChunk));

    // The runtime reads the propagated world transform, so propagate before the
    // blast exactly like update() would have.
    scene::propagate_transforms(w);

    NF_CHECK(env.runtime->apply_damage(crate, centre_blast()) > 0u);
    NF_CHECK(env.runtime->bonds_shattered_total() > 0u);
    const std::vector<Runtime::DebrisSample> spawned = env.runtime->debris_samples();
    NF_CHECK(!spawned.empty());
    for (const Runtime::DebrisSample& s : spawned) {
        NF_CHECK(s.id != kInvalidDebris);
        // The shard spawns exactly where the object was: the hull is authored
        // in asset space and the body is placed at the asset's origin in world,
        // so a chunk whose local centroid is well off the origin (every one of
        // the four pieces here) still starts at the entity's own position. A
        // spawn offset by the chunk centroid — or by Jolt's centre-of-mass
        // position — shows up here as a y that is not 5.
        NF_CHECK_NEAR(s.position.y, 5.0f, 1e-3f);
    }

    // --- The Rule 0 assertion -------------------------------------------
    // update() must step the debris world; a shard that never moves is a shard
    // whose world is not being driven, which is the failure mode this whole
    // file exists to catch. Gravity is the probe: after a second of updates the
    // shard is either on the floor (y at the floor's top face) or still falling,
    // but it cannot still be sitting at its spawn height.
    for (int i = 0; i < 60; ++i) {
        env.runtime->update(kDt);
    }
    const std::vector<Runtime::DebrisSample> after = env.runtime->debris_samples();
    NF_CHECK_EQ(after.size(), spawned.size());
    bool any_fell = false;
    for (const Runtime::DebrisSample& s : after) {
        if (s.position.y < 5.0f - 0.5f) {
            any_fell = true;
        }
        // A static floor 20 units across: a shard that fell through it would be
        // at a large negative y. Resting on it means y ~= 0 (the floor's top
        // face) within the shard's own extent.
        NF_CHECK(s.position.y > -5.0f);
    }
    NF_CHECK(any_fell);
    NF_CHECK(env.runtime->step_destruction(0.0f) == 0u);
}

NF_TEST(runtime_destruction_shards_reach_the_render_world) {
    // A shard that is simulated but never drawn is debris the player cannot see.
    // This test asserts the emission path, not the builder: it reads the render
    // world through render_world_snapshot(), which goes through the same
    // build_render_world() render() calls, so dropping the
    // build_debris_render_world() call in there fails this test — whereas a
    // dedicated "debris objects" accessor would keep passing while the shards
    // stayed invisible, which is exactly the dead-code trap Rule 0 is about.
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    env.runtime->adopt_scene(crate_scene(), "content://Scenes/Destruction.nfscene");
    NF_CHECK(env.runtime->enable_destruction(Runtime::DestructionConfig{}));
    const u32 asset_index = register_four_piece(*env.runtime);
    auto& w = env.runtime->edit_scene()->world();
    const ecs::Entity crate = w.all_entities()[1u];
    NF_CHECK(env.runtime->bind_destructible(crate, asset_index));
    scene::propagate_transforms(w);

    // Neither the floor nor the crate carries a MeshComponent, so before the
    // blast the render world is empty — which makes every object after it a
    // shard, with no scene geometry to disambiguate.
    NF_CHECK(env.runtime->render_world_snapshot().objects.empty());

    NF_CHECK(env.runtime->apply_damage(crate, centre_blast()) > 0u);
    const std::vector<Runtime::DebrisSample> spawned = env.runtime->debris_samples();
    NF_CHECK(!spawned.empty());

    const rendering::RenderWorld drawn = env.runtime->render_world_snapshot();
    // Both lists are ordered by debris id (live_ids is ascending, samples
    // follows the sink's own live set), so index i pairs with index i.
    NF_CHECK_EQ(drawn.objects.size(), spawned.size());
    for (size_t i = 0u; i < drawn.objects.size(); ++i) {
        const rendering::RenderObject& ro = drawn.objects[i];
        const Runtime::DebrisSample& s = spawned[i];
        // id 0 is "no entity" for the picker: a shard is drawable but is never
        // a pickable object, so clicking debris reports nothing rather than a
        // crate that no longer exists.
        NF_CHECK_EQ(ro.id, 0u);
        NF_CHECK(ro.visible);
        NF_CHECK(ro.mesh_handle.valid());
        NF_CHECK(ro.material_handle.valid());
        // The pose handed to the renderer is the pose the body is actually at.
        // The world matrix's translation row is the shape-local origin in world
        // space, which is exactly what debris_samples() reports — a mismatch
        // here is the centre-of-mass/geometry-space trap from JoltWorld::state.
        NF_CHECK_NEAR(ro.world.m[3][0], s.position.x, 1e-4f);
        NF_CHECK_NEAR(ro.world.m[3][1], s.position.y, 1e-4f);
        NF_CHECK_NEAR(ro.world.m[3][2], s.position.z, 1e-4f);
        // The bounds are the shard's own local AABB swept through the shard's
        // rotation, so the point the body sits on must land inside them. The
        // extent check is what makes this catch an empty mesh: a zero-size AABB
        // degenerates to the position itself and "contains" it trivially, which
        // is exactly what a shard whose geometry landed in LOD 1 while bounds()
        // read the empty LOD 0 looked like.
        NF_CHECK(ro.bounds.max_x > ro.bounds.min_x);
        NF_CHECK(ro.bounds.max_y > ro.bounds.min_y);
        NF_CHECK(ro.bounds.max_z > ro.bounds.min_z);
        NF_CHECK(ro.bounds.contains(s.position.x, s.position.y, s.position.z));
    }

    // The render world tracks the shards, not just the blast: after a second of
    // updates the bodies have fallen and the objects carry the new poses. A
    // snapshot built once and cached would leave every shard hanging in the air.
    for (int i = 0; i < 60; ++i) {
        env.runtime->update(kDt);
    }
    const std::vector<Runtime::DebrisSample> fallen = env.runtime->debris_samples();
    const rendering::RenderWorld redrawn = env.runtime->render_world_snapshot();
    NF_CHECK_EQ(redrawn.objects.size(), fallen.size());
    bool any_fell = false;
    for (size_t i = 0u; i < redrawn.objects.size(); ++i) {
        // All three axes, not just y: the pose is rotation * translate, and a
        // swapped product still rotates the shard correctly but swings its
        // position about the world origin too. That leaves y within tolerance
        // whenever the tumble happens to be about a horizontal axis, so a
        // y-only check passes a shard that renders somewhere else entirely.
        NF_CHECK_NEAR(redrawn.objects[i].world.m[3][0], fallen[i].position.x, 1e-4f);
        NF_CHECK_NEAR(redrawn.objects[i].world.m[3][1], fallen[i].position.y, 1e-4f);
        NF_CHECK_NEAR(redrawn.objects[i].world.m[3][2], fallen[i].position.z, 1e-4f);
        if (redrawn.objects[i].world.m[3][1] < drawn.objects[i].world.m[3][1] - 0.5f) {
            any_fell = true;
        }
    }
    NF_CHECK(any_fell);
}

NF_TEST(runtime_destruction_is_deterministic_across_blasts) {
    // Two runtimes given the same scene, asset and blast must produce the same
    // shard count and the same ids. This is the runtime-level mirror of the
    // sink suite's determinism test: it also proves the fixed clock in
    // step_destruction is what makes the trajectories reproducible, since the
    // two runtimes never see a wall clock.
    HeadlessRuntime a;
    HeadlessRuntime b;
    if (!a.ok() || !b.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }

    auto run = [](HeadlessRuntime& env) -> std::vector<u32> {
        env.runtime->adopt_scene(crate_scene(), "content://Scenes/Destruction.nfscene");
        env.runtime->enable_destruction(Runtime::DestructionConfig{});
        const u32 asset_index = register_four_piece(*env.runtime);
        auto& w = env.runtime->edit_scene()->world();
        const ecs::Entity crate = w.all_entities()[1u];
        env.runtime->bind_destructible(crate, asset_index);
        scene::propagate_transforms(w);
        env.runtime->apply_damage(crate, centre_blast());
        for (int i = 0; i < 30; ++i) {
            env.runtime->update(kDt);
        }
        std::vector<u32> ids;
        for (const Runtime::DebrisSample& s : env.runtime->debris_samples()) {
            ids.push_back(s.id);
        }
        return ids;
    };

    const std::vector<u32> ids_a = run(a);
    const std::vector<u32> ids_b = run(b);
    NF_CHECK(!ids_a.empty());
    NF_CHECK_EQ(ids_a.size(), ids_b.size());
    for (size_t i = 0u; i < ids_a.size() && i < ids_b.size(); ++i) {
        NF_CHECK_EQ(ids_a[i], ids_b[i]);
    }
}

NF_TEST(runtime_destruction_scene_change_clears_debris) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    env.runtime->adopt_scene(crate_scene(), "content://Scenes/Destruction.nfscene");
    NF_CHECK(env.runtime->enable_destruction(Runtime::DestructionConfig{}));
    const u32 asset_index = register_four_piece(*env.runtime);
    auto& w = env.runtime->edit_scene()->world();
    const ecs::Entity crate = w.all_entities()[1u];
    env.runtime->bind_destructible(crate, asset_index);
    scene::propagate_transforms(w);
    env.runtime->apply_damage(crate, centre_blast());
    NF_CHECK(env.runtime->active_debris() > static_cast<size_t>(0u));

    // A new scene retires the previous scene's shards and drops its bindings:
    // debris from a level that no longer exists cannot be allowed to fly
    // through the next one. The registered assets survive — the game owns those.
    env.runtime->adopt_scene(crate_scene(), "content://Scenes/Destruction2.nfscene");
    NF_CHECK_EQ(env.runtime->active_debris(), static_cast<size_t>(0u));
    NF_CHECK_EQ(env.runtime->destructible_count(), static_cast<size_t>(0u));
    NF_CHECK_EQ(env.runtime->fracture_asset_count(), 1u);
    NF_CHECK(env.runtime->destruction_enabled());
    // The new scene's floor is in the debris world, not the old one's.
    NF_CHECK_EQ(env.runtime->debris_static_count(), static_cast<size_t>(1u));
}

NF_TEST(runtime_destruction_disable_retires_everything) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no headless Vulkan device available");
    }
    env.runtime->adopt_scene(crate_scene(), "content://Scenes/Destruction.nfscene");
    NF_CHECK(env.runtime->enable_destruction(Runtime::DestructionConfig{}));
    const u32 asset_index = register_four_piece(*env.runtime);
    auto& w = env.runtime->edit_scene()->world();
    const ecs::Entity crate = w.all_entities()[1u];
    env.runtime->bind_destructible(crate, asset_index);
    scene::propagate_transforms(w);
    env.runtime->apply_damage(crate, centre_blast());
    NF_CHECK(env.runtime->active_debris() > static_cast<size_t>(0u));

    env.runtime->disable_destruction();
    NF_CHECK(!env.runtime->destruction_enabled());
    NF_CHECK_EQ(env.runtime->active_debris(), static_cast<size_t>(0u));
    NF_CHECK(env.runtime->debris_samples().empty());
    NF_CHECK_EQ(env.runtime->step_destruction(kDt), 0u);
    // Re-enabling starts clean rather than resurrecting retired shards.
    NF_CHECK(env.runtime->enable_destruction(Runtime::DestructionConfig{}));
    NF_CHECK_EQ(env.runtime->active_debris(), static_cast<size_t>(0u));
}
