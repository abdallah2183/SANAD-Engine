// RuntimeTests — the scene's audio ENVIRONMENT: reverb zones, the level's
// music, and its ambience bed.
//
// The audio module ships all of it (AudioScene's zones, MusicSystem's track and
// bed) but before this the only way to reach any of it was C++ — the scene
// loader parsed `Audio:` for a per-entity source and nothing else. A shipped
// game could not have a cave that echoes or a level theme without engine
// source edits, which is exactly what the Game-Ready criterion measures.
//
// Two halves, and the second is the one that matters:
//
//   1. The three lines parse, save and round-trip, and a scene that has never
//      heard of them writes no line at all.
//   2. The Runtime actually BUILDS them into the live AudioScene — and, because
//      the scene owns its environment, REPLACES the previous scene's. A
//      component the runtime reads and drops is the "registered but never
//      driven" failure this engine has shipped before.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Audio/AudioScene.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Audio/MusicSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::assets;
using namespace nf::scene;
using namespace nf::runtime;

namespace {

void push_u16(std::vector<u8>& b, u16 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
}
void push_u32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

/// A real mono 16-bit WAV, so `buffer=` goes through the actual decode path
/// rather than a stub that would pass even if decoding were broken.
std::vector<u8> make_wav_mono16(u32 rate, float freq, float seconds) {
    const u32 frames = static_cast<u32>(rate * seconds);
    std::vector<u8> b;
    for (char c : {'R', 'I', 'F', 'F'}) b.push_back(static_cast<u8>(c));
    push_u32(b, 36 + frames * 2);
    for (char c : {'W', 'A', 'V', 'E'}) b.push_back(static_cast<u8>(c));
    for (char c : {'f', 'm', 't', ' '}) b.push_back(static_cast<u8>(c));
    push_u32(b, 16);
    push_u16(b, 1);
    push_u16(b, 1);
    push_u32(b, rate);
    push_u32(b, rate * 2);
    push_u16(b, 2);
    push_u16(b, 16);
    for (char c : {'d', 'a', 't', 'a'}) b.push_back(static_cast<u8>(c));
    push_u32(b, frames * 2);
    constexpr float kPi = 3.14159265358979323846f;
    for (u32 i = 0; i < frames; ++i) {
        const float s = 0.5f * std::sin(2.0f * kPi * freq * i / rate);
        push_u16(b, static_cast<u16>(static_cast<i16>(std::lround(s * 32767.0f))));
    }
    return b;
}

void write_file(const std::filesystem::path& path, const void* data, usize size) {
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    fopen_s(&f, path.string().c_str(), "wb");
#else
    f = std::fopen(path.string().c_str(), "wb");
#endif
    if (f) {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
    }
}

/// Scene text with all three environment components on two entities. The zone
/// sits at (3, 0, 4) — the runtime test checks the zone's POSITION comes from
/// the transform, not from the line.
const char* kAudioEnvScene =
    "# NOVAForge Scene v1\nversion: 1\nname: AudioEnv\nentity_count: 2\n"
    "---\nentity: 1:0\n"
    "  Name: Cave\n"
    "  Transform: local(3,0,4) world(3,0,4) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
    "  ReverbZone: radius=12 inner=3 wet=0.5 decay=2 predelay=0.02 spacing=0.09\n"
    "---\nentity: 2:0\n"
    "  Name: Theme\n"
    "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
    "  Music: buffer=content://Audio/theme.wav volume=0.6 fade=0\n"
    "  Ambience: buffer=content://Audio/theme.wav fade=0\n";

/// A scene with no audio environment at all — the compatibility case.
const char* kPlainScene =
    "# NOVAForge Scene v1\nversion: 1\nname: Plain\nentity_count: 1\n"
    "---\nentity: 1:0\n  Name: Rock\n"
    "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n";

} // namespace

NF_TEST(scene_audio_environment_round_trips) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_audio_env_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("AudioEnv");
    auto& w = scene.world();

    ecs::Entity cave = w.create_entity();
    w.add<Transform>(cave, Transform{});
    w.add<NameComponent>(cave, NameComponent{"Cave"});
    audio::ReverbZoneComponent rz;
    rz.radius = 12.0f;
    rz.inner_radius = 3.0f;
    rz.wet_gain = 0.5f;
    rz.decay_seconds = 2.0f;
    rz.pre_delay_seconds = 0.02f;
    rz.echo_spacing_seconds = 0.09f;
    w.add<audio::ReverbZoneComponent>(cave, rz);

    ecs::Entity theme = w.create_entity();
    w.add<Transform>(theme, Transform{});
    w.add<NameComponent>(theme, NameComponent{"Theme"});
    audio::MusicComponent mus;
    mus.buffer_name = "content://Audio/theme.wav";
    mus.volume = 0.6f;
    mus.fade_in_seconds = 0.25f;
    w.add<audio::MusicComponent>(theme, mus);
    audio::AmbienceComponent amb;
    amb.buffer_name = "content://Audio/wind.wav";
    amb.fade_in_seconds = 1.5f;
    w.add<audio::AmbienceComponent>(theme, amb);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/env.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/env.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }
    const auto& lw = result.scene->world();
    const auto* lrz = lw.get<audio::ReverbZoneComponent>(lw.all_entities()[0]);
    NF_CHECK(lrz != nullptr);
    if (lrz != nullptr) {
        NF_CHECK_NEAR(lrz->radius, 12.0f, 1e-5f);
        NF_CHECK_NEAR(lrz->inner_radius, 3.0f, 1e-5f);
        NF_CHECK_NEAR(lrz->wet_gain, 0.5f, 1e-5f);
        NF_CHECK_NEAR(lrz->decay_seconds, 2.0f, 1e-5f);
        NF_CHECK_NEAR(lrz->pre_delay_seconds, 0.02f, 1e-5f);
        NF_CHECK_NEAR(lrz->echo_spacing_seconds, 0.09f, 1e-5f);
        NF_CHECK(lrz->enabled);
    }
    const auto* lmus = lw.get<audio::MusicComponent>(lw.all_entities()[1]);
    NF_CHECK(lmus != nullptr);
    if (lmus != nullptr) {
        NF_CHECK(lmus->buffer_name == "content://Audio/theme.wav");
        NF_CHECK_NEAR(lmus->volume, 0.6f, 1e-5f);
        NF_CHECK_NEAR(lmus->fade_in_seconds, 0.25f, 1e-5f);
        NF_CHECK(lmus->enabled);
    }
    const auto* lamb = lw.get<audio::AmbienceComponent>(lw.all_entities()[1]);
    NF_CHECK(lamb != nullptr);
    if (lamb != nullptr) {
        NF_CHECK(lamb->buffer_name == "content://Audio/wind.wav");
        NF_CHECK_NEAR(lamb->fade_in_seconds, 1.5f, 1e-5f);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_without_audio_environment_writes_no_lines) {
    // A scene that never heard of these components must round-trip byte for
    // byte — the rule the Sky, TimeOfDay and PostProcess lines follow, and the
    // reason none of them may be written unconditionally.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_audio_env_plain";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    auto result = load_scene_from_text(kPlainScene);
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    if (!result.scene) {
        std::filesystem::remove_all(tmp);
        return;
    }
    const std::string text = serialize_scene_to_text(*result.scene);
    NF_CHECK(text.find("ReverbZone:") == std::string::npos);
    NF_CHECK(text.find("Music:") == std::string::npos);
    NF_CHECK(text.find("Ambience:") == std::string::npos);
    // And the plain scene still loads back to the same bytes it came from.
    auto again = load_scene_from_text(text);
    NF_CHECK(again.success);
    if (again.scene) {
        NF_CHECK(serialize_scene_to_text(*again.scene) == text);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(reverb_zone_with_a_non_positive_radius_is_refused) {
    // A zone that reaches nothing is a mistake, not a setting. It must not
    // enter the world, where it would be indistinguishable from an author's
    // deliberate "no reverb here".
    const std::string text =
        std::string(kPlainScene) +
        "  ReverbZone: radius=0 inner=1 wet=0.5 decay=1 predelay=0.02 spacing=0.1\n";
    auto result = load_scene_from_text(text);
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    if (result.scene) {
        const auto& lw = result.scene->world();
        NF_CHECK(lw.get<audio::ReverbZoneComponent>(lw.all_entities()[0]) == nullptr);
    }
    bool warned = false;
    for (const std::string& wmsg : result.warnings) {
        if (wmsg.find("ReverbZone") != std::string::npos) warned = true;
    }
    NF_CHECK(warned);

    // An inner radius past the outer one is clamped, not refused: it is a
    // tunable that got dragged, and the falloff must not invert.
    const std::string clamped =
        std::string(kPlainScene) + "  ReverbZone: radius=5 inner=50\n";
    auto ok = load_scene_from_text(clamped);
    NF_CHECK(ok.success);
    if (ok.scene) {
        const auto& lw = ok.scene->world();
        const auto* rz = lw.get<audio::ReverbZoneComponent>(lw.all_entities()[0]);
        NF_CHECK(rz != nullptr);
        if (rz != nullptr) {
            NF_CHECK_NEAR(rz->inner_radius, 5.0f, 1e-5f);
        }
    }
}

NF_TEST(runtime_builds_the_audio_environment_from_the_scene) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_audio_env";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "Audio");
    std::filesystem::create_directories(tmp / "Scenes");
    vfs.mount("content://", tmp);

    const auto wav = make_wav_mono16(22050, 440.0f, 0.1f);
    write_file(tmp / "Audio" / "theme.wav", wav.data(), wav.size());
    NF_CHECK(vfs.write_text("content://Scenes/Env.nfscene", kAudioEnvScene).ok);

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
        // The Runtime owns the renderer, which owns RHI objects, so it must die
        // before the device is shut down.
        {
            Runtime rt(vfs, reg, manager, *device, nullptr);
            std::string err;
            NF_CHECK(rt.load_scene("content://Scenes/Env.nfscene", err));

            audio::AudioScene& scene_audio = rt.audio_scene();
            // The zone reached the mixer, and its POSITION came from the
            // entity's transform (3, 0, 4) rather than from the line.
            NF_CHECK(scene_audio.zone_count() == 1);
            if (scene_audio.zone_count() == 1) {
                const audio::ReverbZone& z = scene_audio.zone(0);
                NF_CHECK_NEAR(z.position.x, 3.0f, 1e-4f);
                NF_CHECK_NEAR(z.position.y, 0.0f, 1e-4f);
                NF_CHECK_NEAR(z.position.z, 4.0f, 1e-4f);
                NF_CHECK_NEAR(z.radius, 12.0f, 1e-4f);
                NF_CHECK_NEAR(z.inner_radius, 3.0f, 1e-4f);
                NF_CHECK_NEAR(z.wet_gain, 0.5f, 1e-4f);
            }
            // `fade=0` snaps the envelope open, so the state is Playing rather
            // than FadingIn — which also pins that a zero fade is honoured.
            NF_CHECK(scene_audio.music().state() == audio::MusicState::Playing);
            NF_CHECK(scene_audio.music().has_ambience());
        }
        device->wait_idle();
        device->shutdown();
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_replaces_the_previous_scene_audio_environment) {
    // The scene OWNS its environment, so loading a level without one must not
    // leave the previous level's cave echoing or its theme playing. This is the
    // opposite of the post-process rule on purpose: those are renderer settings
    // a caller may have configured directly, these are per-scene artefacts.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_audio_env_swap";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "Audio");
    std::filesystem::create_directories(tmp / "Scenes");
    vfs.mount("content://", tmp);

    const auto wav = make_wav_mono16(22050, 440.0f, 0.1f);
    write_file(tmp / "Audio" / "theme.wav", wav.data(), wav.size());
    NF_CHECK(vfs.write_text("content://Scenes/Env.nfscene", kAudioEnvScene).ok);
    NF_CHECK(vfs.write_text("content://Scenes/Plain.nfscene", kPlainScene).ok);

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
        {
            Runtime rt(vfs, reg, manager, *device, nullptr);
            std::string err;
            NF_CHECK(rt.load_scene("content://Scenes/Env.nfscene", err));
            NF_CHECK(rt.audio_scene().zone_count() == 1);
            NF_CHECK(rt.audio_scene().music().has_ambience());

            NF_CHECK(rt.load_scene("content://Scenes/Plain.nfscene", err));
            NF_CHECK(rt.audio_scene().zone_count() == 0);
            NF_CHECK(rt.audio_scene().music().state() == audio::MusicState::Stopped);
            NF_CHECK(!rt.audio_scene().music().has_ambience());
        }
        device->wait_idle();
        device->shutdown();
    }

    std::filesystem::remove_all(tmp);
}
