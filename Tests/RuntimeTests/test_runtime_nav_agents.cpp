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
void add_nav_scene(Scene& s, bool enable = true) {
    ecs::Entity volume = s.world().create_entity();
    Transform vt{};
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

    const ValidationReport report = validate_scene(s, "test.nfscene");
    NF_CHECK(has(report, "needs at least two"));
    NF_CHECK(has(report, "ping-pong wins"));
    NF_CHECK(has(report, "without a NavAgent"));
    NF_CHECK(has(report, "outside the navigation volume"));

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
