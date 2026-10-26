// Tests/RuntimeTests/test_terrain.cpp — Phase 63: the ground the navigation
// mesh walks on.
//
// Before this phase the bake's height sampler was a FLAT floor at the volume's
// own height, and that was honest: `.nfscene` had no way to author terrain, so
// a sampler that invented hills would have described a world nothing else
// shared. This suite pins the two halves of the fix:
//
//   1. the scene format — `Terrain:` and `NavLink:` parse, refuse what they
//      cannot honour, and survive a save/load round trip unchanged; and
//   2. the integration — a scene that merely DECLARES a terrain gets a
//      navigation mesh that follows it, and a link the author wrote reaches
//      the bake.
//
// The pure half runs anywhere; the integration half needs a Vulkan device
// (adopting a scene constructs a renderer) and NF_SKIPs without one, exactly
// like the nav-agent and destruction reachability suites.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/AI/NavMesh.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Terrain.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Runtime/SceneValidator.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::scene;

namespace {

VirtualFileSystem& scratch_vfs(const char* name) {
    static std::vector<std::unique_ptr<VirtualFileSystem>> owned;
    owned.push_back(std::make_unique<VirtualFileSystem>());
    auto& vfs = *owned.back();
    const auto dir = nf::test::temp_root() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    vfs.mount("content://", dir);
    return vfs;
}

/// A terrain entity plus a navigation volume over it: the smallest scene that
/// can answer "did the bake read the field".
Scene make_terrain_scene(f32 height_scale, f32 area = 20.0f) {
    Scene s("TerrainBake");

    ecs::Entity ground = s.world().create_entity();
    Transform gt{};
    s.world().add<Transform>(ground, gt);
    TerrainComponent terrain;
    terrain.resolution = 32u;
    terrain.area = area;
    terrain.height_scale = height_scale;
    terrain.noise_frequency = 0.08f;
    terrain.octaves = 3u;
    terrain.seed = 7u;
    s.world().add<TerrainComponent>(ground, terrain);

    ecs::Entity volume = s.world().create_entity();
    Transform vt{};
    s.world().add<Transform>(volume, vt);
    NavMeshComponent nm;
    nm.area_x = area;
    nm.area_y = 30.0f;
    nm.area_z = area;
    nm.cell_size = 0.5f;
    s.world().add<NavMeshComponent>(volume, nm);
    return s;
}

struct HeadlessRuntime {
    std::filesystem::path tmp;
    VirtualFileSystem vfs;
    AssetRegistry registry;
    AssetManager manager;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<Runtime> runtime;

    explicit HeadlessRuntime(const char* name) : manager(vfs, registry) {
        tmp = nf::test::temp_root() / name;
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

f32 average_height(const ai::NavMesh& mesh) {
    f64 sum = 0.0;
    usize n = 0;
    for (u32 p = 0; p < mesh.polygon_count(); ++p) {
        for (const Vec3& v : mesh.polygon(p).verts) {
            sum += v.y;
            ++n;
        }
    }
    return n == 0 ? 0.0f : static_cast<f32>(sum / static_cast<f64>(n));
}

} // namespace

// --- Scene format ------------------------------------------------------------

NF_TEST(terrain_line_round_trips_through_scene_text) {
    Scene s("TerrainRoundTrip");
    ecs::Entity e = s.world().create_entity();
    s.world().add<Transform>(e, Transform{});
    TerrainComponent t;
    t.resolution = 48u;
    t.area = 120.0f;
    t.height_scale = 7.5f;
    t.noise_frequency = 0.02f;
    t.octaves = 5u;
    t.seed = 4242u;
    t.heightmap = "content://Textures/Hills.png";
    t.material = "content://Materials/Grass.nfmat";
    s.world().add<TerrainComponent>(e, t);

    const std::string text = serialize_scene_to_text(s);
    NF_CHECK(text.find("Terrain:") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    const TerrainComponent* back = nullptr;
    for (auto entity : loaded.scene->world().all_entities()) {
        if (const auto* found = loaded.scene->world().get<TerrainComponent>(entity)) {
            back = found;
        }
    }
    NF_CHECK(back != nullptr);
    if (back == nullptr) {
        return;
    }
    NF_CHECK_EQ(back->resolution, 48u);
    NF_CHECK_NEAR(back->area, 120.0f, 1e-3f);
    NF_CHECK_NEAR(back->height_scale, 7.5f, 1e-4f);
    NF_CHECK_NEAR(back->noise_frequency, 0.02f, 1e-6f);
    NF_CHECK_EQ(back->octaves, 5u);
    NF_CHECK_EQ(back->seed, 4242u);
    NF_CHECK(back->heightmap == "content://Textures/Hills.png");
    NF_CHECK(back->material == "content://Materials/Grass.nfmat");
}

NF_TEST(terrain_line_refuses_a_value_it_cannot_honour) {
    // resolution < 2 cannot form a grid: the loader drops the whole line and
    // says which key, rather than building a one-vertex field.
    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: BadTerrain\nentity_count: 1\n---\n"
        "entity: 0:0\n  Name: Ground\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  Terrain: res=1 area=40\n";
    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    bool warned = false;
    for (const std::string& w : loaded.warnings) {
        if (w.find("Terrain line with invalid 'res='") != std::string::npos) {
            warned = true;
        }
    }
    NF_CHECK(warned);
    bool has_terrain = false;
    for (auto entity : loaded.scene->world().all_entities()) {
        if (loaded.scene->world().has<TerrainComponent>(entity)) {
            has_terrain = true;
        }
    }
    NF_CHECK(!has_terrain);
}

NF_TEST(nav_link_lines_round_trip_and_keep_their_direction) {
    Scene s("LinkRoundTrip");
    ecs::Entity volume = s.world().create_entity();
    s.world().add<Transform>(volume, Transform{});
    NavMeshComponent nm;
    NavLinkSpec two_way;
    two_way.start_x = 1.0f;
    two_way.start_z = 2.0f;
    two_way.end_x = 5.0f;
    two_way.end_y = 2.0f;
    two_way.end_z = 6.0f;
    NavLinkSpec one_way = two_way;
    one_way.bidirectional = false;
    nm.links.push_back(two_way);
    nm.links.push_back(one_way);
    s.world().add<NavMeshComponent>(volume, nm);

    const std::string text = serialize_scene_to_text(s);
    NF_CHECK(text.find("NavLink:") != std::string::npos);
    NF_CHECK(text.find("one_way=true") != std::string::npos);

    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    const NavMeshComponent* back = nullptr;
    for (auto entity : loaded.scene->world().all_entities()) {
        if (const auto* found = loaded.scene->world().get<NavMeshComponent>(entity)) {
            back = found;
        }
    }
    NF_CHECK(back != nullptr);
    if (back == nullptr) {
        return;
    }
    NF_CHECK_EQ(back->links.size(), static_cast<usize>(2));
    if (back->links.size() == 2) {
        NF_CHECK_NEAR(back->links[0].end_y, 2.0f, 1e-4f);
        NF_CHECK(back->links[0].bidirectional);
        NF_CHECK(!back->links[1].bidirectional);
    }
}

NF_TEST(nav_link_without_an_endpoint_is_ignored_by_name) {
    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: BadLink\nentity_count: 1\n---\n"
        "entity: 0:0\n  Name: Volume\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  NavMesh: area=20,8,20\n"
        "  NavLink: from=1,0,1\n";
    const SceneLoadResult loaded = load_scene_from_text(text);
    NF_CHECK(loaded.success);
    bool warned = false;
    for (const std::string& w : loaded.warnings) {
        if (w.find("NavLink line with invalid 'to'") != std::string::npos) {
            warned = true;
        }
    }
    NF_CHECK(warned);
}

// --- Validation --------------------------------------------------------------

NF_TEST(validator_warns_about_links_the_bake_would_drop) {
    Scene s("LinkWarnings");
    ecs::Entity volume = s.world().create_entity();
    s.world().add<Transform>(volume, Transform{});
    NavMeshComponent nm;
    nm.area_x = 10.0f;
    nm.area_y = 5.0f;
    nm.area_z = 10.0f;
    NavLinkSpec outside;
    outside.start_x = 1.0f;
    outside.start_z = 1.0f;
    outside.end_x = 40.0f; // past area_x: the bake will drop it
    outside.end_z = 2.0f;
    nm.links.push_back(outside);
    NavLinkSpec nowhere; // start == end
    nowhere.start_x = 2.0f;
    nowhere.start_z = 2.0f;
    nowhere.end_x = 2.0f;
    nowhere.end_z = 2.0f;
    nm.links.push_back(nowhere);
    s.world().add<NavMeshComponent>(volume, nm);

    const ValidationReport report = validate_scene(s, "test.nfscene");
    bool dropped = false;
    bool pointless = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("outside the volume") != std::string::npos) {
            dropped = true;
        }
        if (issue.message.find("connects nothing") != std::string::npos) {
            pointless = true;
        }
    }
    NF_CHECK(dropped);
    NF_CHECK(pointless);
}

NF_TEST(validator_warns_when_a_terrain_can_only_be_flat) {
    Scene s("FlatTerrain");
    ecs::Entity ground = s.world().create_entity();
    s.world().add<Transform>(ground, Transform{});
    TerrainComponent t;
    t.height_scale = 12.0f;
    t.octaves = 0u; // no octaves and no heightmap: the field is zero everywhere
    s.world().add<TerrainComponent>(ground, t);

    const ValidationReport report = validate_scene(s, "test.nfscene");
    bool flat = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.message.find("the ground will be flat") != std::string::npos) {
            flat = true;
        }
    }
    NF_CHECK(flat);
}

// --- Integration: the bake reads the field -----------------------------------

NF_TEST(navigation_follows_the_authored_terrain) {
    HeadlessRuntime h("nf_terrain_bake");
    if (!h.ok()) {
        NF_SKIP("no Vulkan device");
    }
    auto scene = std::make_unique<Scene>(make_terrain_scene(8.0f));
    h.runtime->adopt_scene(std::move(scene), "content://Scenes/TerrainBake.nfscene");

    NF_CHECK(h.runtime->navmesh_built());
    const ai::NavMesh* mesh = h.runtime->navmesh();
    NF_CHECK(mesh != nullptr);
    if (mesh == nullptr) {
        return;
    }
    NF_CHECK(mesh->polygon_count() > 0);

    // The same field the mesh was built from, evaluated the same way the
    // runtime does: a hill must move the walkable surface off the floor.
    rendering::TerrainOptions opts;
    opts.resolution = 32u;
    opts.size = 20.0f;
    opts.height_scale = 8.0f;
    opts.noise_frequency = 0.08f;
    opts.octaves = 3u;
    opts.seed = 7u;

    f32 terrain_min = 1e9f;
    f32 terrain_max = -1e9f;
    for (f32 z = 0.0f; z <= 20.0f; z += 1.0f) {
        for (f32 x = 0.0f; x <= 20.0f; x += 1.0f) {
            const f32 y = rendering::terrain_height(x, z, opts);
            terrain_min = std::min(terrain_min, y);
            terrain_max = std::max(terrain_max, y);
        }
    }
    NF_CHECK(terrain_max - terrain_min > 1.0f); // the fixture really has relief

    // The mesh's own height range must track the field's. A polygon corner is
    // not a sample point — the voxeliser samples column CENTRES and stores the
    // span top, so a corner sits up to half a cell away from the sample that
    // produced it — which is why this compares the RANGE rather than asserting
    // height(vertex) exactly. A flat bake would pass a range test only by
    // accident, and the fixture's relief is over a metre.
    f32 mesh_min = 1e9f;
    f32 mesh_max = -1e9f;
    for (u32 p = 0; p < mesh->polygon_count(); ++p) {
        for (const Vec3& v : mesh->polygon(p).verts) {
            mesh_min = std::min(mesh_min, v.y);
            mesh_max = std::max(mesh_max, v.y);
        }
    }
    NF_CHECK(mesh_max - mesh_min > 1.0f);
    // The mesh never rises above the field it sampled: a corner is a sample,
    // not an invention. The TOP of the field can legitimately be missing — a
    // peak steep enough to fail the slope test is a wall, not a floor — so the
    // ceiling is a bound, not an equality.
    NF_CHECK(mesh_max <= terrain_max + 0.5f);
    // And the surface it produced is the hill, not the volume floor: the
    // highest walkable point has to be above the lowest point of the field.
    NF_CHECK(mesh_max > terrain_min + 0.5f);
    // The bottom is the volume's own floor. Boundary columns take the area
    // floor as a corner height (the module's corner rule), so the mesh can dip
    // to 0 at its edge; what it must never do is sink BELOW the floor it was
    // given.
    NF_CHECK(mesh_min >= -0.01f);

    // And the whole surface is not a flat slab at the volume floor: the mean
    // height of a mesh over hills is above the lowest point of the field.
    const f32 mean = average_height(*mesh);
    NF_CHECK(mean > terrain_min + 0.2f);
}

NF_TEST(a_scene_without_terrain_still_bakes_a_flat_floor) {
    HeadlessRuntime h("nf_terrain_flat");
    if (!h.ok()) {
        NF_SKIP("no Vulkan device");
    }
    Scene s("FlatBake");
    ecs::Entity volume = s.world().create_entity();
    s.world().add<Transform>(volume, Transform{});
    NavMeshComponent nm;
    nm.area_x = 12.0f;
    nm.area_y = 8.0f;
    nm.area_z = 12.0f;
    s.world().add<NavMeshComponent>(volume, nm);

    h.runtime->adopt_scene(std::make_unique<Scene>(std::move(s)),
                           "content://Scenes/FlatBake.nfscene");
    NF_CHECK(h.runtime->navmesh_built());
    const ai::NavMesh* mesh = h.runtime->navmesh();
    NF_CHECK(mesh != nullptr);
    if (mesh == nullptr) {
        return;
    }
    // Every corner at y = 0, exactly as every scene baked before Phase 63.
    for (u32 p = 0; p < mesh->polygon_count(); ++p) {
        for (const Vec3& v : mesh->polygon(p).verts) {
            NF_CHECK_NEAR(v.y, 0.0f, 1e-3f);
        }
    }
}

NF_TEST(authored_links_reach_the_bake) {
    HeadlessRuntime h("nf_terrain_links");
    if (!h.ok()) {
        NF_SKIP("no Vulkan device");
    }
    Scene s("LinkBake");
    ecs::Entity volume = s.world().create_entity();
    s.world().add<Transform>(volume, Transform{});
    NavMeshComponent nm;
    nm.area_x = 20.0f;
    nm.area_y = 10.0f;
    nm.area_z = 20.0f;
    // Two ends on the same flat floor: the bake can snap both, so the link
    // survives. A link whose ends are on the mesh is what "reaches the bake"
    // means; the AITests suite owns the A* behaviour through it.
    NavLinkSpec link;
    link.start_x = 4.0f;
    link.start_z = 4.0f;
    link.end_x = 16.0f;
    link.end_z = 16.0f;
    nm.links.push_back(link);
    s.world().add<NavMeshComponent>(volume, nm);

    h.runtime->adopt_scene(std::make_unique<Scene>(std::move(s)),
                           "content://Scenes/LinkBake.nfscene");
    NF_CHECK(h.runtime->navmesh_built());
    const ai::NavMesh* mesh = h.runtime->navmesh();
    NF_CHECK(mesh != nullptr);
    if (mesh == nullptr) {
        return;
    }
    NF_CHECK(mesh->link_count() >= 1u);
    NF_CHECK_EQ(mesh->dropped_link_count(), 0u);
}
