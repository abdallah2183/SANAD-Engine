// RuntimeTests — SkyComponent + shadow tuning scene round-trip.
//
// Saves a scene carrying a SkyComponent and a shadow-tuned directional
// light, reloads it from VFS, and checks every field survived. Uses the
// same temp-dir VFS pattern as the other scene tests.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>

using namespace nf;
using namespace nf::assets;
using namespace nf::scene;
using namespace nf::runtime;

NF_TEST(scene_sky_and_shadow_tuning_round_trip) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_sky_test";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("SkyScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});

    DirectionalLight light;
    light.cast_shadows = true;
    light.shadow_strength = 0.5f;
    light.shadow_bias = 0.001f;
    w.add<DirectionalLight>(e, light);

    SkyComponent sky;
    sky.zenith_r = 0.1f;
    sky.zenith_g = 0.2f;
    sky.zenith_b = 0.3f;
    sky.horizon_r = 0.4f;
    sky.horizon_g = 0.5f;
    sky.horizon_b = 0.6f;
    sky.ground_r = 0.01f;
    sky.ground_g = 0.02f;
    sky.ground_b = 0.03f;
    sky.sun_disk = 2.0f;
    sky.sun_glow = 0.5f;
    sky.enabled = false;
    w.add<SkyComponent>(e, sky);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/sky.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/sky.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    auto loaded_e = result.scene->world().all_entities()[0];

    const auto* loaded_light = result.scene->world().get<DirectionalLight>(loaded_e);
    NF_CHECK(loaded_light);
    NF_CHECK_NEAR(loaded_light->shadow_strength, 0.5f, 1e-5f);
    NF_CHECK_NEAR(loaded_light->shadow_bias, 0.001f, 1e-6f);
    NF_CHECK(loaded_light->cast_shadows);

    const auto* loaded_sky = result.scene->world().get<SkyComponent>(loaded_e);
    NF_CHECK(loaded_sky);
    NF_CHECK_NEAR(loaded_sky->zenith_r, 0.1f, 1e-5f);
    NF_CHECK_NEAR(loaded_sky->zenith_g, 0.2f, 1e-5f);
    NF_CHECK_NEAR(loaded_sky->zenith_b, 0.3f, 1e-5f);
    NF_CHECK_NEAR(loaded_sky->horizon_r, 0.4f, 1e-5f);
    NF_CHECK_NEAR(loaded_sky->horizon_g, 0.5f, 1e-5f);
    NF_CHECK_NEAR(loaded_sky->horizon_b, 0.6f, 1e-5f);
    NF_CHECK_NEAR(loaded_sky->ground_r, 0.01f, 1e-6f);
    NF_CHECK_NEAR(loaded_sky->ground_g, 0.02f, 1e-6f);
    NF_CHECK_NEAR(loaded_sky->ground_b, 0.03f, 1e-6f);
    NF_CHECK_NEAR(loaded_sky->sun_disk, 2.0f, 1e-5f);
    NF_CHECK_NEAR(loaded_sky->sun_glow, 0.5f, 1e-5f);
    NF_CHECK(!loaded_sky->enabled);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_old_light_line_keeps_shadow_defaults) {
    // A pre-Phase-13 Light line (no shadow keys) must load with strength 1
    // and the standard bias: old scenes render exactly as before.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_sky_compat_test";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("CompatScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    w.add<DirectionalLight>(e, DirectionalLight{});

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/compat.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/compat.nfscene");
    NF_CHECK(result.success);
    auto loaded_e = result.scene->world().all_entities()[0];
    const auto* loaded_light = result.scene->world().get<DirectionalLight>(loaded_e);
    NF_CHECK(loaded_light);
    NF_CHECK_NEAR(loaded_light->shadow_strength, 1.0f, 1e-6f);
    NF_CHECK_NEAR(loaded_light->shadow_bias, 0.0005f, 1e-7f);
    NF_CHECK(result.scene->world().get<SkyComponent>(loaded_e) == nullptr);

    std::filesystem::remove_all(tmp);
}
