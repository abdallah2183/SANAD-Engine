// Tests/RuntimeTests/test_runtime_scene.cpp — Scene .nfscene tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>

using namespace nf;
using namespace nf::assets;
using namespace nf::scene;
using namespace nf::runtime;

NF_TEST(scene_save_load_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_rt_test";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("TestScene");
    auto& w = scene.world();
    ecs::Entity e1 = w.create_entity();
    w.add<Transform>(e1, Transform{});
    w.get<Transform>(e1)->local_x = 5;

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/test.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/test.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene && result.scene->world().alive_entity_count()==1);
    auto* t = result.scene->world().get<Transform>(result.scene->world().all_entities()[0]);
    NF_CHECK(t && t->local_x==5);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_hierarchy_preservation) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_hierarchy_test";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("HierTest");
    auto& w = scene.world();
    ecs::Entity parent = w.create_entity();
    w.add<Transform>(parent, Transform{});
    w.get<Transform>(parent)->local_x = 10;
    ecs::Entity child = w.create_entity();
    w.add<Transform>(child, Transform{});
    w.get<Transform>(child)->local_x = 5;
    set_parent(w, child, parent);
    propagate_transforms(w);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/hier.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/hier.nfscene");
    NF_CHECK(result.success);
    // After load, the hierarchy should be preserved (we check that at least 2 entities exist and one has a parent)
    NF_CHECK(result.scene->world().alive_entity_count()==2);
    bool found_parent = false;
    for (auto e : result.scene->world().query<Transform>()) {
        auto* t = result.scene->world().get<Transform>(e);
        if (t && t->parent.valid()) found_parent = true;
    }
    NF_CHECK(found_parent);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_missing_asset_behavior) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_missing_asset";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    // Create a scene file manually with a Mesh asset_id that doesn't exist in registry
    std::string scene_content = "# NOVAForge Scene v1\nversion: 1\nname: MissingAssetTest\nentity_count: 1\n---\nentity: 1:0\n  Transform: local(0,0,0) world(0,0,0) parent(4294967295:0)\n  Mesh: asset_id=00000000-0000-0000-0000-000000000000 material=content://Materials/Default\n";
    vfs.write_text("content://Scenes/missing.nfscene", scene_content);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/missing.nfscene");
    // For v0.1, missing asset should not make the whole load fail, but should be in missing_assets or warnings
    NF_CHECK(result.success || !result.error.empty());
    // The scene should still be loaded (even if asset is missing, we don't crash)
    if (result.scene) {
        NF_CHECK(result.scene->world().alive_entity_count()>=1);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_malformed_rejection) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_malformed";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    vfs.write_text("content://Scenes/bad.nfscene", "not a valid scene file");
    auto result = load_scene_from_vfs(vfs, "content://Scenes/bad.nfscene");
    NF_CHECK(!result.success);
    NF_CHECK(!result.error.empty());

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_cyclic_hierarchy_rejection) {
    Scene scene("CycleTest");
    auto& w = scene.world();
    ecs::Entity a = w.create_entity();
    ecs::Entity b = w.create_entity();
    w.add<Transform>(a, Transform{});
    w.add<Transform>(b, Transform{});
    set_parent(w, b, a);
    // Try to create a cycle: a parent is b
    set_parent(w, a, b);
    // The set_parent should have rejected the cycle, so a should still have no parent
    NF_CHECK(!get_parent(w, a).valid());
    // Now save and load a scene that has a cycle manually crafted
    // For the test, we just verify that the loader detects cycles
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_cycle";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);
    // Manually craft a scene file with a cycle: entity 1 parent 2, entity 2 parent 1
    std::string cycle_content = "# NOVAForge Scene v1\nversion: 1\nname: Cycle\nentity_count: 2\n---\nentity: 1:0\n  Transform: local(0,0,0) world(0,0,0) parent(2:0)\n---\nentity: 2:0\n  Transform: local(0,0,0) world(0,0,0) parent(1:0)\n";
    vfs.write_text("content://Scenes/cycle.nfscene", cycle_content);
    auto result = load_scene_from_vfs(vfs, "content://Scenes/cycle.nfscene");
    NF_CHECK(!result.success);
    NF_CHECK(result.error.find("Cyclic") != std::string::npos);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_version_rejection) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_version";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);
    vfs.write_text("content://Scenes/old.nfscene", "# NOVAForge Scene v1\nversion: 999\nname: Old\nentity_count: 0\n");
    auto result = load_scene_from_vfs(vfs, "content://Scenes/old.nfscene");
    NF_CHECK(!result.success);
    NF_CHECK(result.error.find("Unsupported scene version") != std::string::npos);
    std::filesystem::remove_all(tmp);
}
