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
        AssetManager manager(vfs, reg, device.get());
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
        AssetManager manager(vfs, reg, device.get());
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
        AssetManager manager(vfs, reg, device.get());
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
        AssetManager manager(vfs, reg, device.get());
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
        AssetManager manager(vfs, reg, device.get());
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
