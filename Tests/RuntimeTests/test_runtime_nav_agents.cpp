// Tests/RuntimeTests/test_runtime_nav_agents.cpp — Phase 28 (navigation)
//
// Rule 0 for navigation. `nf::ai::NavMesh` is unit-tested in AITests — the
// voxel pipeline, the walkable classification, the polygon cover, the links
// and the A* all have their own suite, CPU-only. What that suite cannot see is
// whether a shipped game can use any of it: no scene could name a volume, no
// frame stepped an agent. This file is the integration half, and it follows
// the Phase 25 pattern exactly — a scene that merely DECLARES the components
// ends up baked and walked, with nothing driving the seam by hand.
//
// Headless like the destruction reachability suite: NF_SKIP when no Vulkan
// device is available (the Runtime constructs a renderer on adopt).

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Runtime/SceneValidator.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/AI/NavMesh.hpp>
#include <NF/Scripting/ScriptEngine.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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
        tmp = nf::test::temp_root() / "nf_runtime_nav_agents";
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

/// A volume covering [0,12]x[0,8]x[0,12] with the floor at y = 0, and one agent
/// at (1,0,1) walking to (10,0,10). The canonical fixture: one region, a
/// straight path, an observable arrival.
void add_nav_scene(Scene& s, bool enable = true, f32 volume_z0 = 0.0f) {
    ecs::Entity volume = s.world().create_entity();
    Transform vt{};
    vt.local_z = volume_z0;
    s.world().add<Transform>(volume, vt);
    NavMeshComponent nm;
    nm.area_x = 12.0f;
    nm.area_y = 8.0f;
    nm.area_z = 12.0f;
    nm.cell_size = 0.5f;
    nm.enabled = enable;
    s.world().add<NavMeshComponent>(volume, nm);

    ecs::Entity walker = s.world().create_entity();
    Transform wt{};
    wt.local_x = 1.0f;
    wt.local_z = 1.0f;
    s.world().add<Transform>(walker, wt);
    NavAgentComponent na;
    na.speed = 3.0f;
    na.goal_x = 10.0f;
    na.goal_z = 10.0f;
    s.world().add<NavAgentComponent>(walker, na);
}

f32 path_distance(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    // Maximum XZ mismatch between two paths of the same length — the
    // determinism measure (AITests pins the mesh itself bit-identically; this
    // pins the route an agent actually walks).
    if (a.size() != b.size() || a.empty()) {
        return std::numeric_limits<float>::max();
    }
    f32 worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        const f32 dx = a[i].x - b[i].x;
        const f32 dz = a[i].z - b[i].z;
        worst = std::max(worst, std::sqrt(dx * dx + dz * dz));
    }
    return worst;
}

ecs::Entity find_walker(Scene& s) {
    for (ecs::Entity e : s.world().all_entities()) {
        if (s.world().has<NavAgentComponent>(e)) {
            return e;
        }
    }
    return ecs::kInvalidEntity;
}

/// Adopts a route scene and returns the walker. `start` is where the walker
/// stands; the volume is the canonical 12x8x12 flat fixture from
/// add_nav_scene(). Speed is high so a leg completes in a handful of frames and
/// a test can spend its budget observing the route policy instead of walking.
ecs::Entity add_route_scene(Runtime& rt, const NavRouteComponent& route, f32 speed = 50.0f,
                            f32 start_x = 1.0f, f32 start_z = 1.0f) {
    auto s = std::make_unique<Scene>("Route");
    add_nav_scene(*s);
    ecs::Entity walker = find_walker(*s);
    if (walker.valid()) {
        auto* t = s->world().get<Transform>(walker);
        if (t != nullptr) {
            t->local_x = start_x;
            t->local_z = start_z;
        }
        auto* agent = s->world().get<NavAgentComponent>(walker);
        if (agent != nullptr) {
            agent->speed = speed;
        }
        s->world().add<NavRouteComponent>(walker, route);
    }
    rt.adopt_scene(std::move(s), "content://Scenes/Route.nfscene");
    return walker;
}

/// A patrol scene with a sensor (Phase 65): the walker on the canonical volume,
/// a route, an enabled Perception, and one hostile target at (x, 0, z). The
/// walker is Blue and the target Red, so the faction filter sees a threat; the
/// target has hit points, so it is a cast member and a corpse would stop
/// counting. Returns both entities for the test to move or destroy.
struct PerceptionRouteScene {
    ecs::Entity walker = ecs::kInvalidEntity;
    ecs::Entity target = ecs::kInvalidEntity;
};

PerceptionRouteScene add_perception_route_scene(Runtime& rt, const NavRouteComponent& route,
                                                const PerceptionComponent& perception,
                                                f32 target_x, f32 target_z,
                                                f32 volume_z0 = 0.0f) {
    auto s = std::make_unique<Scene>("HoldRoute");
    add_nav_scene(*s, true, volume_z0);
    ecs::Entity walker = find_walker(*s);
    if (walker.valid()) {
        if (auto* t = s->world().get<Transform>(walker); t != nullptr) {
            t->local_x = 1.0f;
            t->local_z = 1.0f;
        }
        if (auto* agent = s->world().get<NavAgentComponent>(walker); agent != nullptr) {
            agent->speed = 4.0f;
        }
        s->world().add<NavRouteComponent>(walker, route);
        FactionComponent faction;
        faction.team = "Blue";
        s->world().add<FactionComponent>(walker, faction);
        s->world().add<PerceptionComponent>(walker, perception);
    }

    ecs::Entity target = s->world().create_entity();
    Transform tt{};
    tt.local_x = target_x;
    tt.local_z = target_z;
    s->world().add<Transform>(target, tt);
    FactionComponent target_faction;
    target_faction.team = "Red";
    s->world().add<FactionComponent>(target, target_faction);
    s->world().add<HealthComponent>(target, HealthComponent{});

    rt.adopt_scene(std::move(s), "content://Scenes/HoldRoute.nfscene");
    return PerceptionRouteScene{walker, target};
}

std::vector<u32> collect_route_indices(Runtime& rt, ecs::Entity walker, int frames) {
    std::vector<u32> seq;
    for (int i = 0; i < frames; ++i) {
        rt.update(kDt);
        u32 index = 0;
        u32 count = 0;
        f32 wait = 0.0f;
        bool finished = false;
        if (rt.nav_route_state(walker, index, count, wait, finished) &&
            (seq.empty() || seq.back() != index)) {
            seq.push_back(index);
        }
    }
    return seq;
}

/// Is `[first, last)` a contiguous subsequence of `seq`?
bool contains_run(const std::vector<u32>& seq, const std::vector<u32>& run) {
    if (run.empty() || seq.size() < run.size()) {
        return false;
    }
    for (usize i = 0; i + run.size() <= seq.size(); ++i) {
        bool all = true;
        for (usize j = 0; j < run.size(); ++j) {
            if (seq[i + j] != run[j]) {
                all = false;
                break;
            }
        }
        if (all) {
            return true;
        }
    }
    return false;
}

} // namespace

// --- Scene format ------------------------------------------------------------

NF_TEST(navmesh_and_agent_lines_round_trip_through_scene_text) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_nav_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("NavRoundTrip");
    ecs::Entity v = scene.world().create_entity();
    scene.world().add<Transform>(v, Transform{});
    NavMeshComponent nm;
    nm.area_x = 16.0f;
    nm.area_y = 6.0f;
    nm.area_z = 9.5f;
    nm.cell_size = 0.75f;
    nm.walkable_climb = 0.35f;
    nm.max_verts_per_poly = 5;
    nm.enabled = false;
    scene.world().add<NavMeshComponent>(v, nm);

    ecs::Entity w = scene.world().create_entity();
    scene.world().add<Transform>(w, Transform{});
    NavAgentComponent na;
    na.speed = 5.5f;
    na.goal_x = -3.0f;
    na.goal_y = 0.0f;
    na.goal_z = 7.25f;
    na.arrive_radius = 0.5f;
    scene.world().add<NavAgentComponent>(w, na);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/n.nfscene", scene, err));
    // Phase 63/64 are opt-in: a scene that authored neither a Terrain line nor
    // a patrol route writes neither, so its text stays what it was.
    const std::string saved = serialize_scene_to_text(scene);
    NF_CHECK(saved.find("Terrain:") == std::string::npos);
    NF_CHECK(saved.find("NavRoute:") == std::string::npos);
    NF_CHECK(saved.find("NavPoint:") == std::string::npos);
    auto result = load_scene_from_vfs(vfs, "content://Scenes/n.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.warnings.empty());

    const auto* nm2 = result.scene->world().get<NavMeshComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(nm2 != nullptr);
    if (nm2 != nullptr) {
        NF_CHECK_NEAR(nm2->area_x, 16.0f, 1e-5f);
        NF_CHECK_NEAR(nm2->area_y, 6.0f, 1e-5f);
        NF_CHECK_NEAR(nm2->area_z, 9.5f, 1e-5f);
        NF_CHECK_NEAR(nm2->cell_size, 0.75f, 1e-5f);
        NF_CHECK_NEAR(nm2->walkable_climb, 0.35f, 1e-5f);
        NF_CHECK_EQ(nm2->max_verts_per_poly, 5u);
        NF_CHECK(!nm2->enabled);
    }
    const auto* na2 = result.scene->world().get<NavAgentComponent>(
        result.scene->world().all_entities()[1]);
    NF_CHECK(na2 != nullptr);
    if (na2 != nullptr) {
        NF_CHECK_NEAR(na2->speed, 5.5f, 1e-5f);
        NF_CHECK_NEAR(na2->goal_x, -3.0f, 1e-5f);
        NF_CHECK_NEAR(na2->goal_z, 7.25f, 1e-5f);
        NF_CHECK_NEAR(na2->arrive_radius, 0.5f, 1e-5f);
    }
    std::filesystem::remove_all(tmp);
}

NF_TEST(navmesh_line_rejects_invalid_values) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_nav_bad";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    // A zero cell size is a divide-by-zero in the voxeliser, so the whole line
    // is refused rather than clamped — same contract as the light lines.
    const std::string text =
        "# SANAD Scene v1\n"
        "version: 1\n"
        "name: Bad\n"
        "entity_count: 2\n"
        "---\n"
        "entity: 0:0\n"
        "  Name: Volume\n"
        "  NavMesh: area(12,8,12) cell=0.0\n"
        "---\n"
        "entity: 1:0\n"
        "  Name: Walker\n"
        "  NavAgent: speed=3 goal(10,0,10) arrive=0.25\n";
    NF_CHECK(vfs.write_text("content://Scenes/bad.nfscene", text).ok);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/bad.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene->world().query<NavMeshComponent>().empty());
    // The agent line on the OTHER entity is unaffected: one bad line is one
    // warning, not a scene that fails to load.
    NF_CHECK_EQ(result.scene->world().query<NavAgentComponent>().size(), static_cast<size_t>(1));
    NF_CHECK(!result.warnings.empty());
    std::filesystem::remove_all(tmp);
}

// --- The frame walks the agent ----------------------------------------------

NF_TEST(scene_navmesh_is_baked_on_adopt_and_agent_arrives) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Patrol");
    add_nav_scene(*s);

    env.runtime->adopt_scene(std::move(s), "content://Scenes/Patrol.nfscene");
    // The bake: one region over the flat floor, polygons covering it, and the
    // agent bound. Zero polygons would mean the volume was never voxelised.
    NF_CHECK(env.runtime->navmesh_built());
    NF_CHECK(env.runtime->navmesh_polygon_count() > 0u);
    NF_CHECK_EQ(env.runtime->nav_agent_count(), static_cast<size_t>(1));

    ecs::Entity walker = ecs::kInvalidEntity;
    for (ecs::Entity e : env.runtime->scene()->world().all_entities()) {
        if (env.runtime->scene()->world().has<NavAgentComponent>(e)) {
            walker = e;
            break;
        }
    }
    NF_CHECK(walker.valid());

    const auto* t0 = env.runtime->scene()->world().get<Transform>(walker);
    NF_CHECK(t0 != nullptr);
    const f32 start_x = t0 != nullptr ? t0->local_x : 0.0f;

    // 12*sqrt(2) metres at 3 m/s: ~5.7 seconds. Budgeted well past that, so a
    // failure is "the agent never walked" and not "it was not given time".
    for (int i = 0; i < 600; ++i) {
        env.runtime->update(kDt);
    }

    const auto* t1 = env.runtime->scene()->world().get<Transform>(walker);
    NF_CHECK(t1 != nullptr);
    if (t1 != nullptr) {
        // The goal is (10,0,10): the agent ends up there, not somewhere near
        // it. A path that stopped short would leave the arrival observable at
        // zero even though the transform moved.
        NF_CHECK_NEAR(t1->local_x, 10.0f, 0.25f);
        NF_CHECK_NEAR(t1->local_z, 10.0f, 0.25f);
        NF_CHECK(t1->local_x > start_x + 5.0f);
    }
    NF_CHECK_EQ(env.runtime->nav_agents_at_goal(), static_cast<size_t>(1));
}

NF_TEST(agent_paths_around_an_obstacle_and_stays_on_the_mesh) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Detour");
    add_nav_scene(*s);
    // A static box wall straight across the middle of the volume: the only way
    // from (1,0,1) to (10,0,10) is around it. Without the obstacles pass the
    // agent would walk straight through and land exactly on the goal.
    ecs::Entity wall = s->world().create_entity();
    Transform wt{};
    wt.local_x = 6.0f;
    wt.local_z = 6.0f;
    s->world().add<Transform>(wall, wt);
    physics::RigidBodyComponent rb;
    rb.type = physics::BodyType::Static;
    s->world().add<physics::RigidBodyComponent>(wall, rb);
    physics::ColliderComponent col;
    col.shape = physics::Shape::make_box(Vec3{3.0f, 1.0f, 0.25f});
    s->world().add<physics::ColliderComponent>(wall, col);

    env.runtime->adopt_scene(std::move(s), "content://Scenes/Detour.nfscene");
    NF_CHECK(env.runtime->navmesh_built());
    // The wall is an obstacle: one region split by it means the bake read the
    // scene's colliders (the whole point of the obstacles pass).
    NF_CHECK(env.runtime->navmesh_obstacle_count() >= 1u);

    ecs::Entity walker = ecs::kInvalidEntity;
    for (ecs::Entity e : env.runtime->scene()->world().all_entities()) {
        if (env.runtime->scene()->world().has<NavAgentComponent>(e)) {
            walker = e;
            break;
        }
    }
    NF_CHECK(walker.valid());

    // Walk the full budget to the goal: the detour is the whole point, so the
    // agent must actually arrive, not just cross the wall's x line.
    for (int i = 0; i < 900; ++i) {
        env.runtime->update(kDt);
    }
    const auto* t1 = env.runtime->scene()->world().get<Transform>(walker);
    NF_CHECK(t1 != nullptr);
    if (t1 != nullptr) {
        NF_CHECK_NEAR(t1->local_x, 10.0f, 0.25f);
        NF_CHECK_NEAR(t1->local_z, 10.0f, 0.25f);
    }
    NF_CHECK_EQ(env.runtime->nav_agents_at_goal(), static_cast<size_t>(1));
}

NF_TEST(scene_without_nav_lines_has_no_mesh_and_no_agents) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Quiet");
    ecs::Entity e = s->world().create_entity();
    s->world().add<Transform>(e, Transform{});

    env.runtime->adopt_scene(std::move(s), "content://Scenes/Quiet.nfscene");
    NF_CHECK(!env.runtime->navmesh_built());
    NF_CHECK_EQ(env.runtime->navmesh_polygon_count(), static_cast<size_t>(0));
    NF_CHECK_EQ(env.runtime->nav_agent_count(), static_cast<size_t>(0));
    // update() must be a no-op for navigation, not a crash or a bake.
    env.runtime->update(kDt);
    NF_CHECK_EQ(env.runtime->navmesh_polygon_count(), static_cast<size_t>(0));
}

NF_TEST(disabled_agent_and_volume_never_bind_or_walk) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Posed");
    ecs::Entity volume = s->world().create_entity();
    s->world().add<Transform>(volume, Transform{});
    NavMeshComponent nm;
    nm.enabled = false; // the volume is placed but never baked
    s->world().add<NavMeshComponent>(volume, nm);
    add_nav_scene(*s, /*enable=*/true);

    // The second (enabled) volume is the one that bakes; the disabled one is
    // decoration the author keeps placed.
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Posed.nfscene");
    NF_CHECK(env.runtime->navmesh_built());

    ecs::Entity walker = ecs::kInvalidEntity;
    for (ecs::Entity e : env.runtime->scene()->world().all_entities()) {
        if (env.runtime->scene()->world().has<NavAgentComponent>(e)) {
            walker = e;
            break;
        }
    }
    NF_CHECK(walker.valid());

    // Now switch the AGENT off: it must hold its placement forever.
    auto* agent = env.runtime->edit_scene()->world().get<NavAgentComponent>(walker);
    NF_CHECK(agent != nullptr);
    if (agent != nullptr) {
        agent->enabled = false;
    }
    const auto* before = env.runtime->scene()->world().get<Transform>(walker);
    const f32 bx = before != nullptr ? before->local_x : 0.0f;
    for (int i = 0; i < 120; ++i) {
        env.runtime->update(kDt);
    }
    const auto* after = env.runtime->scene()->world().get<Transform>(walker);
    NF_CHECK(after != nullptr);
    if (after != nullptr) {
        NF_CHECK_NEAR(after->local_x, bx, 1e-6f);
    }
    NF_CHECK_EQ(env.runtime->nav_agents_at_goal(), static_cast<size_t>(0));
}

NF_TEST(two_runs_of_one_scene_walk_the_same_path) {
    // The engine's determinism contract, at the integration level: two runtimes
    // over two identical scenes must put the walker in the same place on every
    // frame. AITests pins the mesh bit-identically; this pins the route.
    std::vector<Vec3> trace[2];
    for (int run = 0; run < 2; ++run) {
        HeadlessRuntime env;
        if (!env.ok()) {
            NF_SKIP("no Vulkan device");
            return;
        }
        auto s = std::make_unique<Scene>("Repeatable");
        add_nav_scene(*s);
        env.runtime->adopt_scene(std::move(s), "content://Scenes/Repeatable.nfscene");
        ecs::Entity walker = ecs::kInvalidEntity;
        for (ecs::Entity e : env.runtime->scene()->world().all_entities()) {
            if (env.runtime->scene()->world().has<NavAgentComponent>(e)) {
                walker = e;
                break;
            }
        }
        if (!walker.valid()) {
            return;
        }
        for (int i = 0; i < 240; ++i) {
            env.runtime->update(kDt);
            const auto* t = env.runtime->scene()->world().get<Transform>(walker);
            if (t != nullptr) {
                trace[run].push_back(Vec3{t->local_x, t->local_y, t->local_z});
            }
        }
    }
    NF_CHECK_EQ(trace[0].size(), trace[1].size());
    NF_CHECK(!trace[0].empty());
    NF_CHECK(path_distance(trace[0], trace[1]) < 1e-6f);
}

NF_TEST(edit_freeze_holds_the_agent_still) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Authored");
    add_nav_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Authored.nfscene");

    ecs::Entity walker = ecs::kInvalidEntity;
    for (ecs::Entity e : env.runtime->scene()->world().all_entities()) {
        if (env.runtime->scene()->world().has<NavAgentComponent>(e)) {
            walker = e;
            break;
        }
    }
    NF_CHECK(walker.valid());
    const auto* before = env.runtime->scene()->world().get<Transform>(walker);
    const f32 bx = before != nullptr ? before->local_x : 0.0f;
    const f32 bz = before != nullptr ? before->local_z : 0.0f;

    // dt = 0 is the editor's edit mode: nothing simulates, the viewport shows
    // a posed world. An agent that walked here would make an unplayed scene
    // move itself.
    for (int i = 0; i < 30; ++i) {
        env.runtime->update(0.0f);
    }
    const auto* after = env.runtime->scene()->world().get<Transform>(walker);
    NF_CHECK(after != nullptr);
    if (after != nullptr) {
        NF_CHECK_NEAR(after->local_x, bx, 1e-6f);
        NF_CHECK_NEAR(after->local_z, bz, 1e-6f);
    }
    NF_CHECK_EQ(env.runtime->nav_agents_at_goal(), static_cast<size_t>(0));
}

NF_TEST(nav_debug_snapshot_empty_without_a_volume) {
    // No NavMesh line: no bake, no agents, and the snapshot must be empty —
    // the overlay draws nothing rather than a stale previous scene's mesh.
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Quiet");
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Quiet.nfscene");
    env.runtime->update(kDt);

    NF_CHECK(!env.runtime->navmesh_built());
    const NavDebugSnapshot snap = env.runtime->nav_debug_snapshot();
    NF_CHECK(!snap.has_mesh);
    NF_CHECK(snap.mesh_polygons.empty());
    NF_CHECK(snap.agents.empty());
}

NF_TEST(nav_debug_snapshot_matches_the_bake_before_any_step) {
    // Adopted but never stepped (the editor's edit freeze): the mesh is there
    // to draw, the agent is posed with a goal but no route yet.
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Authored");
    add_nav_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Authored.nfscene");

    const NavDebugSnapshot snap = env.runtime->nav_debug_snapshot();
    NF_CHECK(snap.has_mesh);
    NF_CHECK_EQ(snap.mesh_polygons.size(), env.runtime->navmesh_polygon_count());
    NF_CHECK(!snap.mesh_polygons.empty());
    NF_CHECK_EQ(snap.agents.size(), static_cast<size_t>(1));
    const NavDebugAgent& a = snap.agents[0];
    NF_CHECK_NEAR(a.position.x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(a.position.z, 1.0f, 1e-6f);
    NF_CHECK_NEAR(a.goal.x, 10.0f, 1e-6f);
    NF_CHECK_NEAR(a.goal.z, 10.0f, 1e-6f);
    // Never stepped: planned by nobody, so no route — the overlay draws the
    // posed agent and its goal ring, but no path.
    NF_CHECK(!a.has_path);
    NF_CHECK(a.path.empty());
    NF_CHECK(!a.at_goal);
}

NF_TEST(nav_debug_snapshot_carries_the_live_route_while_walking) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Patrol");
    add_nav_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Patrol.nfscene");

    for (int i = 0; i < 30; ++i) {
        env.runtime->update(kDt);
    }
    const NavDebugSnapshot snap = env.runtime->nav_debug_snapshot();
    NF_CHECK(snap.has_mesh);
    NF_CHECK_EQ(snap.agents.size(), static_cast<size_t>(1));
    const NavDebugAgent& a = snap.agents[0];
    // Thirty steps at 3 m/s: under way but not arrived (the full walk is
    // ~5.7 s), so the route exists and still has legs in it.
    NF_CHECK(a.has_path);
    NF_CHECK(a.path.size() >= 2u);
    NF_CHECK(!a.at_goal);
    // The route ends at the authored goal, and the walker has left its start.
    const Vec3& last = a.path.back();
    NF_CHECK_NEAR(last.x, 10.0f, 0.30f);
    NF_CHECK_NEAR(last.z, 10.0f, 0.30f);
    NF_CHECK(a.position.x > 1.0f + 0.5f);
    // Same order every read: the overlay must not flicker between agents.
    const NavDebugSnapshot again = env.runtime->nav_debug_snapshot();
    NF_CHECK_EQ(again.agents.size(), snap.agents.size());
    for (size_t i = 0; i < snap.agents.size(); ++i) {
        NF_CHECK(again.agents[i].entity == snap.agents[i].entity);
    }
}

NF_TEST(nav_debug_edge_points_cover_every_polygon_edge_once_per_owner) {
    // The pure enumerator the overlay strokes: two endpoints per edge, in
    // polygon order, shared edges once per owning polygon.
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Authored");
    add_nav_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Authored.nfscene");
    NF_CHECK(env.runtime->navmesh_built());

    const ai::NavMesh* mesh = env.runtime->navmesh();
    NF_CHECK(mesh != nullptr);
    if (mesh == nullptr) {
        return;
    }
    const std::vector<Vec3> edges = nav_debug_mesh_edge_points(*mesh);
    size_t want = 0;
    for (u32 i = 0; i < mesh->polygon_count(); ++i) {
        want += mesh->polygon(i).verts.size();
    }
    NF_CHECK(!edges.empty());
    NF_CHECK_EQ(edges.size(), want * 2u);
    // Every polygon's first vertex opens an edge: no polygon is skipped.
    for (u32 i = 0; i < mesh->polygon_count(); ++i) {
        const Vec3& v0 = mesh->polygon(i).verts[0];
        bool found = false;
        for (size_t e = 0; e < edges.size(); e += 2) {
            if (edges[e].nearly_equals(v0, 1e-6f)) {
                found = true;
                break;
            }
        }
        NF_CHECK(found);
    }
}

// --- Patrol routes (Phase 64) ------------------------------------------------

NF_TEST(nav_route_lines_round_trip_and_omit_an_inherited_wait) {
    Scene s("RouteRoundTrip");
    ecs::Entity v = s.world().create_entity();
    s.world().add<Transform>(v, Transform{});
    NavMeshComponent nm;
    nm.area_x = 12.0f;
    nm.area_z = 12.0f;
    s.world().add<NavMeshComponent>(v, nm);

    ecs::Entity w = s.world().create_entity();
    s.world().add<Transform>(w, Transform{});
    s.world().add<NavAgentComponent>(w, NavAgentComponent{});
    NavRouteComponent route;
    route.wait = 0.5f;
    route.loop = false;
    route.ping_pong = true;
    route.enabled = false;
    route.points.push_back(NavPointSpec{2.0f, 0.0f, 2.0f, -1.0f});
    route.points.push_back(NavPointSpec{8.0f, 0.0f, 8.0f, 1.25f});
    s.world().add<NavRouteComponent>(w, route);

    const std::string text = serialize_scene_to_text(s);
    NF_CHECK(text.find("NavRoute:") != std::string::npos);
    NF_CHECK(text.find("NavPoint: pos=2,0,2\n") != std::string::npos);
    // The inherited (-1) wait is an absent key, not a negative number: the
    // scene says what it means.
    NF_CHECK(text.find("NavPoint: pos=8,0,8 wait=1.25") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    const NavRouteComponent* back = nullptr;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        if (const auto* found = loaded.scene->world().get<NavRouteComponent>(e)) {
            back = found;
        }
    }
    NF_CHECK(back != nullptr);
    if (back == nullptr) {
        return;
    }
    NF_CHECK_NEAR(back->wait, 0.5f, 1e-5f);
    NF_CHECK(!back->loop);
    NF_CHECK(back->ping_pong);
    NF_CHECK(!back->enabled);
    NF_CHECK_EQ(back->points.size(), static_cast<usize>(2));
    if (back->points.size() == 2) {
        NF_CHECK_NEAR(back->points[0].x, 2.0f, 1e-5f);
        NF_CHECK_NEAR(back->points[0].wait, -1.0f, 1e-5f);
        NF_CHECK_NEAR(back->points[1].z, 8.0f, 1e-5f);
        NF_CHECK_NEAR(back->points[1].wait, 1.25f, 1e-5f);
    }

    // A bare declaration keeps the documented defaults: closed patrol, no
    // reversal, no pause, enabled.
    const std::string bare =
        "# SANAD Scene v1\nversion: 1\nname: BareRoute\nentity_count: 1\n---\n"
        "entity: 1:0\n  Name: Walker\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavRoute:\n"
        "  NavPoint: pos=1,0,1\n"
        "  NavPoint: pos=4,0,4\n";
    const SceneLoadResult plain = load_scene_from_text(bare);
    NF_CHECK(plain.success);
    const auto* plain_route =
        plain.scene->world().get<NavRouteComponent>(plain.scene->world().all_entities()[0]);
    NF_CHECK(plain_route != nullptr);
    if (plain_route != nullptr) {
        NF_CHECK_NEAR(plain_route->wait, 0.0f, 1e-6f);
        NF_CHECK(plain_route->loop);
        NF_CHECK(!plain_route->ping_pong);
        NF_CHECK(plain_route->enabled);
        NF_CHECK_EQ(plain_route->points.size(), static_cast<usize>(2));
    }
}

NF_TEST(nav_route_lines_refuse_bad_values_by_name) {
    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: BadRoute\nentity_count: 1\n---\n"
        "entity: 1:0\n  Name: Walker\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavAgent: speed=3 goal(0,0,0)\n"
        "  NavRoute: wait=-2\n"
        "  NavPoint: pos=1,2\n";
    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    bool bad_wait = false;
    bool bad_pos = false;
    for (const std::string& w : loaded.warnings) {
        if (w.find("NavRoute line with invalid 'wait='") != std::string::npos) {
            bad_wait = true;
        }
        if (w.find("NavPoint line with invalid 'pos'") != std::string::npos) {
            bad_pos = true;
        }
    }
    NF_CHECK(bad_wait);
    NF_CHECK(bad_pos);
    // Neither refused line invents a component: the failure is one warning,
    // not a half-authored route.
    NF_CHECK(loaded.scene->world().query<NavRouteComponent>().empty());
}

NF_TEST(patrol_route_waits_then_stops_at_its_last_point) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    NavRouteComponent route;
    route.wait = 0.0f; // point 0 overrides this with a real pause
    route.loop = false;
    route.ping_pong = false;
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 4.0f, 2.0f});
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 7.0f, 0.0f});
    const ecs::Entity walker = add_route_scene(*env.runtime, route);
    NF_CHECK(walker.valid());
    if (!walker.valid()) {
        return;
    }

    // Ten frames at 50 m/s: the 3 m leg is done, and the two-second pause is
    // running. A patrolling guard counts as "at goal" while it waits.
    for (int i = 0; i < 10; ++i) {
        env.runtime->update(kDt);
    }
    u32 index = 0;
    u32 count = 0;
    f32 wait = 0.0f;
    bool finished = false;
    NF_CHECK(env.runtime->nav_route_state(walker, index, count, wait, finished));
    NF_CHECK_EQ(index, 0u);
    NF_CHECK_EQ(count, 2u);
    NF_CHECK(wait > 1.5f);
    NF_CHECK(!finished);
    NF_CHECK_EQ(env.runtime->nav_agents_at_goal(), static_cast<size_t>(1));

    // Past the pause, the walker finishes the one-way route at its last point
    // and stays there: `finished` is not "reset at the next frame".
    for (int i = 0; i < 150; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(walker, index, count, wait, finished));
    NF_CHECK_EQ(index, 1u);
    NF_CHECK(finished);
    NF_CHECK_EQ(env.runtime->nav_agents_at_goal(), static_cast<size_t>(1));
    const auto* parked = env.runtime->scene()->world().get<Transform>(walker);
    NF_CHECK(parked != nullptr);
    const f32 parked_z = parked != nullptr ? parked->local_z : 0.0f;
    NF_CHECK_NEAR(parked_z, 7.0f, 0.25f);

    for (int i = 0; i < 60; ++i) {
        env.runtime->update(kDt);
    }
    const auto* still = env.runtime->scene()->world().get<Transform>(walker);
    NF_CHECK(still != nullptr);
    if (still != nullptr) {
        NF_CHECK_NEAR(still->local_z, parked_z, 1e-5f);
    }
    // The authored goal (10,10) is ignored while the route runs.
    NF_CHECK_NEAR(still != nullptr ? still->local_x : 0.0f, 1.0f, 0.25f);
}

NF_TEST(loop_route_wraps_to_the_first_point) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    NavRouteComponent route;
    route.loop = true;
    route.ping_pong = false;
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 1.0f, -1.0f});
    route.points.push_back(NavPointSpec{6.0f, 0.0f, 1.0f, -1.0f});
    route.points.push_back(NavPointSpec{6.0f, 0.0f, 6.0f, -1.0f});
    const ecs::Entity walker = add_route_scene(*env.runtime, route);
    NF_CHECK(walker.valid());
    if (!walker.valid()) {
        return;
    }

    const std::vector<u32> seq = collect_route_indices(*env.runtime, walker, 60);
    NF_CHECK(contains_run(seq, {0u, 1u, 2u, 0u}));
    u32 index = 0;
    u32 count = 0;
    f32 wait = 0.0f;
    bool finished = false;
    NF_CHECK(env.runtime->nav_route_state(walker, index, count, wait, finished));
    NF_CHECK(!finished); // a closed patrol never ends
    NF_CHECK_EQ(count, 3u);
}

NF_TEST(ping_pong_route_reverses_at_both_ends) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    NavRouteComponent route;
    // loop=true on purpose: ping-pong wins, and the route never wraps.
    route.loop = true;
    route.ping_pong = true;
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 1.0f, -1.0f});
    route.points.push_back(NavPointSpec{6.0f, 0.0f, 1.0f, -1.0f});
    route.points.push_back(NavPointSpec{6.0f, 0.0f, 6.0f, -1.0f});
    const ecs::Entity walker = add_route_scene(*env.runtime, route);
    NF_CHECK(walker.valid());
    if (!walker.valid()) {
        return;
    }

    const std::vector<u32> seq = collect_route_indices(*env.runtime, walker, 70);
    // Forward to the end, back to the start, forward again: A-B-C-B-A-B.
    NF_CHECK(contains_run(seq, {0u, 1u, 2u, 1u, 0u}));
    NF_CHECK(!contains_run(seq, {2u, 0u})); // never wraps like a loop
}

NF_TEST(one_point_route_falls_back_to_the_goal) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    NavRouteComponent route;
    route.points.push_back(NavPointSpec{2.0f, 0.0f, 9.0f, -1.0f}); // nothing to patrol
    const ecs::Entity walker = add_route_scene(*env.runtime, route, 3.0f);
    NF_CHECK(walker.valid());
    if (!walker.valid()) {
        return;
    }

    for (int i = 0; i < 600; ++i) {
        env.runtime->update(kDt);
    }
    u32 index = 0;
    u32 count = 0;
    f32 wait = 0.0f;
    bool finished = false;
    NF_CHECK(!env.runtime->nav_route_state(walker, index, count, wait, finished));
    const auto* t = env.runtime->scene()->world().get<Transform>(walker);
    NF_CHECK(t != nullptr);
    if (t != nullptr) {
        NF_CHECK_NEAR(t->local_x, 10.0f, 0.3f);
        NF_CHECK_NEAR(t->local_z, 10.0f, 0.3f);
    }
    NF_CHECK_EQ(env.runtime->nav_agents_at_goal(), static_cast<size_t>(1));
}

NF_TEST(validator_names_route_mistakes) {
    const auto has = [](const ValidationReport& report, const char* needle) {
        for (const ValidationIssue& issue : report.issues) {
            if (issue.message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    Scene s("RouteWarnings");
    ecs::Entity volume = s.world().create_entity();
    s.world().add<Transform>(volume, Transform{});
    NavMeshComponent nm;
    nm.area_x = 10.0f;
    nm.area_y = 5.0f;
    nm.area_z = 10.0f;
    s.world().add<NavMeshComponent>(volume, nm);

    // A short route with both end policies set: the two policy warnings.
    ecs::Entity walker = s.world().create_entity();
    s.world().add<Transform>(walker, Transform{});
    s.world().add<NavAgentComponent>(walker, NavAgentComponent{});
    NavRouteComponent short_route;
    short_route.loop = true;
    short_route.ping_pong = true;
    short_route.points.push_back(NavPointSpec{5.0f, 0.0f, 5.0f, -1.0f});
    s.world().add<NavRouteComponent>(walker, short_route);

    // A route with nobody to walk it and a point beyond the volume.
    ecs::Entity orphan = s.world().create_entity();
    s.world().add<Transform>(orphan, Transform{});
    NavRouteComponent lost;
    lost.points.push_back(NavPointSpec{2.0f, 0.0f, 2.0f, -1.0f});
    lost.points.push_back(NavPointSpec{30.0f, 0.0f, 2.0f, -1.0f});
    s.world().add<NavRouteComponent>(orphan, lost);

    // Phase 65: a gate with no sensor can never hold, and a delay with no gate
    // can never matter. Both are author mistakes a readable line cannot show.
    ecs::Entity gated = s.world().create_entity();
    s.world().add<Transform>(gated, Transform{});
    s.world().add<NavAgentComponent>(gated, NavAgentComponent{});
    NavRouteComponent gated_route;
    gated_route.hold = NavRouteHoldPolicy::Aware;
    gated_route.points.push_back(NavPointSpec{3.0f, 0.0f, 3.0f, -1.0f});
    gated_route.points.push_back(NavPointSpec{4.0f, 0.0f, 4.0f, -1.0f});
    s.world().add<NavRouteComponent>(gated, gated_route);

    ecs::Entity delayed = s.world().create_entity();
    s.world().add<Transform>(delayed, Transform{});
    s.world().add<NavAgentComponent>(delayed, NavAgentComponent{});
    NavRouteComponent delayed_route;
    delayed_route.resume = 1.5f; // hold=off: the delay has nothing to delay
    delayed_route.points.push_back(NavPointSpec{5.0f, 0.0f, 5.0f, -1.0f});
    delayed_route.points.push_back(NavPointSpec{6.0f, 0.0f, 6.0f, -1.0f});
    s.world().add<NavRouteComponent>(delayed, delayed_route);

    // Phase 66: an investigation is the release of a hold; without a gate
    // there is no last known position, so the search can never run.
    ecs::Entity searching = s.world().create_entity();
    s.world().add<Transform>(searching, Transform{});
    s.world().add<NavAgentComponent>(searching, NavAgentComponent{});
    NavRouteComponent searching_route;
    searching_route.investigate = 2.0f; // hold=off
    searching_route.points.push_back(NavPointSpec{2.0f, 0.0f, 2.0f, -1.0f});
    searching_route.points.push_back(NavPointSpec{3.0f, 0.0f, 3.0f, -1.0f});
    s.world().add<NavRouteComponent>(searching, searching_route);

    // Phase 67: a squad shares a search; a route that never investigates has
    // nothing to share. The token is only meaningful with a search.
    ecs::Entity squad_only = s.world().create_entity();
    s.world().add<Transform>(squad_only, Transform{});
    s.world().add<NavAgentComponent>(squad_only, NavAgentComponent{});
    NavRouteComponent squad_route;
    squad_route.hold = NavRouteHoldPolicy::Aware;
    squad_route.squad = "guards"; // investigate=0
    squad_route.points.push_back(NavPointSpec{2.0f, 0.0f, 2.0f, -1.0f});
    squad_route.points.push_back(NavPointSpec{3.0f, 0.0f, 3.0f, -1.0f});
    s.world().add<NavRouteComponent>(squad_only, squad_route);

    const ValidationReport report = validate_scene(s, "test.nfscene");
    NF_CHECK(has(report, "needs at least two"));
    NF_CHECK(has(report, "ping-pong wins"));
    NF_CHECK(has(report, "without a NavAgent"));
    NF_CHECK(has(report, "outside the navigation volume"));
    NF_CHECK(has(report, "no enabled Perception line"));
    NF_CHECK(has(report, "resume= without hold="));
    NF_CHECK(has(report, "investigate= without hold="));
    NF_CHECK(has(report, "squad= without investigate="));

    // No volume at all: the route can never bind, and the report says why.
    Scene empty("NoVolume");
    ecs::Entity lonely = empty.world().create_entity();
    empty.world().add<Transform>(lonely, Transform{});
    empty.world().add<NavAgentComponent>(lonely, NavAgentComponent{});
    NavRouteComponent route;
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 1.0f, -1.0f});
    route.points.push_back(NavPointSpec{4.0f, 0.0f, 4.0f, -1.0f});
    empty.world().add<NavRouteComponent>(lonely, route);
    const ValidationReport no_mesh = validate_scene(empty, "test.nfscene");
    NF_CHECK(has(no_mesh, "no enabled navigation volume"));
}

NF_TEST(a_script_reads_the_live_patrol_cursor) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    const std::string script =
        "function update(dt)\n"
        "  local id, gen = nf.self()\n"
        "  local index, count, wait, finished = nf.route()\n"
        "  if index == nil then\n"
        "    nf.set_entity_scale(id, gen, 0.0, 0.0, 0.0)\n"
        "  else\n"
        "    nf.set_entity_scale(id, gen, index + 1.0, count,\n"
        "                        finished and 1.0 or 0.0)\n"
        "  end\n"
        "end\n";
    NF_CHECK(env.vfs.write_text("content://Scripts/route_probe.lua", script).ok);

    auto s = std::make_unique<Scene>("RouteProbe");
    add_nav_scene(*s);
    ecs::Entity walker = find_walker(*s);
    NF_CHECK(walker.valid());
    if (!walker.valid()) {
        return;
    }
    NavRouteComponent route;
    // A long pause keeps the cursor on point 1 while the script samples it.
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 4.0f, 5.0f});
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 7.0f, 0.0f});
    s->world().add<NavRouteComponent>(walker, route);
    auto* t = s->world().get<Transform>(walker);
    if (t != nullptr) {
        t->local_z = 1.0f;
    }
    auto* agent = s->world().get<NavAgentComponent>(walker);
    if (agent != nullptr) {
        agent->speed = 50.0f;
    }
    scripting::ScriptComponent script_component;
    script_component.path = "content://Scripts/route_probe.lua";
    s->world().add<scripting::ScriptComponent>(walker, script_component);

    env.runtime->adopt_scene(std::move(s), "content://Scenes/RouteProbe.nfscene");
    for (int i = 0; i < 30; ++i) {
        env.runtime->update(kDt);
    }
    const auto* probe = env.runtime->scene()->world().get<Transform>(walker);
    NF_CHECK(probe != nullptr);
    if (probe != nullptr) {
        NF_CHECK_NEAR(probe->scale_x, 1.0f, 1e-5f); // index 0 + 1
        NF_CHECK_NEAR(probe->scale_y, 2.0f, 1e-5f); // point count
        NF_CHECK_NEAR(probe->scale_z, 0.0f, 1e-5f); // not finished
    }
}

// --- Perception-gated patrols (Phase 65) ------------------------------------

NF_TEST(nav_route_hold_lines_round_trip_and_default_to_off) {
    // The gate is two keys on the line the patrol already has. A route that
    // never writes them is a Phase 64 route, byte for byte.
    NF_CHECK(std::string(nav_route_hold_name(NavRouteHoldPolicy::Off)) == "off");
    NF_CHECK(std::string(nav_route_hold_name(NavRouteHoldPolicy::Aware)) == "aware");
    NF_CHECK(std::string(nav_route_hold_name(NavRouteHoldPolicy::Sensing)) == "sensing");
    NavRouteHoldPolicy parsed = NavRouteHoldPolicy::Aware;
    NF_CHECK(nav_route_hold_from_name("sensing", parsed));
    NF_CHECK(parsed == NavRouteHoldPolicy::Sensing);
    NF_CHECK(!nav_route_hold_from_name("watching", parsed));

    Scene s("HoldRoundTrip");
    ecs::Entity v = s.world().create_entity();
    s.world().add<Transform>(v, Transform{});
    NavMeshComponent nm;
    nm.area_x = 12.0f;
    nm.area_z = 12.0f;
    s.world().add<NavMeshComponent>(v, nm);

    ecs::Entity w = s.world().create_entity();
    s.world().add<Transform>(w, Transform{});
    s.world().add<NavAgentComponent>(w, NavAgentComponent{});
    NavRouteComponent route;
    route.hold = NavRouteHoldPolicy::Sensing;
    route.resume = 1.5f;
    route.points.push_back(NavPointSpec{2.0f, 0.0f, 2.0f, -1.0f});
    route.points.push_back(NavPointSpec{8.0f, 0.0f, 8.0f, -1.0f});
    s.world().add<NavRouteComponent>(w, route);

    const std::string text = serialize_scene_to_text(s);
    NF_CHECK(text.find("hold=sensing") != std::string::npos);
    NF_CHECK(text.find("resume=1.5") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    const NavRouteComponent* back = nullptr;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        if (const auto* found = loaded.scene->world().get<NavRouteComponent>(e)) {
            back = found;
        }
    }
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK(back->hold == NavRouteHoldPolicy::Sensing);
        NF_CHECK_NEAR(back->resume, 1.5f, 1e-5f);
    }

    // A route written before this phase carries neither key: it loads as
    // `hold=off, resume=0` and serializes without inventing either.
    const std::string legacy =
        "# SANAD Scene v1\nversion: 1\nname: LegacyHold\nentity_count: 1\n---\n"
        "entity: 1:0\n  Name: Walker\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavRoute: wait=0.5\n"
        "  NavPoint: pos=1,0,1\n"
        "  NavPoint: pos=4,0,4\n";
    const SceneLoadResult plain = load_scene_from_text(legacy);
    NF_CHECK(plain.success);
    const auto* plain_route =
        plain.scene->world().get<NavRouteComponent>(plain.scene->world().all_entities()[0]);
    NF_CHECK(plain_route != nullptr);
    if (plain_route != nullptr) {
        NF_CHECK(plain_route->hold == NavRouteHoldPolicy::Off);
        NF_CHECK_NEAR(plain_route->resume, 0.0f, 1e-6f);
    }
    const std::string again = serialize_scene_to_text(*plain.scene);
    NF_CHECK(again.find("hold=") == std::string::npos);
    NF_CHECK(again.find("resume=") == std::string::npos);
}

NF_TEST(nav_route_hold_lines_refuse_bad_values_by_name) {
    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: BadHold\nentity_count: 2\n---\n"
        "entity: 1:0\n  Name: WalkerA\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavAgent: speed=3 goal(0,0,0)\n"
        "  NavRoute: hold=watching\n"
        "---\n"
        "entity: 2:0\n  Name: WalkerB\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavAgent: speed=3 goal(0,0,0)\n"
        "  NavRoute: hold=aware resume=-1\n";
    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    bool bad_hold = false;
    bool bad_resume = false;
    for (const std::string& w : loaded.warnings) {
        if (w.find("NavRoute line with invalid 'hold'") != std::string::npos) {
            bad_hold = true;
        }
        if (w.find("NavRoute line with invalid 'resume='") != std::string::npos) {
            bad_resume = true;
        }
    }
    NF_CHECK(bad_hold);
    NF_CHECK(bad_resume);
    // A refused policy does not fall back to `off`: half the line is not a
    // route, so neither entity gets a component.
    NF_CHECK(loaded.scene->world().query<NavRouteComponent>().empty());
}

// --- Last-known-position investigation (Phase 66) ---------------------------

NF_TEST(nav_route_investigate_line_round_trips_and_defaults_to_zero) {
    Scene s("InvestigateRoundTrip");
    ecs::Entity v = s.world().create_entity();
    s.world().add<Transform>(v, Transform{});
    NavMeshComponent nm;
    nm.area_x = 12.0f;
    nm.area_z = 12.0f;
    s.world().add<NavMeshComponent>(v, nm);

    ecs::Entity w = s.world().create_entity();
    s.world().add<Transform>(w, Transform{});
    s.world().add<NavAgentComponent>(w, NavAgentComponent{});
    NavRouteComponent route;
    route.hold = NavRouteHoldPolicy::Aware;
    route.resume = 0.5f;
    route.investigate = 2.5f;
    route.points.push_back(NavPointSpec{2.0f, 0.0f, 2.0f, -1.0f});
    route.points.push_back(NavPointSpec{8.0f, 0.0f, 8.0f, -1.0f});
    s.world().add<NavRouteComponent>(w, route);

    const std::string text = serialize_scene_to_text(s);
    NF_CHECK(text.find("hold=aware") != std::string::npos);
    NF_CHECK(text.find("resume=0.5") != std::string::npos);
    NF_CHECK(text.find("investigate=2.5") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    const NavRouteComponent* back = nullptr;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        if (const auto* found = loaded.scene->world().get<NavRouteComponent>(e)) {
            back = found;
        }
    }
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK(back->hold == NavRouteHoldPolicy::Aware);
        NF_CHECK_NEAR(back->resume, 0.5f, 1e-5f);
        NF_CHECK_NEAR(back->investigate, 2.5f, 1e-5f);
    }

    // A route written before this phase carries no key: it loads as zero and
    // serializes without inventing one. A Phase 65 route stays byte-identical.
    const std::string legacy =
        "# SANAD Scene v1\nversion: 1\nname: LegacyInvestigate\nentity_count: 1\n---\n"
        "entity: 1:0\n  Name: Walker\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavRoute: wait=0.5 hold=aware resume=1\n"
        "  NavPoint: pos=1,0,1\n"
        "  NavPoint: pos=4,0,4\n";
    const SceneLoadResult plain = load_scene_from_text(legacy);
    NF_CHECK(plain.success);
    const auto* plain_route =
        plain.scene->world().get<NavRouteComponent>(plain.scene->world().all_entities()[0]);
    NF_CHECK(plain_route != nullptr);
    if (plain_route != nullptr) {
        NF_CHECK_NEAR(plain_route->investigate, 0.0f, 1e-6f);
    }
    const std::string again = serialize_scene_to_text(*plain.scene);
    NF_CHECK(again.find("investigate=") == std::string::npos);
}

NF_TEST(nav_route_investigate_lines_refuse_bad_values_by_name) {
    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: BadInvestigate\nentity_count: 2\n---\n"
        "entity: 1:0\n  Name: WalkerA\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavAgent: speed=3 goal(0,0,0)\n"
        "  NavRoute: hold=aware investigate=-0.5\n"
        "---\n"
        "entity: 2:0\n  Name: WalkerB\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavAgent: speed=3 goal(0,0,0)\n"
        "  NavRoute: hold=aware investigate=4000\n";
    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    int bad_investigate = 0;
    for (const std::string& w : loaded.warnings) {
        if (w.find("NavRoute line with invalid 'investigate='") != std::string::npos) {
            ++bad_investigate;
        }
    }
    NF_CHECK(bad_investigate == 2);
    // A refused line is no route at all: the gate must not silently walk with
    // a default search time the author never wrote.
    NF_CHECK(loaded.scene->world().query<NavRouteComponent>().empty());
}

NF_TEST(nav_route_squad_line_round_trips_and_defaults_to_empty) {
    Scene s("SquadRoundTrip");
    ecs::Entity v = s.world().create_entity();
    s.world().add<Transform>(v, Transform{});
    NavMeshComponent nm;
    nm.area_x = 12.0f;
    nm.area_z = 12.0f;
    s.world().add<NavMeshComponent>(v, nm);

    ecs::Entity w = s.world().create_entity();
    s.world().add<Transform>(w, Transform{});
    s.world().add<NavAgentComponent>(w, NavAgentComponent{});
    NavRouteComponent route;
    route.hold = NavRouteHoldPolicy::Aware;
    route.investigate = 2.5f;
    route.squad = "guards";
    route.points.push_back(NavPointSpec{2.0f, 0.0f, 2.0f, -1.0f});
    route.points.push_back(NavPointSpec{8.0f, 0.0f, 8.0f, -1.0f});
    s.world().add<NavRouteComponent>(w, route);

    const std::string text = serialize_scene_to_text(s);
    NF_CHECK(text.find("squad=guards") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    const NavRouteComponent* back = nullptr;
    for (ecs::Entity e : loaded.scene->world().all_entities()) {
        if (const auto* found = loaded.scene->world().get<NavRouteComponent>(e)) {
            back = found;
        }
    }
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK(back->squad == "guards");
    }

    // A route written before this phase carries no token: it loads as solo and
    // serializes without inventing one. A Phase 66 route stays byte-identical.
    const std::string legacy =
        "# SANAD Scene v1\nversion: 1\nname: LegacySquad\nentity_count: 1\n---\n"
        "entity: 1:0\n  Name: Walker\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavRoute: wait=0.5 hold=aware resume=1 investigate=2\n"
        "  NavPoint: pos=1,0,1\n"
        "  NavPoint: pos=4,0,4\n";
    const SceneLoadResult plain = load_scene_from_text(legacy);
    NF_CHECK(plain.success);
    const auto* plain_route =
        plain.scene->world().get<NavRouteComponent>(plain.scene->world().all_entities()[0]);
    NF_CHECK(plain_route != nullptr);
    if (plain_route != nullptr) {
        NF_CHECK(plain_route->squad.empty());
    }
    const std::string again = serialize_scene_to_text(*plain.scene);
    NF_CHECK(again.find("squad=") == std::string::npos);

    // The cap is exactly 64 characters. A 64-character token is a valid name;
    // a 65th character is refused rather than silently truncated, because two
    // squads that differ only past the cut would keep answering each other.
    const std::string max_token(64u, 's');
    const std::string too_long(65u, 's');
    Scene cap_scene("CapSquad");
    ecs::Entity cap_walker = cap_scene.world().create_entity();
    cap_scene.world().add<Transform>(cap_walker, Transform{});
    cap_scene.world().add<NavAgentComponent>(cap_walker, NavAgentComponent{});
    NavRouteComponent cap_route;
    cap_route.hold = NavRouteHoldPolicy::Aware;
    cap_route.investigate = 1.0f;
    cap_route.squad = max_token;
    cap_route.points.push_back(NavPointSpec{1.0f, 0.0f, 1.0f, -1.0f});
    cap_route.points.push_back(NavPointSpec{4.0f, 0.0f, 4.0f, -1.0f});
    cap_scene.world().add<NavRouteComponent>(cap_walker, cap_route);
    const SceneLoadResult at_cap = load_scene_from_text(serialize_scene_to_text(cap_scene));
    NF_CHECK(at_cap.success);
    const auto* at_cap_route =
        at_cap.scene->world().get<NavRouteComponent>(at_cap.scene->world().all_entities()[0]);
    NF_CHECK(at_cap_route != nullptr);
    if (at_cap_route != nullptr) {
        NF_CHECK(at_cap_route->squad == max_token);
    }

    const std::string bad =
        "# SANAD Scene v1\nversion: 1\nname: LongSquad\nentity_count: 1\n---\n"
        "entity: 1:0\n  Name: Walker\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavAgent: speed=3 goal(0,0,0)\n"
        "  NavRoute: hold=aware investigate=1 squad=" + too_long + "\n";
    const SceneLoadResult refused = load_scene_from_text(bad);
    NF_CHECK(refused.success);
    bool named = false;
    for (const std::string& warning : refused.warnings) {
        if (warning.find("NavRoute line with invalid 'squad='") != std::string::npos) {
            named = true;
        }
    }
    NF_CHECK(named);
    NF_CHECK(refused.scene->world().query<NavRouteComponent>().empty());
}

NF_TEST(perception_aware_hold_freezes_the_patrol_then_resumes_with_hysteresis) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    NavRouteComponent route;
    route.loop = false;
    route.hold = NavRouteHoldPolicy::Aware;
    // Half a second of post-sighting stillness: long enough to observe, short
    // enough that the test does not spend its budget waiting for it.
    route.resume = 0.5f;
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 6.0f, -1.0f});
    route.points.push_back(NavPointSpec{6.0f, 0.0f, 6.0f, -1.0f});

    PerceptionComponent senses;
    senses.sight_range = 30.0f;
    senses.sight_half_fov_deg = 60.0f;
    senses.sight_gain = 2.0f; // one second to fill at strength 0.5
    senses.sight_loss = 0.6f;
    senses.sight_linger = 3.0f;
    senses.hearing_range = 0.0f;
    senses.touch_range = 0.0f;
    senses.pain = false;

    // The walker starts at (1,1) looking down -Z; the target stands in front of
    // it at (1,-2), so the sight cone and the faction filter both see a threat.
    const PerceptionRouteScene sc =
        add_perception_route_scene(*env.runtime, route, senses, 1.0f, -2.0f);
    NF_CHECK(sc.walker.valid());
    NF_CHECK(sc.target.valid());
    if (!sc.walker.valid() || !sc.target.valid()) {
        return;
    }

    for (int i = 0; i < 45; ++i) {
        env.runtime->update(kDt);
    }
    u32 index = 0;
    u32 count = 0;
    f32 wait = 0.0f;
    bool finished = false;
    bool held = false;
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held));
    NF_CHECK(held);
    NF_CHECK(env.runtime->entity_aware_of(sc.walker, sc.target));
    NF_CHECK(env.runtime->entity_sensing(sc.walker, sc.target));

    const auto* frozen = env.runtime->scene()->world().get<Transform>(sc.walker);
    NF_CHECK(frozen != nullptr);
    const f32 frozen_z = frozen != nullptr ? frozen->local_z : 0.0f;
    NF_CHECK(frozen_z > 1.0f); // it walked before it noticed, then stopped
    for (int i = 0; i < 30; ++i) {
        env.runtime->update(kDt);
    }
    const auto* still = env.runtime->scene()->world().get<Transform>(sc.walker);
    NF_CHECK(still != nullptr);
    if (still != nullptr) {
        NF_CHECK_NEAR(still->local_z, frozen_z, 1e-4f);
    }

    // The debug overlay reads the same hold: red route, arrived marker.
    const NavDebugSnapshot snap = env.runtime->nav_debug_snapshot();
    bool saw_agent = false;
    for (const NavDebugAgent& agent : snap.agents) {
        if (agent.entity == sc.walker) {
            saw_agent = true;
            NF_CHECK(agent.route_held);
            NF_CHECK(agent.at_goal);
        }
    }
    NF_CHECK(saw_agent);

    // The fifth value of nf.route() is the hold, so a script can play its own
    // "I see something" pose without duplicating the gate.
    const std::string script =
        "function update(dt)\n"
        "  local id, gen = nf.self()\n"
        "  local index, count, wait, finished, held = nf.route()\n"
        "  if index == nil then\n"
        "    nf.set_entity_scale(id, gen, -1.0, -1.0, -1.0)\n"
        "  else\n"
        "    nf.set_entity_scale(id, gen, held and 1.0 or 0.0, count,\n"
        "                        finished and 1.0 or 0.0)\n"
        "  end\n"
        "end\n";
    NF_CHECK(env.vfs.write_text("content://Scripts/route_hold_probe.lua", script).ok);
    ecs::World& world = env.runtime->edit_scene()->world();
    scripting::ScriptComponent script_component;
    // Added after adoption, so nothing has loaded the file into `source` yet;
    // ScriptSystem ticks the source, not the path.
    script_component.source = script;
    script_component.path = "content://Scripts/route_hold_probe.lua";
    world.add<scripting::ScriptComponent>(sc.walker, script_component);
    for (int i = 0; i < 3; ++i) {
        env.runtime->update(kDt);
    }
    const auto* probe = world.get<Transform>(sc.walker);
    NF_CHECK(probe != nullptr);
    if (probe != nullptr) {
        NF_CHECK_NEAR(probe->scale_x, 1.0f, 1e-5f); // held
        NF_CHECK_NEAR(probe->scale_y, 2.0f, 1e-5f); // point count
    }

    // Removing the threat does not release the patrol immediately: `resume=`
    // keeps it still for half a second of post-sighting hysteresis.
    world.destroy_entity(sc.target);
    for (int i = 0; i < 20; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held));
    NF_CHECK(held);
    const auto* during = world.get<Transform>(sc.walker);
    NF_CHECK(during != nullptr);
    if (during != nullptr) {
        NF_CHECK_NEAR(during->local_z, frozen_z, 1e-4f);
    }

    // Past the delay the walker takes up the same route and point again.
    for (int i = 0; i < 20; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held));
    NF_CHECK(!held);
    const auto* released = world.get<Transform>(sc.walker);
    NF_CHECK(released != nullptr);
    if (released != nullptr) {
        NF_CHECK(released->local_z > frozen_z + 0.05f);
        NF_CHECK_NEAR(released->scale_x, 0.0f, 1e-5f); // the script sees it too
    }
}

NF_TEST(perception_sensing_hold_releases_when_the_target_leaves_the_cone) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    NavRouteComponent route;
    route.loop = false;
    route.hold = NavRouteHoldPolicy::Sensing;
    route.resume = 0.0f;
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 6.0f, -1.0f});
    route.points.push_back(NavPointSpec{6.0f, 0.0f, 6.0f, -1.0f});

    PerceptionComponent senses;
    senses.sight_range = 30.0f;
    senses.sight_half_fov_deg = 60.0f;
    senses.sight_gain = 2.0f;
    senses.sight_loss = 0.6f;
    senses.sight_linger = 30.0f; // the memory outlives the sighting by far
    senses.hearing_range = 0.0f;
    senses.touch_range = 0.0f;
    senses.pain = false;

    const PerceptionRouteScene sc =
        add_perception_route_scene(*env.runtime, route, senses, 1.0f, -2.0f);
    NF_CHECK(sc.walker.valid());
    NF_CHECK(sc.target.valid());
    if (!sc.walker.valid() || !sc.target.valid()) {
        return;
    }

    for (int i = 0; i < 45; ++i) {
        env.runtime->update(kDt);
    }
    u32 index = 0;
    u32 count = 0;
    f32 wait = 0.0f;
    bool finished = false;
    bool held = false;
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held));
    NF_CHECK(held);

    const auto* frozen = env.runtime->scene()->world().get<Transform>(sc.walker);
    NF_CHECK(frozen != nullptr);
    const f32 frozen_z = frozen != nullptr ? frozen->local_z : 0.0f;

    // Out of the cone, still well inside sight range: the walker remembers the
    // target (linger) but no sense is reading it this frame. `sensing` must
    // release the patrol even though `aware` stays true.
    auto* tt = env.runtime->edit_scene()->world().get<Transform>(sc.target);
    NF_CHECK(tt != nullptr);
    if (tt == nullptr) {
        return;
    }
    tt->local_x = 10.0f;
    tt->local_z = 1.0f;

    // The first frame's perception step is what stops `sensed`; navigation
    // runs before it, so the gate opens on the second frame.
    for (int i = 0; i < 2; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held));
    NF_CHECK(!held);
    NF_CHECK(env.runtime->entity_aware_of(sc.walker, sc.target));
    NF_CHECK(!env.runtime->entity_sensing(sc.walker, sc.target));

    // Released for the whole frame, not just the Accessor: the walker moved.
    const auto* released = env.runtime->scene()->world().get<Transform>(sc.walker);
    NF_CHECK(released != nullptr);
    if (released != nullptr) {
        NF_CHECK(released->local_z > frozen_z + 0.02f);
    }

    // And the snapshot agrees, so the overlay is not frozen red.
    const NavDebugSnapshot snap = env.runtime->nav_debug_snapshot();
    for (const NavDebugAgent& agent : snap.agents) {
        if (agent.entity == sc.walker) {
            NF_CHECK(!agent.route_held);
        }
    }
}

// --- Last-known-position investigation (Phase 66) ---------------------------

NF_TEST(perception_hold_investigates_last_known_position_then_returns_to_route) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    NavRouteComponent route;
    route.loop = false;
    route.hold = NavRouteHoldPolicy::Aware;
    route.resume = 0.0f;    // no hysteresis: destruction releases the gate now
    route.investigate = 1.0f;
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 6.0f, -1.0f});
    route.points.push_back(NavPointSpec{6.0f, 0.0f, 6.0f, -1.0f});

    PerceptionComponent senses;
    senses.sight_range = 30.0f;
    senses.sight_half_fov_deg = 60.0f;
    senses.sight_gain = 2.0f;
    senses.sight_loss = 0.6f;
    senses.sight_linger = 3.0f;
    senses.hearing_range = 0.0f;
    senses.touch_range = 0.0f;
    senses.pain = false;

    // The walker heads down +Z while facing -Z, so the target at (1,-2) stays
    // in the sight cone for the whole approach. The volume is shifted so the
    // remembered point lies on the mesh: the canonical [0,12] footprint does
    // not cover negative Z, and an off-mesh memory would never be reached.
    const PerceptionRouteScene sc =
        add_perception_route_scene(*env.runtime, route, senses, 1.0f, -2.0f, -4.0f);
    NF_CHECK(sc.walker.valid());
    NF_CHECK(sc.target.valid());
    if (!sc.walker.valid() || !sc.target.valid()) {
        return;
    }

    for (int i = 0; i < 45; ++i) {
        env.runtime->update(kDt);
    }
    u32 index = 0;
    u32 count = 0;
    f32 wait = 0.0f;
    bool finished = false;
    bool held = false;
    bool investigating = false;
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating));
    NF_CHECK(held);
    NF_CHECK(!investigating);

    // The sixth value of nf.route() is the search, appended after Phase 65's
    // held flag so an older script's destructuring keeps its meaning.
    const std::string script =
        "function update(dt)\n"
        "  local id, gen = nf.self()\n"
        "  local index, count, wait, finished, held, investigating = nf.route()\n"
        "  if index == nil then\n"
        "    nf.set_entity_scale(id, gen, -1.0, -1.0, -1.0)\n"
        "  else\n"
        "    nf.set_entity_scale(id, gen, held and 1.0 or 0.0,\n"
        "                        investigating and 1.0 or 0.0,\n"
        "                        count)\n"
        "  end\n"
        "end\n";
    NF_CHECK(env.vfs.write_text("content://Scripts/route_investigate_probe.lua", script).ok);
    ecs::World& world = env.runtime->edit_scene()->world();
    scripting::ScriptComponent script_component;
    script_component.source = script; // added after adoption
    script_component.path = "content://Scripts/route_investigate_probe.lua";
    world.add<scripting::ScriptComponent>(sc.walker, script_component);
    for (int i = 0; i < 3; ++i) {
        env.runtime->update(kDt);
    }
    const auto* probe = world.get<Transform>(sc.walker);
    NF_CHECK(probe != nullptr);
    if (probe != nullptr) {
        NF_CHECK_NEAR(probe->scale_x, 1.0f, 1e-5f); // held
        NF_CHECK_NEAR(probe->scale_y, 0.0f, 1e-5f); // not investigating yet
        NF_CHECK_NEAR(probe->scale_z, 2.0f, 1e-5f); // point count
    }

    // Losing the threat releases the gate; the walker leaves the route and
    // heads for where it last knew the target (1, 0, -2).
    world.destroy_entity(sc.target);
    for (int i = 0; i < 5; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating));
    NF_CHECK(!held);
    NF_CHECK(investigating);
    NF_CHECK(index == 0);
    NF_CHECK(count == 2);
    const auto* leaving = world.get<Transform>(sc.walker);
    NF_CHECK(leaving != nullptr);
    if (leaving != nullptr) {
        NF_CHECK_NEAR(leaving->scale_x, 0.0f, 1e-5f); // script agrees: not held
        NF_CHECK_NEAR(leaving->scale_y, 1.0f, 1e-5f); // script agrees: searching
    }

    // The overlay draws the detour to the remembered point in yellow, not the
    // route point the walker will pick up afterwards.
    const NavDebugSnapshot snap = env.runtime->nav_debug_snapshot();
    bool saw_investigation = false;
    for (const NavDebugAgent& agent : snap.agents) {
        if (agent.entity == sc.walker) {
            saw_investigation = true;
            NF_CHECK(agent.route_investigating);
            NF_CHECK(agent.goal.nearly_equals(Vec3{1.0f, 0.0f, -2.0f}, 1e-3f));
        }
    }
    NF_CHECK(saw_investigation);

    // Walk to the memory, scan there for `investigate=` seconds, then return
    // to the SAME route point: the hold interrupted a leg, not a pause.
    f32 min_z = std::numeric_limits<float>::max();
    int frames = 0;
    while (investigating && frames < 600) {
        env.runtime->update(kDt);
        ++frames;
        const auto* t = world.get<Transform>(sc.walker);
        if (t != nullptr) {
            min_z = std::min(min_z, t->local_z);
        }
        if (!env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating)) {
            break;
        }
    }
    NF_CHECK(!investigating);
    NF_CHECK(frames < 600);
    NF_CHECK(min_z < -1.5f); // it actually walked back to the last known position
    NF_CHECK(index == 0);
    NF_CHECK(count == 2);
    NF_CHECK(!finished);
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating));
    NF_CHECK(!held);
    NF_CHECK(!investigating);

    // And it walks back towards point 1 (z=6) instead of stopping at the memory.
    const f32 turned_at = world.get<Transform>(sc.walker) != nullptr
                              ? world.get<Transform>(sc.walker)->local_z
                              : 0.0f;
    for (int i = 0; i < 60; ++i) {
        env.runtime->update(kDt);
    }
    const auto* back = world.get<Transform>(sc.walker);
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK(back->local_z > turned_at + 1.0f);
        NF_CHECK_NEAR(back->scale_y, 0.0f, 1e-5f); // the search is over
    }
}

NF_TEST(a_new_sighting_during_the_scan_restarts_the_search) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    NavRouteComponent route;
    route.loop = false;
    route.hold = NavRouteHoldPolicy::Aware;
    route.resume = 0.0f;
    route.investigate = 3.0f;
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 6.0f, -1.0f});
    route.points.push_back(NavPointSpec{6.0f, 0.0f, 6.0f, -1.0f});

    PerceptionComponent senses;
    senses.sight_range = 30.0f;
    senses.sight_half_fov_deg = 60.0f;
    senses.sight_gain = 2.0f;
    senses.sight_loss = 0.6f;
    senses.sight_linger = 3.0f;
    senses.hearing_range = 0.0f;
    senses.touch_range = 0.0f;
    senses.pain = false;

    const PerceptionRouteScene sc =
        add_perception_route_scene(*env.runtime, route, senses, 1.0f, -2.0f, -4.0f);
    NF_CHECK(sc.walker.valid());
    NF_CHECK(sc.target.valid());
    if (!sc.walker.valid() || !sc.target.valid()) {
        return;
    }

    u32 index = 0;
    u32 count = 0;
    f32 wait = 0.0f;
    bool finished = false;
    bool held = false;
    bool investigating = false;
    for (int i = 0; i < 45; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating));
    NF_CHECK(held);

    // Destroy the first threat; the walker starts searching its memory.
    ecs::World& world = env.runtime->edit_scene()->world();
    world.destroy_entity(sc.target);
    for (int i = 0; i < 100; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating));
    NF_CHECK(investigating);
    const auto* at_memory = world.get<Transform>(sc.walker);
    NF_CHECK(at_memory != nullptr);
    if (at_memory != nullptr) {
        NF_CHECK(at_memory->local_z < -1.5f); // standing at the first memory
    }

    // A second threat appears directly in front. The gate closes while the
    // scan is running: the clock must freeze and the remembered point move.
    ecs::Entity second = world.create_entity();
    Transform bt{};
    bt.local_x = 1.0f;
    bt.local_z = -3.5f;
    bt.dirty = true;
    world.add<Transform>(second, bt);
    FactionComponent second_faction;
    second_faction.team = "Red";
    world.add<FactionComponent>(second, second_faction);
    world.add<HealthComponent>(second, HealthComponent{});

    int acquire = 0;
    while (!held && acquire < 120) {
        env.runtime->update(kDt);
        ++acquire;
        if (!env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating)) {
            break;
        }
    }
    NF_CHECK(held);
    NF_CHECK(acquire < 120);
    const auto* frozen = world.get<Transform>(sc.walker);
    NF_CHECK(frozen != nullptr);
    const f32 frozen_z = frozen != nullptr ? frozen->local_z : 0.0f;
    for (int i = 0; i < 20; ++i) {
        env.runtime->update(kDt);
    }
    const auto* still = world.get<Transform>(sc.walker);
    NF_CHECK(still != nullptr);
    if (still != nullptr) {
        NF_CHECK_NEAR(still->local_z, frozen_z, 1e-4f); // the scan is frozen
    }

    // Losing the second threat re-arms the detour at ITS position, not the
    // first one, and the walker walks there before scanning again.
    world.destroy_entity(second);
    for (int i = 0; i < 5; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating));
    NF_CHECK(!held);
    NF_CHECK(investigating);
    const NavDebugSnapshot snap = env.runtime->nav_debug_snapshot();
    bool saw_new_goal = false;
    for (const NavDebugAgent& agent : snap.agents) {
        if (agent.entity == sc.walker) {
            saw_new_goal = true;
            NF_CHECK(agent.route_investigating);
            NF_CHECK(agent.goal.nearly_equals(Vec3{1.0f, 0.0f, -3.5f}, 1e-3f));
        }
    }
    NF_CHECK(saw_new_goal);

    int frames = 0;
    f32 min_z = std::numeric_limits<float>::max();
    while (investigating && frames < 500) {
        env.runtime->update(kDt);
        ++frames;
        const auto* t = world.get<Transform>(sc.walker);
        if (t != nullptr) {
            min_z = std::min(min_z, t->local_z);
        }
        if (!env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating)) {
            break;
        }
    }
    NF_CHECK(!investigating);
    NF_CHECK(frames < 500);
    NF_CHECK(min_z < -3.0f); // it reached the second memory
    NF_CHECK(index == 0);    // the interrupt was mid-leg, so the point is kept
}

NF_TEST(investigation_replaces_an_interrupted_waypoint_pause) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    NavRouteComponent route;
    route.loop = false;
    route.hold = NavRouteHoldPolicy::Aware;
    route.resume = 0.0f;
    route.investigate = 1.0f;
    // A long pause at point 1. The hold starts while this pause is running, so
    // the search must REPLACE it and the route must move on afterwards — not
    // sit out the remaining seconds at a point the walker already visited.
    route.points.push_back(NavPointSpec{1.0f, 0.0f, 2.0f, 5.0f});
    route.points.push_back(NavPointSpec{6.0f, 0.0f, 6.0f, 0.0f});

    PerceptionComponent senses;
    senses.sight_range = 30.0f;
    senses.sight_half_fov_deg = 60.0f;
    senses.sight_gain = 2.0f;
    senses.sight_loss = 0.6f;
    senses.sight_linger = 3.0f;
    senses.hearing_range = 0.0f;
    senses.touch_range = 0.0f;
    senses.pain = false;

    // Out of the cone at first: the walker reaches its point and starts the
    // pause without being held. The shifted volume keeps the remembered point
    // on the mesh at (1, 0, -2).
    const PerceptionRouteScene sc =
        add_perception_route_scene(*env.runtime, route, senses, 10.0f, 10.0f, -4.0f);
    NF_CHECK(sc.walker.valid());
    NF_CHECK(sc.target.valid());
    if (!sc.walker.valid() || !sc.target.valid()) {
        return;
    }

    u32 index = 0;
    u32 count = 0;
    f32 wait = 0.0f;
    bool finished = false;
    bool held = false;
    bool investigating = false;
    for (int i = 0; i < 25; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating));
    NF_CHECK(index == 0);
    NF_CHECK(wait > 3.0f); // the long pause is running
    NF_CHECK(!held);

    // Put the target in front of the waiting walker. Detection accumulates for
    // about half a second, then the gate closes and freezes the pause.
    auto* tt = env.runtime->edit_scene()->world().get<Transform>(sc.target);
    NF_CHECK(tt != nullptr);
    if (tt == nullptr) {
        return;
    }
    tt->local_x = 1.0f;
    tt->local_z = -2.0f;
    tt->dirty = true;
    int acquire_frames = 0;
    while (!held && acquire_frames < 120) {
        env.runtime->update(kDt);
        ++acquire_frames;
        if (!env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating)) {
            break;
        }
    }
    NF_CHECK(held);
    NF_CHECK(acquire_frames < 120);
    const f32 wait_when_held = wait;
    NF_CHECK(wait_when_held > 3.0f);

    for (int i = 0; i < 30; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating));
    NF_CHECK(held);
    NF_CHECK_NEAR(wait, wait_when_held, 1e-4f); // the pause is frozen, not spent

    // The threat vanishes; the search replaces the pause and, when the scan
    // ends, the route advances to point 2 without replaying the five seconds.
    env.runtime->edit_scene()->world().destroy_entity(sc.target);
    env.runtime->update(kDt);
    NF_CHECK(env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating));
    NF_CHECK(!held);
    NF_CHECK(investigating);
    int frames = 0;
    while (investigating && frames < 300) {
        env.runtime->update(kDt);
        ++frames;
        if (!env.runtime->nav_route_state(sc.walker, index, count, wait, finished, &held,
                                          &investigating)) {
            break;
        }
    }
    NF_CHECK(!investigating);
    NF_CHECK(frames < 300);
    NF_CHECK(index == 1); // the pause was replaced, not resumed
    NF_CHECK(wait <= 1e-4f);
}

NF_TEST(squad_mates_answer_a_shared_search_and_strangers_do_not) {
    HeadlessRuntime env;
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }

    auto s = std::make_unique<Scene>("SquadSearch");
    // The volume is shifted so the remembered point (1, 0, -2) lies on the
    // mesh, exactly like the Phase 66 fixtures.
    add_nav_scene(*s, true, -4.0f);
    ecs::Entity originator = find_walker(*s);
    NF_CHECK(originator.valid());
    if (!originator.valid()) {
        return;
    }

    const auto place = [&](ecs::Entity e, f32 x, f32 z) {
        if (auto* t = s->world().get<Transform>(e); t != nullptr) {
            t->local_x = x;
            t->local_z = z;
            t->local_y = 0.0f;
        }
        if (auto* agent = s->world().get<NavAgentComponent>(e); agent != nullptr) {
            agent->speed = 50.0f;
        }
        FactionComponent faction;
        faction.team = "Blue";
        s->world().add<FactionComponent>(e, faction);
    };
    const auto make_mate = [&](f32 x, f32 z, const char* route_squad) {
        ecs::Entity e = s->world().create_entity();
        s->world().add<Transform>(e, Transform{});
        NavAgentComponent agent;
        agent.speed = 50.0f;
        agent.goal_x = x;
        agent.goal_z = z;
        s->world().add<NavAgentComponent>(e, agent);
        place(e, x, z);
        NavRouteComponent route;
        route.loop = true;
        route.hold = NavRouteHoldPolicy::Aware;
        route.investigate = 0.5f;
        route.squad = route_squad;
        route.points.push_back(NavPointSpec{x, 0.0f, 6.0f, -1.0f});
        route.points.push_back(NavPointSpec{x > 8.0f ? 9.0f : 10.0f, 0.0f, 6.0f, -1.0f});
        s->world().add<NavRouteComponent>(e, route);
        return e;
    };

    NavRouteComponent originator_route;
    originator_route.loop = true;
    originator_route.hold = NavRouteHoldPolicy::Aware;
    originator_route.resume = 0.0f;
    originator_route.investigate = 4.0f;
    originator_route.squad = "guards";
    originator_route.points.push_back(NavPointSpec{1.0f, 0.0f, 6.0f, -1.0f});
    originator_route.points.push_back(NavPointSpec{6.0f, 0.0f, 6.0f, -1.0f});
    place(originator, 1.0f, 1.0f);
    s->world().add<NavRouteComponent>(originator, originator_route);

    PerceptionComponent senses;
    senses.sight_range = 30.0f;
    senses.sight_half_fov_deg = 60.0f;
    senses.sight_gain = 2.0f;
    senses.sight_loss = 0.6f;
    senses.sight_linger = 3.0f;
    senses.hearing_range = 0.0f;
    senses.touch_range = 0.0f;
    senses.pain = false;
    s->world().add<PerceptionComponent>(originator, senses);

    // One mate carries the same token. It has no Perception, so the only way
    // it can ever leave its route is the shared call. The stranger carries
    // another token and must never detour.
    ecs::Entity mate = make_mate(7.0f, 1.0f, "guards");
    ecs::Entity stranger = make_mate(11.0f, 1.0f, "others");

    ecs::Entity target = s->world().create_entity();
    Transform tt{};
    tt.local_x = 1.0f;
    tt.local_z = -2.0f;
    s->world().add<Transform>(target, tt);
    FactionComponent target_faction;
    target_faction.team = "Red";
    s->world().add<FactionComponent>(target, target_faction);
    s->world().add<HealthComponent>(target, HealthComponent{});

    env.runtime->adopt_scene(std::move(s), "content://Scenes/SquadSearch.nfscene");

    const auto query = [&](ecs::Entity e, bool& held, bool& investigating) {
        u32 index = 0;
        u32 count = 0;
        f32 wait = 0.0f;
        bool finished = false;
        return env.runtime->nav_route_state(e, index, count, wait, finished, &held,
                                            &investigating);
    };
    const auto z_of = [&](ecs::Entity e) {
        const auto* t = env.runtime->edit_scene()->world().get<Transform>(e);
        return t != nullptr ? t->local_z : 0.0f;
    };

    // Acquire the threat and hold. The mate has no sight and keeps patrolling;
    // the stranger keeps walking its own route for now.
    bool held = false;
    bool investigating = false;
    bool mate_held = false;
    bool mate_investigating = false;
    bool stranger_held = false;
    bool stranger_investigating = false;
    for (int i = 0; i < 45; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(query(originator, held, investigating));
    NF_CHECK(held);
    NF_CHECK(!investigating);
    NF_CHECK(query(mate, mate_held, mate_investigating));
    NF_CHECK(!mate_held);
    NF_CHECK(!mate_investigating);
    NF_CHECK(query(stranger, stranger_held, stranger_investigating));
    NF_CHECK(!stranger_investigating);

    // The threat vanishes: the originator leaves its route for the last known
    // position. The mate joins on the next navigation step, because the board
    // is rebuilt from the previous frame (the documented one-frame latency).
    env.runtime->edit_scene()->world().destroy_entity(target);
    int start_frames = 0;
    while (!investigating && start_frames < 60) {
        env.runtime->update(kDt);
        ++start_frames;
        if (!query(originator, held, investigating)) {
            break;
        }
    }
    NF_CHECK(investigating);
    NF_CHECK(start_frames < 60);

    int frames = 0;
    f32 originator_min_z = std::numeric_limits<float>::max();
    f32 mate_min_z = std::numeric_limits<float>::max();
    f32 stranger_min_z = std::numeric_limits<float>::max();
    f32 mate_after_finish_min_z = std::numeric_limits<float>::max();
    bool mate_was_investigating = false;
    bool mate_finished = false;
    bool mate_returned_to_route = false;
    bool stranger_ever_investigated = false;
    bool still_investigating_at_one_second = false;
    while (investigating && frames < 600) {
        env.runtime->update(kDt);
        ++frames;
        if (!query(originator, held, investigating)) {
            break;
        }
        query(mate, mate_held, mate_investigating);
        query(stranger, stranger_held, stranger_investigating);
        originator_min_z = std::min(originator_min_z, z_of(originator));
        mate_min_z = std::min(mate_min_z, z_of(mate));
        stranger_min_z = std::min(stranger_min_z, z_of(stranger));
        if (mate_investigating) {
            mate_was_investigating = true;
        } else if (mate_was_investigating) {
            mate_finished = true;
        }
        // Measure the "does not re-join" rule only after the mate has actually
        // walked back out of the search point; the frame the scan ends still
        // stands at the negative Z it searched.
        if (mate_finished && z_of(mate) > 0.5f) {
            mate_returned_to_route = true;
        }
        if (mate_returned_to_route) {
            mate_after_finish_min_z = std::min(mate_after_finish_min_z, z_of(mate));
        }
        if (stranger_investigating) {
            stranger_ever_investigated = true;
        }
        if (frames == 60) {
            still_investigating_at_one_second = investigating;
            // The mate's own 0.5s scan is over while the originator's 4s scan
            // is still running: each searcher uses its own investigate=.
            NF_CHECK(!mate_investigating);
            NF_CHECK(z_of(mate) > 0.0f);
        }
    }

    NF_CHECK(!investigating);
    NF_CHECK(frames < 600);
    NF_CHECK(frames >= 230); // the originator's four-second scan really ran
    NF_CHECK(still_investigating_at_one_second);
    NF_CHECK(mate_was_investigating);
    NF_CHECK(mate_finished);
    NF_CHECK(!stranger_ever_investigated);
    NF_CHECK(originator_min_z < -1.5f);
    NF_CHECK(mate_min_z < -1.5f);
    NF_CHECK(stranger_min_z > 0.5f);
    // Finishing a scan does not re-join the same still-live call: the mate
    // returned to its route while the originator was still sweeping.
    NF_CHECK(mate_returned_to_route);
    NF_CHECK(mate_after_finish_min_z > 0.5f);

    // And the originator picks its route point back up after the scan.
    for (int i = 0; i < 60; ++i) {
        env.runtime->update(kDt);
    }
    NF_CHECK(query(originator, held, investigating));
    NF_CHECK(!held);
    NF_CHECK(!investigating);
    NF_CHECK(z_of(originator) > 0.5f);
}
