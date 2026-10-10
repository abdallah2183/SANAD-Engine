// RuntimeTests — SkyComponent + shadow tuning scene round-trip.
//
// Saves a scene carrying a SkyComponent and a shadow-tuned directional
// light, reloads it from VFS, and checks every field survived. Uses the
// same temp-dir VFS pattern as the other scene tests.

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
    light.shadow_cascades = 2;
    light.shadow_distance = 45.5f;
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
    NF_CHECK_EQ(loaded_light->shadow_cascades, 2u);
    NF_CHECK_NEAR(loaded_light->shadow_distance, 45.5f, 1e-5f);
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

// ---------------------------------------------------------------------------
// TimeOfDay — the day/night cycle as authored data (design doc §64)
//
// `rendering::TimeOfDay` was built, tested (Tests/RHITests/test_timeofday.cpp)
// and used by two hand-written samples — and unreachable from a scene. "Make the
// sun move and the sky follow" was a C++ edit. These tests close that: the clock
// is now a component, and the assertions are about the *shipped path* — a scene
// file in, a running clock and a changing sun out.
// ---------------------------------------------------------------------------

NF_TEST(scene_time_of_day_round_trips_through_the_file) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_tod_test";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("TodScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    w.add<SkyComponent>(e, SkyComponent{});

    TimeOfDayComponent tod;
    tod.time_hours = 17.25f;
    tod.day_length_seconds = 90.0f;
    tod.enabled = true;
    tod.drive_light = false;
    w.add<TimeOfDayComponent>(e, tod);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/tod.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/tod.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    const auto* loaded =
        result.scene->world().get<TimeOfDayComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(loaded);
    NF_CHECK_NEAR(loaded->time_hours, 17.25f, 1e-4f);
    NF_CHECK_NEAR(loaded->day_length_seconds, 90.0f, 1e-4f);
    NF_CHECK(loaded->enabled);
    // drive_light=false is the case worth pinning: it is how a scene takes the
    // moving sky without having its hand-posed key light swung around.
    NF_CHECK(!loaded->drive_light);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_time_of_day_line_wraps_hours_and_keeps_defaults) {
    // A `TimeOfDay:` line is written by hand as often as by the editor, so the
    // loader's tolerance is part of the contract: 25.0 means 1am, and an absent
    // key keeps the component default rather than resetting the whole line.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_tod_parse";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: Tod\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Sky\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  TimeOfDay: hours=25.0\n";
    NF_CHECK(vfs.write_text("content://Scenes/wrap.nfscene", text).ok);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/wrap.nfscene");
    NF_CHECK(result.success);
    const auto* tod =
        result.scene->world().get<TimeOfDayComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(tod);
    // 25.0 wraps to 1.0 rather than rejecting the scene.
    NF_CHECK_NEAR(tod->time_hours, 1.0f, 1e-4f);
    // An absent key keeps the default, so a one-key line is a complete line.
    NF_CHECK_NEAR(tod->day_length_seconds, 240.0f, 1e-4f);
    NF_CHECK(tod->enabled);
    NF_CHECK(tod->drive_light);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_without_time_of_day_writes_no_time_of_day_line) {
    // The compatibility half: a scene that has never heard of the component must
    // round-trip to exactly the bytes it was loaded from. A serializer that always
    // emitted the line would silently add a day/night cycle to every existing
    // level on the first save.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_tod_absent";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("NoTod");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    w.add<SkyComponent>(e, SkyComponent{});

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/notod.nfscene", scene, err));
    auto first = load_scene_from_vfs(vfs, "content://Scenes/notod.nfscene");
    NF_CHECK(first.success);

    // Re-serializing the loaded scene must produce no TimeOfDay line at all.
    const std::string text = serialize_scene_to_text(*first.scene);
    NF_CHECK(text.find("TimeOfDay:") == std::string::npos);
    NF_CHECK(first.scene->world().get<TimeOfDayComponent>(
                 first.scene->world().all_entities()[0]) == nullptr);

    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_advances_the_scene_clock_and_the_sun_follows_it) {
    // The end-to-end claim: a scene file carrying a cycle produces a clock that
    // actually moves, with no C++ host. Before this, only the samples could do it
    // and a game had to copy one to get a day/night cycle.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_tod_advance";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: Tod\nentity_count: 2\n"
        "---\nentity: 1:0\n  Name: Sun\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  Light: type=Directional dir(-0.4,-1,-0.25) color(1,1,1) intensity=1.0\n"
        "---\nentity: 2:0\n  Name: Sky\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  TimeOfDay: hours=6.0 day_length=240\n";
    NF_CHECK(vfs.write_text("content://Scenes/Tod.nfscene", text).ok);

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
        NF_CHECK(rt.load_scene("content://Scenes/Tod.nfscene", err));

        // Located by query rather than by assuming an entity id: the loader may
        // renumber, and a test that pins "entity 2" would fail for a reason that
        // has nothing to do with the clock.
        TimeOfDayComponent* tod = nullptr;
        for (auto e : rt.edit_scene()->world().all_entities()) {
            if (auto* c = rt.edit_scene()->world().get<TimeOfDayComponent>(e)) {
                tod = c;
                break;
            }
        }
        // The scene has no mesh, so the runtime creates no camera: the device is
        // still required because load_scene builds the renderer, but nothing is
        // drawn.
        NF_CHECK(tod);
        if (tod == nullptr) {
            device->wait_idle();
            device->shutdown();
            std::filesystem::remove_all(tmp);
            return;
        }
        NF_CHECK_NEAR(tod->time_hours, 6.0f, 1e-4f);

        // 240s per 24h = 10s per hour. 6.0h + 120s of play = 12.0h (noon).
        for (int i = 0; i < 120; ++i) {
            rt.update(1.0f);
        }
        NF_CHECK_NEAR(tod->time_hours, 18.0f, 0.05f);

        // Midnight: the clock wraps rather than running off the end of the day.
        for (int i = 0; i < 150; ++i) {
            rt.update(1.0f);
        }
        NF_CHECK(tod->time_hours >= 0.0f && tod->time_hours < 24.0f);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_frozen_clock_does_not_advance) {
    // day_length <= 0 is the documented "frozen" state: a fixed golden hour is a
    // legitimate authoring choice, so it must render the same frame forever
    // rather than being treated as a broken value.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_tod_frozen";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        "# SANAD Scene v1\nversion: 1\nname: Tod\nentity_count: 1\n"
        "---\nentity: 1:0\n  Name: Sky\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  TimeOfDay: hours=17.5 day_length=0\n";
    NF_CHECK(vfs.write_text("content://Scenes/Frozen.nfscene", text).ok);

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
        NF_CHECK(rt.load_scene("content://Scenes/Frozen.nfscene", err));

        TimeOfDayComponent* tod = nullptr;
        for (auto e : rt.edit_scene()->world().all_entities()) {
            if (auto* c = rt.edit_scene()->world().get<TimeOfDayComponent>(e)) {
                tod = c;
                break;
            }
        }
        NF_CHECK(tod);
        if (tod != nullptr) {
            for (int i = 0; i < 60; ++i) {
                rt.update(1.0f);
            }
            // Exactly where the author put it.
            NF_CHECK_NEAR(tod->time_hours, 17.5f, 1e-4f);
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_old_light_line_keeps_shadow_defaults) {
    // A Light line without shadow keys must load with strength 1 and the standard
    // bias: old scenes render exactly as before. This also covers the default
    // cascade settings, since save writes those keys only when a scene departs
    // from them — so a default light round-trips through a file that never
    // mentions cascades at all, which is what a pre-cascade scene is.
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
    NF_CHECK_EQ(loaded_light->shadow_cascades, 4u);
    NF_CHECK_NEAR(loaded_light->shadow_distance, 0.0f, 1e-7f);
    NF_CHECK(result.scene->world().get<SkyComponent>(loaded_e) == nullptr);

    std::filesystem::remove_all(tmp);
}
