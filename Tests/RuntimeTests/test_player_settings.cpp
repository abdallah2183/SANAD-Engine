// Tests/RuntimeTests/test_player_settings.cpp — the settings that belong to
// the player, not to the level.
//
// The scene authors sensitivity, inverted-Y and FOV, and that is right: a
// level shows the framing it was built for. Two players at one machine still
// disagree about all three. The contract these cases pin:
//
//   1. a file that says nothing leaves the scene exactly as authored;
//   2. a player value OVERRIDES without editing the scene;
//   3. a broken file changes nothing and says which line is wrong;
//   4. unknown keys are ignored, so a newer file still loads in an older build.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/PlayerSettings.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;

namespace {

constexpr float kEps = 1e-4f;

} // namespace

NF_TEST(player_settings_default_to_the_scene) {
    const PlayerSettings s;
    NF_CHECK_NEAR(s.look_scale, 1.0f, kEps);
    NF_CHECK_NEAR(s.stick_scale, 1.0f, kEps);
    NF_CHECK(s.invert_y == InvertY::Scene);
    NF_CHECK_NEAR(s.fov_y, 0.0f, kEps);
    NF_CHECK_NEAR(s.ads_scale, 1.0f, kEps);
    NF_CHECK_NEAR(s.shake_scale, 1.0f, kEps);

    // Dynamics are OFF and neutral by default: a player who never opens the
    // settings file hears the author's master, not a house curve.
    NF_CHECK(!s.dynamics.compressor.enabled);
    NF_CHECK(!s.dynamics.limiter.enabled);
    NF_CHECK_NEAR(s.dynamics.compressor.makeup_db, 0.0f, kEps);
    NF_CHECK_NEAR(s.dynamics.limiter.ceiling_db, -0.3f, kEps);
}

NF_TEST(player_settings_parse_every_key_and_ignore_unknown_ones) {
    const std::string text =
        "# a player's taste\n"
        "version: 1\n"
        "look_scale: 1.4   # faster than the scene\n"
        "stick_scale: 0.8\n"
        "invert_y: on\n"
        "fov: 100\n"
        "ads_scale: 0.9\n"
        "shake_scale: 0\n"
        "audio.dynamics.compressor: on\n"
        "audio.dynamics.threshold_db: -24\n"
        "audio.dynamics.ratio: 4\n"
        "audio.dynamics.attack_ms: 5\n"
        "audio.dynamics.release_ms: 250\n"
        "audio.dynamics.knee_db: 8\n"
        "audio.dynamics.makeup_db: 3\n"
        "audio.dynamics.limiter: on\n"
        "audio.dynamics.ceiling_db: -1\n"
        "audio.dynamics.limiter_release_ms: 120\n"
        "audio.dynamics.lookahead_ms: 4\n"
        "future_key: whatever\n"; // must not fail the file

    PlayerSettings s;
    std::string err;
    NF_CHECK(parse_player_settings(text, s, err));
    NF_CHECK(err.empty());
    NF_CHECK_NEAR(s.look_scale, 1.4f, kEps);
    NF_CHECK_NEAR(s.stick_scale, 0.8f, kEps);
    NF_CHECK(s.invert_y == InvertY::On);
    NF_CHECK_NEAR(s.fov_y, 100.0f, kEps);
    NF_CHECK_NEAR(s.ads_scale, 0.9f, kEps);
    NF_CHECK_NEAR(s.shake_scale, 0.0f, kEps);
    NF_CHECK(s.dynamics.compressor.enabled);
    NF_CHECK(s.dynamics.limiter.enabled);
    NF_CHECK_NEAR(s.dynamics.compressor.threshold_db, -24.0f, kEps);
    NF_CHECK_NEAR(s.dynamics.compressor.ratio, 4.0f, kEps);
    NF_CHECK_NEAR(s.dynamics.compressor.attack_ms, 5.0f, kEps);
    NF_CHECK_NEAR(s.dynamics.compressor.release_ms, 250.0f, kEps);
    NF_CHECK_NEAR(s.dynamics.compressor.knee_db, 8.0f, kEps);
    NF_CHECK_NEAR(s.dynamics.compressor.makeup_db, 3.0f, kEps);
    NF_CHECK_NEAR(s.dynamics.limiter.ceiling_db, -1.0f, kEps);
    NF_CHECK_NEAR(s.dynamics.limiter.release_ms, 120.0f, kEps);
    NF_CHECK_NEAR(s.dynamics.limiter.lookahead_ms, 4.0f, kEps);

    // "auto" and "scene" both mean "the player has no opinion".
    PlayerSettings auto_settings;
    NF_CHECK(parse_player_settings("invert_y: auto\n", auto_settings, err));
    NF_CHECK(auto_settings.invert_y == InvertY::Scene);
    NF_CHECK(parse_player_settings("invert_y: scene\n", auto_settings, err));
    NF_CHECK(auto_settings.invert_y == InvertY::Scene);
}

NF_TEST(a_broken_player_settings_file_names_its_line_and_changes_nothing) {
    PlayerSettings s;
    s.look_scale = 2.0f; // a previous value the caller expects to survive
    std::string err;

    NF_CHECK(!parse_player_settings("look_scale: not-a-number\n", s, err));
    NF_CHECK(err.find("line 1") != std::string::npos);
    NF_CHECK_NEAR(s.look_scale, 2.0f, kEps); // untouched

    // A zero sensitivity would freeze the camera; it is refused rather than
    // clamped, so the player is told instead of quietly getting a dead mouse.
    NF_CHECK(!parse_player_settings("look_scale: 0\n", s, err));
    NF_CHECK(!parse_player_settings("fov: 5\n", s, err));
    NF_CHECK(!parse_player_settings("invert_y: sideways\n", s, err));
    NF_CHECK(!parse_player_settings("no colon here\n", s, err));
    NF_CHECK(!parse_player_settings("version: 2\n", s, err));
    // shake_scale 0 IS legal — a player who cannot tolerate shake.
    NF_CHECK(parse_player_settings("shake_scale: 0\n", s, err));

    // Dynamics get the same treatment: a bad value names its line and changes
    // nothing, and a misspelled control inside the namespace is refused rather
    // than silently doing nothing.
    NF_CHECK(!parse_player_settings("audio.dynamics.compressor: maybe\n", s, err));
    NF_CHECK(err.find("line 1") != std::string::npos);
    NF_CHECK(!parse_player_settings("audio.dynamics.threshold_db: -200\n", s, err));
    NF_CHECK(!parse_player_settings("audio.dynamics.ratio: 0.5\n", s, err));
    NF_CHECK(!parse_player_settings("audio.dynamics.lookahead_ms: 500\n", s, err));
    NF_CHECK(!parse_player_settings("audio.dynamics.ceiling_db: 3\n", s, err));
    NF_CHECK(!parse_player_settings("audio.dynamics.threshold: -20\n", s, err));
    NF_CHECK(err.find("threshold") != std::string::npos);
}

NF_TEST(player_settings_round_trip_through_the_writer) {
    PlayerSettings s;
    s.look_scale = 1.25f;
    s.stick_scale = 0.75f;
    s.invert_y = InvertY::Off;
    s.fov_y = 90.0f;
    s.ads_scale = 0.5f;
    s.shake_scale = 0.25f;

    const std::string text = serialize_player_settings(s);
    PlayerSettings back;
    std::string err;
    NF_CHECK(parse_player_settings(text, back, err));
    NF_CHECK_NEAR(back.look_scale, s.look_scale, 1e-4f);
    NF_CHECK_NEAR(back.stick_scale, s.stick_scale, 1e-4f);
    NF_CHECK(back.invert_y == InvertY::Off);
    NF_CHECK_NEAR(back.fov_y, s.fov_y, 1e-4f);
    NF_CHECK_NEAR(back.ads_scale, s.ads_scale, 1e-4f);
    NF_CHECK_NEAR(back.shake_scale, s.shake_scale, 1e-4f);

    // Dynamics round-trip too, and the writer's stable key order means a
    // saved file diffs cleanly when only one slider moved.
    s.dynamics.compressor.enabled = true;
    s.dynamics.compressor.threshold_db = -21.5f;
    s.dynamics.compressor.ratio = 3.5f;
    s.dynamics.compressor.attack_ms = 12.0f;
    s.dynamics.compressor.release_ms = 180.0f;
    s.dynamics.compressor.knee_db = 7.0f;
    s.dynamics.compressor.makeup_db = 2.5f;
    s.dynamics.limiter.enabled = true;
    s.dynamics.limiter.ceiling_db = -0.5f;
    s.dynamics.limiter.release_ms = 90.0f;
    s.dynamics.limiter.lookahead_ms = 6.0f;
    const std::string dyn_text = serialize_player_settings(s);
    NF_CHECK(parse_player_settings(dyn_text, back, err));
    NF_CHECK(back.dynamics.compressor.enabled);
    NF_CHECK(back.dynamics.limiter.enabled);
    NF_CHECK_NEAR(back.dynamics.compressor.threshold_db, -21.5f, 1e-5f);
    NF_CHECK_NEAR(back.dynamics.compressor.ratio, 3.5f, 1e-5f);
    NF_CHECK_NEAR(back.dynamics.compressor.attack_ms, 12.0f, 1e-5f);
    NF_CHECK_NEAR(back.dynamics.compressor.release_ms, 180.0f, 1e-5f);
    NF_CHECK_NEAR(back.dynamics.compressor.knee_db, 7.0f, 1e-5f);
    NF_CHECK_NEAR(back.dynamics.compressor.makeup_db, 2.5f, 1e-5f);
    NF_CHECK_NEAR(back.dynamics.limiter.ceiling_db, -0.5f, 1e-5f);
    NF_CHECK_NEAR(back.dynamics.limiter.release_ms, 90.0f, 1e-5f);
    NF_CHECK_NEAR(back.dynamics.limiter.lookahead_ms, 6.0f, 1e-5f);
}

NF_TEST(the_runtime_loads_and_saves_player_settings_through_the_vfs) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_player_settings";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

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
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        // A missing file is a failure the caller can report, not a crash.
        std::string err;
        NF_CHECK(!rt.load_player_settings(Runtime::kDefaultPlayerSettingsPath, err));
        NF_CHECK(!err.empty());

        PlayerSettings s;
        s.look_scale = 1.5f;
        s.invert_y = InvertY::On;
        s.shake_scale = 0.0f;
        rt.set_player_settings(s);
        NF_CHECK(rt.save_player_settings(Runtime::kDefaultPlayerSettingsPath, err));

        // A fresh runtime reads back what the first one wrote.
        Runtime rt2(vfs, registry, manager, *device, nullptr);
        NF_CHECK(rt2.load_player_settings(Runtime::kDefaultPlayerSettingsPath, err));
        NF_CHECK_NEAR(rt2.player_settings().look_scale, 1.5f, kEps);
        NF_CHECK(rt2.player_settings().invert_y == InvertY::On);
        NF_CHECK_NEAR(rt2.player_settings().shake_scale, 0.0f, kEps);
    }
    if (device) {
        device->wait_idle();
        device->shutdown();
    }
    std::filesystem::remove_all(tmp);
}

// --- mixer volumes -----------------------------------------------------------

NF_TEST(player_settings_carry_the_mixer_volumes) {
    // The defaults are AudioVolumeSettings' own, not a second copy of them:
    // the shipped mix is master 1.0, music 0.8, sfx 1.0, ambience 0.7,
    // voice 1.0, and a settings file that says nothing keeps it.
    const PlayerSettings s;
    NF_CHECK_NEAR(s.volumes.volume(audio::BusId::Master), 1.0f, kEps);
    NF_CHECK_NEAR(s.volumes.volume(audio::BusId::Music), 0.8f, kEps);
    NF_CHECK_NEAR(s.volumes.volume(audio::BusId::Sfx), 1.0f, kEps);
    NF_CHECK_NEAR(s.volumes.volume(audio::BusId::Ambience), 0.7f, kEps);
    NF_CHECK_NEAR(s.volumes.volume(audio::BusId::Voice), 1.0f, kEps);

    PlayerSettings parsed;
    std::string err;
    NF_CHECK(parse_player_settings("volume.master: 0.5\nvolume.music: 0.25\n", parsed, err));
    NF_CHECK_NEAR(parsed.volumes.volume(audio::BusId::Master), 0.5f, kEps);
    NF_CHECK_NEAR(parsed.volumes.volume(audio::BusId::Music), 0.25f, kEps);
    // The buses the file did not mention keep the shipped default.
    NF_CHECK_NEAR(parsed.volumes.volume(audio::BusId::Sfx), 1.0f, kEps);

    // A misspelled bus is refused by name rather than silently ignored: a
    // slider that does nothing is the hardest kind of bug to report.
    NF_CHECK(!parse_player_settings("volume.musik: 0.5\n", parsed, err));
    NF_CHECK(err.find("musik") != std::string::npos);
    // Out of range is refused too — amplification belongs to the source.
    NF_CHECK(!parse_player_settings("volume.master: 1.5\n", parsed, err));
    NF_CHECK(!parse_player_settings("volume.master: -0.1\n", parsed, err));

    // And the writer emits one line per bus, so a saved file restores all five.
    PlayerSettings round;
    round.volumes.set_volume(audio::BusId::Voice, 0.3f);
    round.volumes.set_volume(audio::BusId::Ambience, 0.0f);
    PlayerSettings back;
    NF_CHECK(parse_player_settings(serialize_player_settings(round), back, err));
    NF_CHECK_NEAR(back.volumes.volume(audio::BusId::Voice), 0.3f, kEps);
    NF_CHECK_NEAR(back.volumes.volume(audio::BusId::Ambience), 0.0f, kEps);
    NF_CHECK_NEAR(back.volumes.volume(audio::BusId::Music), 0.8f, kEps);
}

NF_TEST(the_runtime_pushes_volumes_into_the_live_mixer) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_player_volumes";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

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
        AssetRegistry registry;
        AssetManager manager(vfs, registry);
        Runtime rt(vfs, registry, manager, *device, nullptr);

        PlayerSettings s;
        s.volumes.set_volume(audio::BusId::Music, 0.4f);
        s.volumes.set_volume(audio::BusId::Master, 0.9f);
        s.dynamics.compressor.enabled = true;
        s.dynamics.compressor.threshold_db = -22.0f;
        s.dynamics.limiter.enabled = true;
        s.dynamics.limiter.ceiling_db = -1.0f;
        rt.set_player_settings(s);
        // "The slider moved" and "the mixer uses it" are the same statement:
        // the scene reads the same object the next block mixes from.
        NF_CHECK_NEAR(rt.audio_scene().settings().volume(audio::BusId::Music), 0.4f, kEps);
        NF_CHECK_NEAR(rt.audio_scene().settings().volume(audio::BusId::Master), 0.9f, kEps);
        // A bus the player did not touch stays at the shipped default.
        NF_CHECK_NEAR(rt.audio_scene().settings().volume(audio::BusId::Sfx), 1.0f, kEps);

        // The same binding reaches the master dynamics: enabling a stage in
        // the player's file is enough for the live mix to run it.
        const auto& live_dyn = rt.audio_scene().dynamics_settings();
        NF_CHECK(live_dyn.compressor.enabled);
        NF_CHECK(live_dyn.limiter.enabled);
        NF_CHECK_NEAR(live_dyn.compressor.threshold_db, -22.0f, kEps);
        NF_CHECK_NEAR(live_dyn.limiter.ceiling_db, -1.0f, kEps);
        NF_CHECK(rt.audio_scene().dynamics().settings().compressor.enabled);
    }
    if (device) {
        device->wait_idle();
        device->shutdown();
    }
    std::filesystem::remove_all(tmp);
}
