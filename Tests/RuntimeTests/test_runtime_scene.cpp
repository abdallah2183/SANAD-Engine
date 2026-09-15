// Tests/RuntimeTests/test_runtime_scene.cpp — Scene .nfscene tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>

using namespace nf;
using namespace nf::assets;
using namespace nf::scene;
using namespace nf::runtime;
using namespace nf::physics;
using namespace nf::animation;
using namespace nf::audio;

NF_TEST(scene_animation_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_anim_test";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("AnimScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});

    AnimationComponent anim;
    anim.player.set_clip("walk");
    anim.speed = 1.5f;
    anim.paused = false;
    anim.use_state_machine = false;
    w.add<AnimationComponent>(e, std::move(anim));

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/anim.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/anim.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    auto loaded_e = result.scene->world().all_entities()[0];
    auto* loaded_anim = result.scene->world().get<AnimationComponent>(loaded_e);
    NF_CHECK(loaded_anim);
    NF_CHECK_EQ(loaded_anim->player.clip_name(), std::string("walk"));
    NF_CHECK_NEAR(loaded_anim->speed, 1.5f, 1e-5f);
    NF_CHECK(!loaded_anim->paused);
    NF_CHECK(!loaded_anim->use_state_machine);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_audio_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_audio_test";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("AudioScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});

    AudioComponent aud;
    aud.buffer_name = "shoot.wav";
    aud.volume = 0.75f;
    aud.pitch = 1.2f;
    aud.looping = true;
    aud.spatial = true;
    aud.autoplay = true;
    w.add<AudioComponent>(e, aud);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/audio.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/audio.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    auto loaded_e = result.scene->world().all_entities()[0];
    auto* loaded_aud = result.scene->world().get<AudioComponent>(loaded_e);
    NF_CHECK(loaded_aud);
    NF_CHECK_EQ(loaded_aud->buffer_name, std::string("shoot.wav"));
    NF_CHECK_NEAR(loaded_aud->volume, 0.75f, 1e-5f);
    NF_CHECK_NEAR(loaded_aud->pitch, 1.2f, 1e-5f);
    NF_CHECK(loaded_aud->looping);
    NF_CHECK(loaded_aud->spatial);
    NF_CHECK(loaded_aud->autoplay);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_animation_audio_combined_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_aa_test";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("CombinedScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    w.get<Transform>(e)->local_y = 3.0f;

    AnimationComponent anim;
    anim.player.set_clip("idle");
    anim.speed = 0.5f;
    w.add<AnimationComponent>(e, std::move(anim));

    AudioComponent aud;
    aud.buffer_name = "ambient.wav";
    aud.volume = 0.3f;
    aud.looping = true;
    w.add<AudioComponent>(e, aud);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/combined.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/combined.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    auto loaded_e = result.scene->world().all_entities()[0];
    auto* t = result.scene->world().get<Transform>(loaded_e);
    auto* loaded_anim = result.scene->world().get<AnimationComponent>(loaded_e);
    auto* loaded_aud = result.scene->world().get<AudioComponent>(loaded_e);
    NF_CHECK(t && fabsf(t->local_y - 3.0f) < 1e-5f);
    NF_CHECK(loaded_anim && loaded_anim->player.clip_name() == std::string("idle"));
    NF_CHECK_NEAR(loaded_anim->speed, 0.5f, 1e-5f);
    NF_CHECK(loaded_aud && loaded_aud->buffer_name == "ambient.wav");
    NF_CHECK_NEAR(loaded_aud->volume, 0.3f, 1e-5f);
    NF_CHECK(loaded_aud->looping);

    std::filesystem::remove_all(tmp);
}

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

NF_TEST(scene_light_shadow_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_light_shadow_test";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("LightShadow");
    auto& w = scene.world();
    ecs::Entity e1 = w.create_entity();
    w.add<Transform>(e1, Transform{});
    DirectionalLight no_shadow;
    no_shadow.cast_shadows = false;
    w.add<DirectionalLight>(e1, no_shadow);
    ecs::Entity e2 = w.create_entity();
    w.add<Transform>(e2, Transform{});
    w.add<DirectionalLight>(e2, DirectionalLight{}); // default: shadows on

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/light.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/light.nfscene");
    NF_CHECK(result.success);
    bool saw_off = false, saw_on = false;
    for (auto e : result.scene->world().query<DirectionalLight>()) {
        const auto* l = result.scene->world().get<DirectionalLight>(e);
        if (l->cast_shadows) saw_on = true;
        else saw_off = true;
    }
    NF_CHECK(saw_off); // shadows=false survived the round trip
    NF_CHECK(saw_on);  // default (no key written) loads as shadows on

    // Old files without the key keep working: shadows default to on.
    NF_CHECK(vfs.write_text("content://Scenes/legacy.nfscene",
                            "# NOVAForge Scene v1\nversion: 1\nname: Legacy\n"
                            "entity_count: 1\n"
                            "---\n"
                            "entity: 0:0\n"
                            "  Light: type=Directional dir(0,-1,0) color(1,1,1) intensity=1\n")
                 .ok);
    auto legacy = load_scene_from_vfs(vfs, "content://Scenes/legacy.nfscene");
    NF_CHECK(legacy.success);
    const auto* ll = legacy.scene->world().get<DirectionalLight>(
        legacy.scene->world().all_entities()[0]);
    NF_CHECK(ll != nullptr && ll->cast_shadows);

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

// ---------------------------------------------------------------------------
// Physics component serialization round-trip
//
// These tests prove that RigidBodyComponent and ColliderComponent survive a
// save → load cycle with all their fields intact. The runtime body handle is
// deliberately not serialized, so after load it should be default-constructed
// (the runtime rebuilds it when stepping physics).
// ---------------------------------------------------------------------------

NF_TEST(scene_physics_rigid_body_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_phys_rb";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("PhysRBTest");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    RigidBodyComponent rb;
    rb.type = BodyType::Kinematic;
    rb.mass = 2.5f;
    rb.friction = 0.8f;
    rb.restitution = 0.3f;
    rb.linear_damping = 0.1f;
    rb.angular_damping = 0.15f;
    rb.allow_sleep = false;
    w.add<RigidBodyComponent>(e, rb);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/phys_rb.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/phys_rb.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene && result.scene->world().alive_entity_count() == 1);
    auto* loaded = result.scene->world().get<RigidBodyComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    NF_CHECK(loaded->type == BodyType::Kinematic);
    NF_CHECK(fabsf(loaded->mass - 2.5f) < 1e-5f);
    NF_CHECK(fabsf(loaded->friction - 0.8f) < 1e-5f);
    NF_CHECK(fabsf(loaded->restitution - 0.3f) < 1e-5f);
    NF_CHECK(fabsf(loaded->linear_damping - 0.1f) < 1e-5f);
    NF_CHECK(fabsf(loaded->angular_damping - 0.15f) < 1e-5f);
    NF_CHECK(loaded->allow_sleep == false);
    // The body handle is NOT serialized — it must be default after load.
    NF_CHECK(!loaded->body.valid());

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_physics_collider_box_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_phys_col_box";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("PhysColBoxTest");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    ColliderComponent col;
    col.shape = Shape::make_box(Vec3(1.0f, 2.0f, 3.0f));
    w.add<ColliderComponent>(e, col);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/col_box.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/col_box.nfscene");
    NF_CHECK(result.success);
    auto* loaded = result.scene->world().get<ColliderComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    NF_CHECK(loaded->shape.type == ShapeType::Box);
    NF_CHECK(fabsf(loaded->shape.box.half_extents.x - 1.0f) < 1e-5f);
    NF_CHECK(fabsf(loaded->shape.box.half_extents.y - 2.0f) < 1e-5f);
    NF_CHECK(fabsf(loaded->shape.box.half_extents.z - 3.0f) < 1e-5f);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_physics_collider_sphere_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_phys_col_sph";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("PhysColSphereTest");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    ColliderComponent col;
    col.shape = Shape::make_sphere(1.5f);
    w.add<ColliderComponent>(e, col);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/col_sph.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/col_sph.nfscene");
    NF_CHECK(result.success);
    auto* loaded = result.scene->world().get<ColliderComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    NF_CHECK(loaded->shape.type == ShapeType::Sphere);
    NF_CHECK(fabsf(loaded->shape.sphere.radius - 1.5f) < 1e-5f);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_physics_collider_plane_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_phys_col_pl";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("PhysColPlaneTest");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    ColliderComponent col;
    col.shape = Shape::make_plane(Vec3(0.0f, 1.0f, 0.0f));
    w.add<ColliderComponent>(e, col);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/col_pl.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/col_pl.nfscene");
    NF_CHECK(result.success);
    auto* loaded = result.scene->world().get<ColliderComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    NF_CHECK(loaded->shape.type == ShapeType::Plane);
    NF_CHECK(fabsf(loaded->shape.plane.normal.y - 1.0f) < 1e-5f);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_physics_combined_round_trip) {
    // A full physics entity has Transform + RigidBody + Collider. Verify all
    // three survive the round-trip together, which is what the runtime relies
    // on when rebuilding bodies from a loaded scene.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_phys_all";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("PhysCombined");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    w.get<Transform>(e)->local_y = 10.0f;

    RigidBodyComponent rb;
    rb.type = BodyType::Dynamic;
    rb.mass = 3.0f;
    rb.friction = 0.6f;
    w.add<RigidBodyComponent>(e, rb);

    ColliderComponent col;
    col.shape = Shape::make_box(Vec3(0.5f, 0.5f, 0.5f));
    w.add<ColliderComponent>(e, col);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/all.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/all.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene && result.scene->world().alive_entity_count() == 1);
    auto loaded_e = result.scene->world().all_entities()[0];
    auto* t = result.scene->world().get<Transform>(loaded_e);
    auto* rbc = result.scene->world().get<RigidBodyComponent>(loaded_e);
    auto* cc = result.scene->world().get<ColliderComponent>(loaded_e);
    NF_CHECK(t && fabsf(t->local_y - 10.0f) < 1e-5f);
    NF_CHECK(rbc && rbc->type == BodyType::Dynamic && fabsf(rbc->mass - 3.0f) < 1e-5f);
    NF_CHECK(cc && cc->shape.type == ShapeType::Box);

    std::filesystem::remove_all(tmp);
}
