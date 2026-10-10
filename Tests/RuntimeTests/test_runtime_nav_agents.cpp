// Tests/RuntimeTests/test_runtime_nav_agents.cpp â€” Phase 28 (navigation)
//
// Rule 0 for navigation. `nf::ai::NavMesh` is unit-tested in AITests â€” the
// voxel pipeline, the walkable classification, the polygon cover, the links
// and the A* all have their own suite, CPU-only. What that suite cannot see is
// whether a shipped game can use any of it: no scene could name a volume, no
// frame stepped an agent. This file is the integration half, and it follows
// the Phase 25 pattern exactly â€” a scene that merely DECLARES the components
// ends up baked and walked, with nothing driving the seam by hand.
//
// Headless like the destruction reachability suite: NF_SKIP when no Vulkan
// device is available (the Runtime constructs a renderer on adopt).

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/AI/NavMesh.hpp>

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
        tmp = std::filesystem::temp_directory_path() / "nf_runtime_nav_agents";
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
    // Maximum XZ mismatch between two paths of the same length â€” the
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

} // namespace

// --- Scene format ------------------------------------------------------------

NF_TEST(navmesh_and_agent_lines_round_trip_through_scene_text) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_nav_rt";
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
    auto tmp = std::filesystem::temp_directory_path() / "nf_nav_bad";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    // A zero cell size is a divide-by-zero in the voxeliser, so the whole line
    // is refused rather than clamped â€” same contract as the light lines.
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
