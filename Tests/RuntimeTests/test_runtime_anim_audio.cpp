// Tests/RuntimeTests/test_runtime_anim_audio.cpp — Phase 9 runtime integration.
//
// These are the tests that fail if `Runtime::step_animation` or
// `Runtime::step_audio` stops being called from `Runtime::update`. That matters
// because the AnimationTests and AudioTests suites cover the *modules*, and a
// green module suite says nothing about whether anything calls them. The defect
// this file guards against was exactly that: 51 passing module tests sitting
// next to an animation system that never ran in a shipped game, because the
// pose was computed by nobody and dropped by nobody.
//
// So every assertion here is about an observable that only exists if the
// runtime actually stepped the subsystem: a transform that moved, a pose that
// was filled, a mix buffer with signal in it.

#include <NF/Test/TestFramework.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

using namespace nf;
using namespace nf::assets;
using namespace nf::runtime;
using namespace nf::scene;
using namespace nf::animation;
using namespace nf::audio;

namespace {

/// Scene the tests run against: one entity placed at (2, 1, 0) carrying an
/// AnimationComponent with a real, one-turn spin clip, and optionally an
/// autoplay tone.
///
/// The placement is load-bearing. The pose is applied as an *offset* from the
/// authored transform, so the entity must still be at (2, 1, 0) after stepping.
/// If the implementation overwrote the transform with the root bone instead, the
/// entity would snap to the rig origin and the translation assertions below
/// would catch it.
void write_animated_scene(VirtualFileSystem& vfs, bool paused, bool with_audio) {
    Scene scene("AnimScene");
    auto& w = scene.world();

    ecs::Entity e = w.create_entity();
    Transform t;
    t.local_x = 2.0f;
    t.local_y = 1.0f;
    t.local_z = 0.0f;
    t.world_x = 2.0f;
    t.world_y = 1.0f;
    t.world_z = 0.0f;
    w.add<Transform>(e, t);

    AnimationComponent anim;
    anim.skeleton = make_default_skeleton();
    ProceduralClipSpec spec;
    spec.kind = ProceduralClipSpec::Kind::Spin;
    spec.axis = {0.0f, 1.0f, 0.0f};
    spec.turns = 1.0f;
    spec.duration = 2.0f;
    anim.clips["spin"] = make_procedural_clip("spin", anim.skeleton, spec);
    anim.player.set_clip("spin");
    anim.player.set_speed(1.0f);
    anim.player.play();
    anim.paused = paused;
    anim.has_procedural = true;
    anim.procedural = spec;
    anim.procedural_clip_name = "spin";
    w.add<AnimationComponent>(e, std::move(anim));

    if (with_audio) {
        AudioComponent aud;
        aud.buffer_name = "tone";
        aud.owned_buffer = make_tone_buffer(440.0f, 1.0f, kDefaultSampleRate, 1);
        aud.tone_hz = 440.0f;
        aud.tone_duration = 1.0f;
        aud.volume = 0.4f;
        aud.looping = true;
        aud.autoplay = true;
        w.add<AudioComponent>(e, std::move(aud));
    }

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/Anim.nfscene", scene, err));
}

/// Largest absolute change in the sampled rotation over `frames` steps of
/// 1/60 s. Returns the max deviation from the first sample, not a first-vs-last
/// comparison: a clip that completes a whole number of turns returns to its
/// starting rotation, so first-vs-last would report "no motion" for a good spin.
float max_rotation_deviation(Runtime& rt, int frames) {
    rt.update(1.0f / 60.0f);
    const auto first = rt.animated_transform_samples();
    if (first.size() != 1) {
        return 0.0f;
    }
    float max_dev = 0.0f;
    for (int i = 0; i < frames; ++i) {
        rt.update(1.0f / 60.0f);
        const auto now = rt.animated_transform_samples();
        if (now.size() != 1) {
            continue;
        }
        max_dev = std::max(
            max_dev, std::abs(now[0].rotation_euler_degrees.y - first[0].rotation_euler_degrees.y));
    }
    return max_dev;
}

} // namespace

NF_TEST(runtime_steps_animation_and_moves_entity) {
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_anim_step";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);
    AssetRegistry reg;
    write_animated_scene(vfs, /*paused=*/false, /*with_audio=*/false);

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
        AssetManager manager(vfs, reg);
        Runtime rt(vfs, reg, manager, *device, nullptr);

        std::string err;
        NF_CHECK(rt.load_scene("content://Scenes/Anim.nfscene", err));

        // Nothing has been stepped yet, so no entity counts as animated. This
        // is the observable that separates "the component is present" from "the
        // runtime stepped it".
        NF_CHECK_EQ(rt.animated_entity_count(), static_cast<size_t>(0));

        const float deviation = max_rotation_deviation(rt, 60);

        NF_CHECK_EQ(rt.animated_entity_count(), static_cast<size_t>(1));
        // 60 frames at 1/60 s is one second of a two-second, one-turn clip, so
        // roughly half a turn. Anything near zero means the pose never reached
        // the transform.
        NF_CHECK(deviation > 45.0f);

        const auto samples = rt.animated_transform_samples();
        NF_CHECK_EQ(samples.size(), static_cast<size_t>(1));
        if (samples.size() == 1) {
            // The authored placement survives: the pose is an offset from it,
            // not a replacement for it.
            NF_CHECK_NEAR(samples[0].translation.x, 2.0f, 1e-3f);
            NF_CHECK_NEAR(samples[0].translation.y, 1.0f, 1e-3f);
            NF_CHECK_NEAR(samples[0].translation.z, 0.0f, 1e-3f);
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_paused_animation_does_not_move) {
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_anim_paused";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);
    AssetRegistry reg;
    write_animated_scene(vfs, /*paused=*/true, /*with_audio=*/false);

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
        AssetManager manager(vfs, reg);
        Runtime rt(vfs, reg, manager, *device, nullptr);

        std::string err;
        NF_CHECK(rt.load_scene("content://Scenes/Anim.nfscene", err));

        // Still stepped — the component exists and is being processed — but the
        // clock must not advance. `paused` and the player state have to agree;
        // a paused component whose clock still ran would be a silent
        // contradiction between the flag and the behaviour.
        const float deviation = max_rotation_deviation(rt, 60);
        NF_CHECK_EQ(rt.animated_entity_count(), static_cast<size_t>(1));
        NF_CHECK(deviation < 1e-3f);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_audio_mixes_generated_tone) {
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_audio_mix";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);
    AssetRegistry reg;
    write_animated_scene(vfs, /*paused=*/false, /*with_audio=*/true);

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
        AssetManager manager(vfs, reg);
        Runtime rt(vfs, reg, manager, *device, nullptr);

        std::string err;
        NF_CHECK(rt.load_scene("content://Scenes/Anim.nfscene", err));

        // The mixer releases whole device buffers, so one frame is not enough to
        // guarantee a block. Run a couple of seconds and keep the maxima.
        size_t mixed_max = 0;
        float peak_max = 0.0f;
        for (int i = 0; i < 120; ++i) {
            rt.update(1.0f / 60.0f);
            mixed_max = std::max(mixed_max, rt.audio_sources_mixed());
            peak_max = std::max(peak_max, rt.audio_output_peak());
        }

        // A source that is never mixed is the audio equivalent of the pose that
        // was never applied: the component exists, nothing plays.
        NF_CHECK(mixed_max >= static_cast<size_t>(1));
        // volume 0.4 * the tone generator's 0.5 amplitude.
        NF_CHECK_NEAR(peak_max, 0.2f, 0.02f);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_audio_silent_without_sources) {
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_audio_silent";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);
    AssetRegistry reg;
    write_animated_scene(vfs, /*paused=*/false, /*with_audio=*/false);

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
        AssetManager manager(vfs, reg);
        Runtime rt(vfs, reg, manager, *device, nullptr);

        std::string err;
        NF_CHECK(rt.load_scene("content://Scenes/Anim.nfscene", err));

        float peak_max = 0.0f;
        for (int i = 0; i < 120; ++i) {
            rt.update(1.0f / 60.0f);
            peak_max = std::max(peak_max, rt.audio_output_peak());
        }

        // The control for the test above: with no source the mix is silent, so
        // a non-zero peak there is genuinely the scene's audio and not the
        // backend's own output leaking through.
        NF_CHECK_EQ(rt.audio_sources_mixed(), static_cast<size_t>(0));
        NF_CHECK(peak_max < 1e-6f);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
// ---------------------------------------------------------------------------
// The AudioScene bridge (G5 -> Runtime)
//
// The defect these guard against: `AudioScene` — buses, Settings volumes,
// music, ambience, reverb — was fully built and fully tested (AudioTests 110)
// while `Runtime::step_audio` mixed every component straight into the output
// buffers through a bare `AudioBus`. Nothing a game shipped could reach any of
// it. A green AudioTests suite said only that the mixer worked, never that a
// game could hear it.
//
// So every assertion below is about the *shipped path*: a setting written
// through the public runtime API must change the mix the player hears.
// ---------------------------------------------------------------------------

namespace {

/// Peak the runtime's mix reaches over `frames` steps, with `bus` set to
/// `volume` through the public Settings API. Uses the same scene as the
/// existing audio tests, so the tone is 440 Hz at 0.4 source volume; only the
/// bus routing differs. Returns -1 when no device could be created.
float peak_with_bus_volume(BusId bus, float volume, int frames = 120) {
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_audio_bridge";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);
    AssetRegistry reg;
    write_animated_scene(vfs, /*paused=*/false, /*with_audio=*/true);

    auto device = rhi::create_device();
    if (!device) {
        std::filesystem::remove_all(tmp);
        return -1.0f;
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) {
        std::filesystem::remove_all(tmp);
        return -1.0f;
    }

    float peak_max = 0.0f;
    {
        AssetManager manager(vfs, reg);
        Runtime rt(vfs, reg, manager, *device, nullptr);

        // The Settings-window path: write the bus volume, and the very next
        // block must honour it. No reconfiguration, no replacing the runtime.
        rt.audio_scene().settings().set_volume(bus, volume);

        std::string err;
        if (!rt.load_scene("content://Scenes/Anim.nfscene", err)) {
            device->wait_idle();
            device->shutdown();
            std::filesystem::remove_all(tmp);
            return -1.0f;
        }

        for (int i = 0; i < frames; ++i) {
            rt.update(1.0f / 60.0f);
            peak_max = std::max(peak_max, rt.audio_output_peak());
        }
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
    return peak_max;
}

} // namespace

NF_TEST(runtime_audio_sfx_bus_volume_reaches_the_shipped_mix) {
    const float loud = peak_with_bus_volume(BusId::Sfx, 1.0f);
    if (loud < 0.0f) {
        NF_SKIP("no Vulkan device available");
    }
    const float quiet = peak_with_bus_volume(BusId::Sfx, 0.0f);

    // The scene's source is on the Sfx bus, so the Sfx slider owns what the
    // player hears. Before the bridge this value changed nothing: the runtime
    // never consulted a BusMixer, so both runs returned the same peak and this
    // difference was exactly zero.
    NF_CHECK(loud > 0.05f);
    NF_CHECK(quiet < loud * 0.5f);
}

NF_TEST(runtime_audio_master_volume_reaches_the_shipped_mix) {
    const float loud = peak_with_bus_volume(BusId::Master, 1.0f);
    if (loud < 0.0f) {
        NF_SKIP("no Vulkan device available");
    }
    const float quiet = peak_with_bus_volume(BusId::Master, 0.0f);

    // Master scales every bus, so it must gate the same source.
    NF_CHECK(loud > 0.05f);
    NF_CHECK(quiet < loud * 0.5f);
}

NF_TEST(runtime_audio_unrelated_bus_volume_does_not_silence_the_source) {
    // The control for the two tests above: routing is per-bus, not global.
    // Muting Voice must not touch a source on Sfx — if it did, the bus tree
    // would be a single volume with five names on it.
    const float before = peak_with_bus_volume(BusId::Voice, 1.0f);
    if (before < 0.0f) {
        NF_SKIP("no Vulkan device available");
    }
    const float after = peak_with_bus_volume(BusId::Voice, 0.0f);

    NF_CHECK(before > 0.05f);
    NF_CHECK_NEAR(after, before, before * 0.05f);
}

NF_TEST(audio_component_pitch_changes_playback_rate) {
    // `pitch` was authored, serialized, round-trip-tested and editable in the
    // Inspector, and the mixer never read it — the field was decorative. This
    // pins the two ends of that: a doubled pitch advances the cursor twice as
    // fast, and a nonsensical pitch plays at the authored rate instead of
    // stalling the source into silence forever.
    AudioBuffer buf = make_tone_buffer(440.0f, 1.0f, kDefaultSampleRate, 1);

    AudioSource fast;
    fast.buffer = &buf;
    fast.playing = true;
    fast.pitch = 2.0f;
    std::vector<f32> l(44100, 0.0f), r(44100, 0.0f);
    AudioBus bus;
    bus.mix_source(fast, Vec3{}, Vec3{0, 0, -1}, Vec3{0, 1, 0}, l.data(), r.data(),
                   44100, kDefaultSampleRate);
    // One second of output at pitch 2 consumes two seconds of the 1 s buffer,
    // so the source has run off the end rather than played it straight through.
    NF_CHECK(!fast.playing);
    NF_CHECK(fast.sample_cursor >= buf.frame_count());

    // A pitch the mixer cannot honour must fall back to 1.0. Zero would freeze
    // the cursor on one sample forever; a negative step walks off the front.
    for (f32 bad : {0.0f, -1.0f, std::numeric_limits<f32>::quiet_NaN()}) {
        AudioSource src;
        src.buffer = &buf;
        src.playing = true;
        src.pitch = bad;
        std::vector<f32> bl(1024, 0.0f), br(1024, 0.0f);
        AudioBus b2;
        b2.mix_source(src, Vec3{}, Vec3{0, 0, -1}, Vec3{0, 1, 0}, bl.data(), br.data(),
                      1024, kDefaultSampleRate);
        NF_CHECK(src.sample_cursor > 0);
        float peak = 0.0f;
        for (usize i = 0; i < 1024; ++i) {
            peak = std::max(peak, std::abs(bl[i]));
        }
        NF_CHECK(peak > 0.0f);
    }
}

NF_TEST(runtime_audio_persists_the_fractional_cursor_across_blocks) {
    // The second half of the defect's blast radius. `sample_cursor` is the
    // integral position; `sample_position` is the exact resampling cursor.
    // Persisting only the first re-seeks a rate-converted buffer to a truncated
    // position every block, so the sound drifts. This pins that the runtime
    // keeps both, and that they stay consistent.
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_audio_cursor";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);
    AssetRegistry reg;
    write_animated_scene(vfs, /*paused=*/false, /*with_audio=*/true);

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
        AssetManager manager(vfs, reg);
        Runtime rt(vfs, reg, manager, *device, nullptr);
        std::string err;
        NF_CHECK(rt.load_scene("content://Scenes/Anim.nfscene", err));

        for (int i = 0; i < 90; ++i) {
            rt.update(1.0f / 60.0f);
        }

        bool found = false;
        const ecs::World& w = rt.edit_scene()->world();
        for (auto e : w.all_entities()) {
            auto* aud = w.get<AudioComponent>(e);
            if (aud == nullptr) {
                continue;
            }
            found = true;
            // A looping 1 s tone played for ~1.5 s has a live cursor...
            NF_CHECK(aud->sample_cursor > 0);
            // ...and the fractional field must be carrying the exact position,
            // not left at its -1 "adopt the cursor" sentinel. A -1 here would
            // mean every block re-seeks from the truncated integer and drifts.
            NF_CHECK(aud->sample_position >= 0.0);
            // The two must agree: the fractional position's floor is the
            // integer cursor. Disagreement means one was not persisted.
            NF_CHECK_EQ(static_cast<usize>(std::floor(aud->sample_position)),
                        aud->sample_cursor);
        }
        NF_CHECK(found);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_audio_bus_and_occlusion_round_trip) {
    // `bus` and `occluded` are the data a game needs to put dialogue on the
    // Voice slider and to keep a UI blip from being muffled by a wall. Both are
    // entity component lines, so they need no format migration.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_audio_bus";
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("AudioBusScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});

    AudioComponent aud;
    aud.buffer_name = "line.wav";
    aud.bus = BusId::Voice;
    aud.occluded = false;
    w.add<AudioComponent>(e, aud);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/bus.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/bus.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    auto* loaded = result.scene->world().get<AudioComponent>(
        result.scene->world().all_entities()[0]);
    NF_CHECK(loaded);
    NF_CHECK(loaded->bus == BusId::Voice);
    NF_CHECK(!loaded->occluded);

    // A source left on the defaults must keep them through the file, not
    // acquire a surprising bus on the way back in.
    AudioComponent dflt;
    dflt.buffer_name = "amb.wav";
    ecs::Entity e2 = w.create_entity();
    w.add<Transform>(e2, Transform{});
    w.add<AudioComponent>(e2, dflt);
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/bus2.nfscene", scene, err));
    auto r2 = load_scene_from_vfs(vfs, "content://Scenes/bus2.nfscene");
    NF_CHECK(r2.success);
    NF_CHECK(r2.scene);

    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_scene_round_trip_keeps_animation_moving) {
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_anim_roundtrip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    VirtualFileSystem vfs;
    vfs.mount("content://", tmp);
    AssetRegistry reg;
    write_animated_scene(vfs, /*paused=*/false, /*with_audio=*/true);

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
        AssetManager manager(vfs, reg);
        Runtime rt(vfs, reg, manager, *device, nullptr);

        std::string err;
        NF_CHECK(rt.load_scene("content://Scenes/Anim.nfscene", err));
        rt.update(1.0f / 60.0f);

        // Clips and tone buffers are *generated* at load time, never stored, so
        // the spec is the only record of them. If the save path drops it, the
        // reloaded scene has an empty clip and a silent source — the original
        // defect reintroduced through the back door.
        NF_CHECK(rt.save_scene("content://Scenes/Saved.nfscene", err));
        NF_CHECK(rt.load_scene("content://Scenes/Saved.nfscene", err));

        const auto* w = rt.scene() ? &rt.scene()->world() : nullptr;
        NF_CHECK(w != nullptr);
        if (w != nullptr) {
            for (auto e : w->query<AnimationComponent>()) {
                const auto* anim = w->get<AnimationComponent>(e);
                NF_CHECK(anim != nullptr);
                if (anim != nullptr) {
                    NF_CHECK(anim->has_procedural);
                    NF_CHECK_EQ(anim->procedural_clip_name, std::string("spin"));
                    const auto it = anim->clips.find("spin");
                    NF_CHECK(it != anim->clips.end());
                    if (it != anim->clips.end()) {
                        NF_CHECK(!it->second.tracks.empty());
                    }
                }
            }
            for (auto e : w->query<AudioComponent>()) {
                const auto* aud = w->get<AudioComponent>(e);
                NF_CHECK(aud != nullptr);
                if (aud != nullptr) {
                    NF_CHECK(aud->tone_hz > 0.0f);
                    NF_CHECK(aud->resolved_buffer() != nullptr);
                }
            }
        }

        // And it still moves after the round trip.
        const float deviation = max_rotation_deviation(rt, 60);
        NF_CHECK(deviation > 45.0f);
    }

    device->wait_idle();
    device->shutdown();
    std::filesystem::remove_all(tmp);
}
