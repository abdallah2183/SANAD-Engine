// RuntimeTests — projected decals as scene data (Phase 36).
//
// Two contracts, both about reachability rather than library internals:
//
//   1. `Decal:` parses, saves and round-trips; a scene that never mentions one
//      writes no line, and a one-key line keeps the documented defaults. The
//      loader is also allowed to *refuse* a line — an impossible size must
//      produce a warning that names the key instead of silently creating an
//      invisible decal.
//   2. The Runtime extracts enabled decals into `RenderWorld::decals` with the
//      entity Transform (position AND facing) and the authored size baked into
//      the world matrix. A component the runtime parses and drops is precisely
//      the "registered but never driven" failure this file exists to catch.
//
// The GPU half lives in Tests/RHITests/test_decals.cpp: that suite proves the
// pass composites in the right order and preserves the normal/emissive buffers.
// This one only proves the scene says what the renderer will receive.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Decals.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>

#include <cmath>
#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::assets;
using namespace nf::scene;
using namespace nf::runtime;

namespace {

float distance_between(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

NF_TEST(scene_decal_round_trips_through_the_file) {
    // Every field through the real writer and the real reader. A key the writer
    // emits but the parser never reads (or the reverse) is exactly the drift a
    // hand-written fixture cannot see.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_decal_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("DecalScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});

    DecalComponent decal;
    decal.material = "content://Materials/scorch.nfmat";
    decal.size_x = 2.5f;
    decal.size_y = 3.75f;
    decal.size_z = 0.6f;
    decal.opacity = 0.35f;
    decal.enabled = false;
    w.add<DecalComponent>(e, decal);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/decal.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/decal.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }

    const auto* loaded = result.scene->world().get<DecalComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded);
    if (loaded != nullptr) {
        NF_CHECK_EQ(loaded->material, std::string("content://Materials/scorch.nfmat"));
        NF_CHECK_NEAR(loaded->size_x, 2.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->size_y, 3.75f, 1e-5f);
        NF_CHECK_NEAR(loaded->size_z, 0.6f, 1e-5f);
        NF_CHECK_NEAR(loaded->opacity, 0.35f, 1e-5f);
        NF_CHECK(!loaded->enabled);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_without_a_decal_writes_no_line) {
    // The compatibility half: a scene authored before decals existed must save
    // to bytes that still contain no `Decal:` keyword, so loading and saving an
    // old level is a genuine no-op rather than an edit.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_decal_absent";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("PlainScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/plain.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/plain.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }

    const std::string text = serialize_scene_to_text(*result.scene);
    NF_CHECK(text.find("Decal:") == std::string::npos);
    NF_CHECK(result.scene->world().get<DecalComponent>(
                  result.scene->world().all_entities()[0]) == nullptr);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_decal_line_keeps_defaults_for_absent_keys) {
    // Hand-written lines are the common case, so a line that names only the
    // material must still be a complete decal: 1x1x1, fully opaque, enabled.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_decal_min";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: DecalMin\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Patch\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  Decal: material=content://Materials/patch.nfmat\n";
    NF_CHECK(vfs.write_text("content://Scenes/min.nfscene", text).ok);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/min.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }

    const auto* decal = result.scene->world().get<DecalComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(decal);
    if (decal != nullptr) {
        NF_CHECK_EQ(decal->material, std::string("content://Materials/patch.nfmat"));
        NF_CHECK_NEAR(decal->size_x, 1.0f, 1e-6f);
        NF_CHECK_NEAR(decal->size_y, 1.0f, 1e-6f);
        NF_CHECK_NEAR(decal->size_z, 1.0f, 1e-6f);
        NF_CHECK_NEAR(decal->opacity, 1.0f, 1e-6f);
        NF_CHECK(decal->enabled);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_decal_rejects_an_impossible_size_and_names_it) {
    // A zero-width decal has no volume, so every fragment fails the box test and
    // the author sees nothing. The loader refuses the line instead, and the
    // warning must say *which* key was wrong: "invalid ''" would send the author
    // hunting through a line full of numbers.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_decal_bad_size";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: DecalBad\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Bad\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  Decal: material=content://Materials/patch.nfmat size(0,1,1) opacity=0.5\n";
    NF_CHECK(vfs.write_text("content://Scenes/bad.nfscene", text).ok);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/bad.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }

    NF_CHECK(result.scene->world().get<DecalComponent>(
                  result.scene->world().all_entities()[0]) == nullptr);

    bool named_key = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("Decal") != std::string::npos &&
            warning.find("'size'") != std::string::npos) {
            named_key = true;
            break;
        }
    }
    NF_CHECK(named_key);

    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_extracts_enabled_decals_with_transform_and_size) {
    // The integration claim. The component round-tripping is not enough: the
    // renderer draws `RenderWorld::decals`, and only the Runtime builds it.
    //
    // Orientation is the subtle part. Local +Z is the decal's facing direction
    // and the authored size is baked into the world matrix, so the assertion
    // walks the matrix instead of trusting a translation-only check:
    //   * translation lands on the entity (2,0,0);
    //   * the local +Z row is world (+1,0,0) for a +90 degree Y rotation;
    //   * transforming unit local axes yields the authored 2/4/1 extents.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_rt_decal_extract";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: DecalRuntime\nentity_count: 2\n"
        "---\nentity: 1:0\n  Name: Scorch\n"
        "  Transform: local(2,0,0) world(2,0,0) rot(0,90,0) scale(1,1,1) parent(4294967295:0)\n"
        "  Decal: material=content://Materials/scorch.nfmat size(2,4,1) opacity=0.5\n"
        "---\nentity: 2:0\n  Name: Hidden\n"
        "  Transform: local(6,0,0) world(6,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  Decal: size(3,3,1) opacity=1 enabled=false\n";
    NF_CHECK(vfs.write_text("content://Scenes/decals.nfscene", text).ok);

    auto device = rhi::create_device();
    if (!device) {
        std::filesystem::remove_all(tmp);
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        std::filesystem::remove_all(tmp);
        NF_SKIP("headless Vulkan device init failed");
    }

    {
        AssetRegistry reg;
        AssetManager manager(vfs, reg);
        Runtime rt(vfs, reg, manager, *device, nullptr);
        std::string err;
        NF_CHECK(rt.load_scene("content://Scenes/decals.nfscene", err));

        auto world = rt.render_world_snapshot();
        // The disabled decal is not extracted at all: the renderer must not
        // spend a draw on something the author explicitly turned off.
        NF_CHECK_EQ(world.decals.size(), static_cast<size_t>(1));
        if (world.decals.size() == 1) {
            const rendering::DecalInstance& decal = world.decals[0];
            NF_CHECK(decal.enabled);
            NF_CHECK_NEAR(decal.opacity, 0.5f, 1e-5f);

            const Vec3 translation{decal.world.m[3][0], decal.world.m[3][1],
                                   decal.world.m[3][2]};
            NF_CHECK_NEAR(translation.x, 2.0f, 1e-5f);
            NF_CHECK_NEAR(translation.y, 0.0f, 1e-5f);
            NF_CHECK_NEAR(translation.z, 0.0f, 1e-5f);

            // For a +90 degree Y rotation the entity's local +Z (the decal's
            // facing direction) points down world +X.
            NF_CHECK_NEAR(decal.world.m[2][0], 1.0f, 1e-5f);
            NF_CHECK_NEAR(decal.world.m[2][1], 0.0f, 1e-5f);
            NF_CHECK_NEAR(decal.world.m[2][2], 0.0f, 1e-5f);

            // The size is baked in, not left for the shader: unit local axes
            // become the authored 2/4/1 extents in world space.
            const Vec3 unit_x{decal.world.m[0][0], decal.world.m[0][1], decal.world.m[0][2]};
            const Vec3 unit_y{decal.world.m[1][0], decal.world.m[1][1], decal.world.m[1][2]};
            const Vec3 unit_z{decal.world.m[2][0], decal.world.m[2][1], decal.world.m[2][2]};
            NF_CHECK_NEAR(distance_between(unit_x, Vec3{0, 0, 0}), 2.0f, 1e-5f);
            NF_CHECK_NEAR(distance_between(unit_y, Vec3{0, 0, 0}), 4.0f, 1e-5f);
            NF_CHECK_NEAR(distance_between(unit_z, Vec3{0, 0, 0}), 1.0f, 1e-5f);

            // Sanity-check the box contract the shader relies on: the authored
            // +X extent is 2, so the local (+0.5,0,0) corner sits 1 unit from
            // the centre. (The entity's rotation carries it to world -Z.)
            const Vec3 corner{decal.world.m[3][0] + decal.world.m[0][0] * 0.5f,
                              decal.world.m[3][1] + decal.world.m[0][1] * 0.5f,
                              decal.world.m[3][2] + decal.world.m[0][2] * 0.5f};
            NF_CHECK_NEAR(distance_between(corner, translation), 1.0f, 1e-5f);
            NF_CHECK_NEAR(corner.z, -1.0f, 1e-5f);
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
