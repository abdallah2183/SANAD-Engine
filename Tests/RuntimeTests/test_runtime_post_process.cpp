// RuntimeTests — the post-processing stack as scene data (design §206).
//
// Two halves, and the second is the one that matters:
//
//   1. The `PostProcess:` line parses, saves and round-trips, and a scene that
//      has never heard of it writes no line at all. That is the compatibility
//      contract the writer's "absent means unchanged" rule depends on.
//   2. The Runtime actually PUSHES the block into the renderer. A component the
//      runtime reads and drops is the "registered but never driven" failure the
//      engine has shipped before: the library is real, the tests are real, and
//      the feature does nothing. `runtime_pushes_post_process_into_the_renderer`
//      is the test that would have caught it.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>

#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::assets;
using namespace nf::scene;
using namespace nf::runtime;

namespace {

const char* kSceneHeader =
    "# SANAD Scene v1\nversion: 1\nname: Post\nentity_count: 1\n"
    "---\nentity: 1:0\n  Name: Env\n"
    "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n";

} // namespace

NF_TEST(scene_post_process_round_trip) {
    // Every field, through the real writer and the real reader. A field that the
    // writer emits but the parser does not read (or vice versa) is exactly the
    // drift a round trip catches and a hand-written fixture does not.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_pp_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("PostScene");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});

    PostProcessComponent pp;
    pp.bloom_enabled = true;
    pp.bloom_threshold = 1.5f;
    pp.bloom_knee = 0.25f;
    pp.bloom_intensity = 2.5f;
    pp.bloom_radius = 1.75f;
    pp.grade_enabled = true;
    pp.grade_contrast = 1.25f;
    pp.grade_pivot = 0.75f;
    pp.grade_temperature = -0.5f;
    pp.grade_tint = 0.25f;
    pp.grade_gamma = 1.5f;
    pp.sharpen_enabled = true;
    pp.sharpen_amount = 0.75f;
    pp.sharpen_radius = 2.5f;
    pp.saturation = 1.5f;
    pp.vignette = 0.5f;
    pp.lens_enabled = true;
    pp.lens_distortion = 0.25f;
    pp.lens_chromatic_aberration = 0.015f;
    pp.dof_enabled = true;
    pp.dof_focus_distance = 22.5f;
    pp.dof_focus_range = 4.5f;
    pp.dof_max_radius = 9.0f;
    pp.motion_enabled = true;
    pp.motion_intensity = 1.75f;
    pp.motion_max_length = 0.08f;
    pp.render_scale = 1.5f;
    pp.exposure = 2.5f;
    pp.tonemap = 1; // ACES
    pp.lut_path = "content://LUTs/teal_orange.png";
    pp.lut_strength = 0.75f;
    pp.fog_enabled = true;
    pp.fog_r = 0.55f;
    pp.fog_g = 0.65f;
    pp.fog_b = 0.75f;
    pp.fog_start = 14.0f;
    pp.fog_end = 55.0f;
    w.add<PostProcessComponent>(e, pp);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/pp.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/pp.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    const auto* loaded =
        result.scene->world().get<PostProcessComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(loaded);
    if (loaded != nullptr) {
        NF_CHECK(loaded->bloom_enabled);
        NF_CHECK_NEAR(loaded->bloom_threshold, 1.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->bloom_knee, 0.25f, 1e-5f);
        NF_CHECK_NEAR(loaded->bloom_intensity, 2.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->bloom_radius, 1.75f, 1e-5f);
        NF_CHECK(loaded->grade_enabled);
        NF_CHECK_NEAR(loaded->grade_contrast, 1.25f, 1e-5f);
        NF_CHECK_NEAR(loaded->grade_pivot, 0.75f, 1e-5f);
        NF_CHECK_NEAR(loaded->grade_temperature, -0.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->grade_tint, 0.25f, 1e-5f);
        NF_CHECK_NEAR(loaded->grade_gamma, 1.5f, 1e-5f);
        NF_CHECK(loaded->sharpen_enabled);
        NF_CHECK_NEAR(loaded->sharpen_amount, 0.75f, 1e-5f);
        NF_CHECK_NEAR(loaded->sharpen_radius, 2.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->saturation, 1.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->vignette, 0.5f, 1e-5f);
        NF_CHECK(loaded->lens_enabled);
        NF_CHECK_NEAR(loaded->lens_distortion, 0.25f, 1e-5f);
        NF_CHECK_NEAR(loaded->lens_chromatic_aberration, 0.015f, 1e-6f);
        NF_CHECK(loaded->dof_enabled);
        NF_CHECK_NEAR(loaded->dof_focus_distance, 22.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->dof_focus_range, 4.5f, 1e-5f);
        NF_CHECK_NEAR(loaded->dof_max_radius, 9.0f, 1e-5f);
        NF_CHECK(loaded->motion_enabled);
        NF_CHECK_NEAR(loaded->motion_intensity, 1.75f, 1e-5f);
        NF_CHECK_NEAR(loaded->motion_max_length, 0.08f, 1e-6f);
        NF_CHECK_NEAR(loaded->exposure, 2.5f, 1e-5f);
        NF_CHECK(loaded->tonemap == 1);
        NF_CHECK(loaded->lut_path == "content://LUTs/teal_orange.png");
        NF_CHECK_NEAR(loaded->lut_strength, 0.75f, 1e-5f);
        NF_CHECK(loaded->fog_enabled);
        NF_CHECK_NEAR(loaded->fog_r, 0.55f, 1e-5f);
        NF_CHECK_NEAR(loaded->fog_g, 0.65f, 1e-5f);
        NF_CHECK_NEAR(loaded->fog_b, 0.75f, 1e-5f);
        NF_CHECK_NEAR(loaded->fog_start, 14.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->fog_end, 55.0f, 1e-5f);
        NF_CHECK_NEAR(loaded->render_scale, 1.5f, 1e-5f);
    }

    // A non-default scale is authored state and must be written; the next
    // test proves the default is omitted.
    const std::string saved = serialize_scene_to_text(*result.scene);
    NF_CHECK(saved.find("render_scale=1.5") != std::string::npos);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_post_process_line_keeps_defaults_for_absent_keys) {
    // The line is written by hand as often as by the editor, so a one-key line
    // must be a complete line: `bloom=true` alone means "bloom with the default
    // threshold, knee, intensity and radius", not "bloom with zeroes".
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_pp_min";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    NF_CHECK(vfs.write_text("content://Scenes/min.nfscene",
                            std::string(kSceneHeader) + "  PostProcess: bloom=true\n")
                 .ok);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/min.nfscene");
    NF_CHECK(result.success);
    const auto* pp =
        result.scene->world().get<PostProcessComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(pp);
    if (pp != nullptr) {
        NF_CHECK(pp->bloom_enabled);
        NF_CHECK_NEAR(pp->bloom_threshold, 1.0f, 1e-5f);
        NF_CHECK_NEAR(pp->bloom_knee, 0.5f, 1e-5f);
        NF_CHECK_NEAR(pp->bloom_intensity, 1.0f, 1e-5f);
        NF_CHECK_NEAR(pp->bloom_radius, 1.0f, 1e-5f);
        // The stages the line did not mention stay off, not on-with-defaults.
        NF_CHECK(!pp->grade_enabled);
        NF_CHECK(!pp->sharpen_enabled);
        NF_CHECK_NEAR(pp->saturation, 1.0f, 1e-5f);
        NF_CHECK_NEAR(pp->vignette, 0.0f, 1e-5f);
        // Anti-aliasing is one of them: an absent `fxaa=` leaves the pass off,
        // and the thresholds keep the FXAA 3.11 console defaults so a scene that
        // turns it on with one key gets the classic look.
        NF_CHECK(!pp->fxaa_enabled);
        NF_CHECK_NEAR(pp->fxaa_threshold, 0.125f, 1e-5f);
        NF_CHECK_NEAR(pp->fxaa_threshold_min, 0.0312f, 1e-5f);
        // Render scale is absent too, and its default is the identity. A line
        // that only turns on bloom must not change the internal resolution.
        NF_CHECK_NEAR(pp->render_scale, 1.0f, 1e-5f);
        // Dynamic resolution is opt-in: absent means the governor is off, even
        // though its tuning fields always have usable defaults.
        NF_CHECK(!pp->dynamic_resolution);
        NF_CHECK_NEAR(pp->target_fps, 60.0f, 1e-5f);
        NF_CHECK_NEAR(pp->min_render_scale, 0.5f, 1e-5f);
        NF_CHECK_EQ(pp->dynamic_resolution_timing, kDynamicResolutionTimingCpu);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_post_process_line_refuses_a_zero_radius) {
    // A bloom radius of 0 collapses all thirteen taps onto one texel — a blur
    // that blurs nothing, which reads as "bloom is broken" rather than "bloom is
    // misconfigured". The line is ignored with a warning that names the key.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_pp_bad";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    NF_CHECK(vfs.write_text("content://Scenes/bad.nfscene",
                            std::string(kSceneHeader) +
                                "  PostProcess: bloom=true bloom_radius=0\n")
                 .ok);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/bad.nfscene");
    NF_CHECK(result.success); // the SCENE is still valid; the line is not
    NF_CHECK(result.scene->world().get<PostProcessComponent>(
                 result.scene->world().all_entities()[0]) == nullptr);
    bool warned = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("bloom_radius=") != std::string::npos) warned = true;
    }
    NF_CHECK(warned);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_post_process_line_refuses_an_out_of_range_render_scale) {
    // The loader validates against the renderer's own bounds. Accepting 0.1 or
    // 2.5 here would mean the file says one resolution and the renderer quietly
    // renders another, which is exactly the drift the shared constants exist to
    // prevent.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_pp_scale_bad";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    for (const char* value : {"0.1", "2.5", "nan"}) {
        const std::string name = std::string("content://Scenes/scale_") + value + ".nfscene";
        NF_CHECK(vfs.write_text(name,
                                std::string(kSceneHeader) +
                                    "  PostProcess: fxaa=true render_scale=" + value + "\n")
                     .ok);
        auto result = load_scene_from_vfs(vfs, name);
        NF_CHECK(result.success); // the scene is valid; the line is not
        NF_CHECK(result.scene->world().get<PostProcessComponent>(
                     result.scene->world().all_entities()[0]) == nullptr);
        bool warned = false;
        for (const std::string& warning : result.warnings) {
            if (warning.find("render_scale=") != std::string::npos) warned = true;
        }
        NF_CHECK(warned);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_post_process_writes_render_scale_only_when_non_default) {
    // The compatibility rule the whole writer follows: absent means unchanged,
    // and a default value is not authored state. Emitting `render_scale=1`
    // would rewrite every existing level on its first save.
    scene::Scene scene("ScaleWrite");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    PostProcessComponent pp;
    pp.fxaa_enabled = true; // make sure the line itself is emitted
    w.add<PostProcessComponent>(e, pp);

    const std::string native = serialize_scene_to_text(scene);
    NF_CHECK(native.find("PostProcess:") != std::string::npos);
    NF_CHECK(native.find("render_scale=") == std::string::npos);

    w.get<PostProcessComponent>(e)->render_scale = 2.0f;
    const std::string supersampled = serialize_scene_to_text(scene);
    NF_CHECK(supersampled.find("render_scale=2") != std::string::npos);
}

NF_TEST(scene_dynamic_resolution_is_written_only_while_enabled) {
    // `target_fps` and `min_render_scale` are tuning for a feature that can be
    // off. Writing them unconditionally would put two keys into every level on
    // its first save; writing them only while enabled keeps "off" literally
    // indistinguishable from a scene authored before Phase 38.
    scene::Scene scene("DynamicWrite");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});
    PostProcessComponent pp;
    pp.fxaa_enabled = true;
    pp.target_fps = 90.0f;
    pp.min_render_scale = 0.6f;
    pp.dynamic_resolution_timing = kDynamicResolutionTimingGpu;
    w.add<PostProcessComponent>(e, pp);

    const std::string off = serialize_scene_to_text(scene);
    NF_CHECK(off.find("PostProcess:") != std::string::npos);
    NF_CHECK(off.find("dynamic_resolution") == std::string::npos);
    NF_CHECK(off.find("target_fps=") == std::string::npos);
    NF_CHECK(off.find("min_render_scale=") == std::string::npos);

    w.get<PostProcessComponent>(e)->dynamic_resolution = true;
    const std::string on = serialize_scene_to_text(scene);
    NF_CHECK(on.find("dynamic_resolution=true") != std::string::npos);
    NF_CHECK(on.find("target_fps=90") != std::string::npos);
    NF_CHECK(on.find("min_render_scale=0.6") != std::string::npos);
    NF_CHECK(on.find("dynamic_resolution_timing=gpu") != std::string::npos);

    // CPU is the absent/default state. A Phase 38/39 scene that never named a
    // timing source must not gain a key on save.
    w.get<PostProcessComponent>(e)->dynamic_resolution_timing =
        kDynamicResolutionTimingCpu;
    const std::string cpu = serialize_scene_to_text(scene);
    NF_CHECK(cpu.find("dynamic_resolution=true") != std::string::npos);
    NF_CHECK(cpu.find("dynamic_resolution_timing=") == std::string::npos);
}

NF_TEST(scene_dynamic_resolution_round_trips_and_refuses_inverted_bounds) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_dynamic_bounds";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    // A well-formed line: the ceiling is 1.5, the floor 0.6, and the target is
    // 90 fps. All three values must survive a save/load cycle unchanged.
    NF_CHECK(vfs.write_text(
                  "content://Scenes/dynamic.nfscene",
                  std::string(kSceneHeader) +
                      "  PostProcess: dynamic_resolution=true min_render_scale=0.6"
                      " render_scale=1.5 target_fps=90 dynamic_resolution_timing=gpu\n")
                  .ok);
    auto loaded = load_scene_from_vfs(vfs, "content://Scenes/dynamic.nfscene");
    NF_CHECK(loaded.success);
    const auto* pp = loaded.scene->world().get<PostProcessComponent>(
        loaded.scene->world().all_entities()[0]);
    NF_CHECK(pp != nullptr);
    if (pp != nullptr) {
        NF_CHECK(pp->dynamic_resolution);
        NF_CHECK_NEAR(pp->render_scale, 1.5f, 1e-5f);
        NF_CHECK_NEAR(pp->target_fps, 90.0f, 1e-5f);
        NF_CHECK_NEAR(pp->min_render_scale, 0.6f, 1e-5f);
        NF_CHECK_EQ(pp->dynamic_resolution_timing, kDynamicResolutionTimingGpu);
    }

    // The timing source is a closed set. An unknown name is refused by name,
    // not treated as CPU: a typo must not silently run a different controller.
    for (const char* value : {"auto", "GPU", "1"}) {
        const std::string name = std::string("content://Scenes/timing_") + value + ".nfscene";
        NF_CHECK(vfs.write_text(
                      name,
                      std::string(kSceneHeader) +
                          "  PostProcess: dynamic_resolution=true"
                          " dynamic_resolution_timing=" + value + "\n")
                      .ok);
        auto timing = load_scene_from_vfs(vfs, name);
        NF_CHECK(timing.success);
        NF_CHECK(timing.scene->world().get<PostProcessComponent>(
                      timing.scene->world().all_entities()[0]) == nullptr);
        bool timing_warned = false;
        for (const std::string& warning : timing.warnings) {
            if (warning.find("dynamic_resolution_timing=") != std::string::npos) {
                timing_warned = true;
            }
        }
        NF_CHECK(timing_warned);
    }

    // Inverted bounds are refused by naming the floor key, not by swapping the
    // pair: the file would then mean a different range than it says.
    NF_CHECK(vfs.write_text(
                  "content://Scenes/inverted.nfscene",
                  std::string(kSceneHeader) +
                      "  PostProcess: render_scale=1.0 dynamic_resolution=true"
                      " min_render_scale=1.5\n")
                  .ok);
    auto inverted = load_scene_from_vfs(vfs, "content://Scenes/inverted.nfscene");
    NF_CHECK(inverted.success);
    NF_CHECK(inverted.scene->world().get<PostProcessComponent>(
                  inverted.scene->world().all_entities()[0]) == nullptr);
    bool floor_warned = false;
    for (const std::string& warning : inverted.warnings) {
        if (warning.find("min_render_scale=") != std::string::npos) floor_warned = true;
    }
    NF_CHECK(floor_warned);

    // The frame target uses the same bounds the editor slider does.
    for (const char* value : {"5", "1000", "nan"}) {
        const std::string name = std::string("content://Scenes/fps_") + value + ".nfscene";
        NF_CHECK(vfs.write_text(name,
                                std::string(kSceneHeader) +
                                    "  PostProcess: dynamic_resolution=true target_fps=" +
                                    value + "\n")
                     .ok);
        auto fps = load_scene_from_vfs(vfs, name);
        NF_CHECK(fps.success);
        NF_CHECK(fps.scene->world().get<PostProcessComponent>(
                      fps.scene->world().all_entities()[0]) == nullptr);
        bool fps_warned = false;
        for (const std::string& warning : fps.warnings) {
            if (warning.find("target_fps=") != std::string::npos) fps_warned = true;
        }
        NF_CHECK(fps_warned);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_without_post_process_writes_no_line) {
    // The compatibility half. A serializer that always emitted the line would
    // add a post block to every existing level on its first save, and — because
    // the defaults are neutral — nobody would notice until someone edited one.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_pp_absent";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("NoPost");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<Transform>(e, Transform{});

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/nopost.nfscene", scene, err));
    auto first = load_scene_from_vfs(vfs, "content://Scenes/nopost.nfscene");
    NF_CHECK(first.success);

    const std::string text = serialize_scene_to_text(*first.scene);
    NF_CHECK(text.find("PostProcess:") == std::string::npos);
    NF_CHECK(first.scene->world().get<PostProcessComponent>(
                  first.scene->world().all_entities()[0]) == nullptr);

    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_pushes_post_process_into_the_renderer) {
    // The integration claim. Everything above proves the data round-trips; this
    // proves the runtime does something with it. The observable is the
    // renderer's own state, not a counter, because "the component exists" is
    // true even when nothing reads it.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_rt_pp_apply";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        std::string(kSceneHeader) +
        "  PostProcess: bloom=true bloom_threshold=2.0 bloom_intensity=3.0"
        " grade=true grade_temperature=-0.5 sharpen=true sharpen_amount=1.5"
        " saturation=1.5 vignette=0.25 lens=true lens_distortion=0.3 lens_chroma=0.02"
        " dof=true dof_focus=18 dof_range=3.5 dof_radius=7"
        " motion=true motion_intensity=2.5 motion_length=0.07"
        " fxaa=true fxaa_threshold=0.2 fxaa_threshold_min=0.05 render_scale=1.5"
        " exposure=2.25 tonemap=aces lut=content://LUTs/grade.png lut_strength=0.6"
        " fog=true fog_color(0.7,0.6,0.5) fog_start=12 fog_end=40\n";
    NF_CHECK(vfs.write_text("content://Scenes/Post.nfscene", text).ok);

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
        // The Runtime owns the renderer, and the renderer owns RHI objects, so it
        // has to be destroyed BEFORE the device is shut down — otherwise the
        // device reports its live resources as leaks. Same for the target and
        // command buffer, which get their own scope inside draw_frame.
        {
            Runtime rt(vfs, reg, manager, *device, nullptr);
            std::string err;
            NF_CHECK(rt.load_scene("content://Scenes/Post.nfscene", err));

            // The block is pushed on the RENDER path, not on load — the same
            // place the sky and the lights are pushed, and for the same reason:
            // those are renderer state, and a scene that is loaded but never
            // drawn has no frame for them to apply to. So one frame has to
            // actually be recorded before the renderer's state means anything.
            auto draw_frame = [&](bool& ok) {
                rhi::TextureDesc td{};
                td.width = 64;
                td.height = 64;
                td.format = rhi::Format::R8G8B8A8_UNorm;
                td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
                auto target = device->create_texture(td);
                auto cmd = device->create_command_buffer();
                auto fence = device->create_fence(false);
                ok = (target != nullptr && cmd != nullptr && fence != nullptr);
                if (!ok) return;
                cmd->begin();
                rt.render_offscreen(*target, *cmd);
                cmd->end();
                device->submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
                ok = fence->wait(5000000000ULL);
                device->wait_idle();
            };

            bool ok = false;
            draw_frame(ok);
            NF_CHECK(ok);

            const rendering::Renderer3D* renderer = rt.renderer();
            NF_CHECK(renderer != nullptr);
            if (renderer != nullptr) {
                const rendering::PostFxParams& seen = renderer->postfx();
                NF_CHECK(seen.bloom.enabled);
                NF_CHECK_NEAR(seen.bloom.threshold, 2.0f, 1e-5f);
                NF_CHECK_NEAR(seen.bloom.intensity, 3.0f, 1e-5f);
                NF_CHECK(seen.grade.enabled);
                NF_CHECK_NEAR(seen.grade.temperature, -0.5f, 1e-5f);
                NF_CHECK(seen.sharpen.enabled);
                NF_CHECK_NEAR(seen.sharpen.amount, 1.5f, 1e-5f);
                NF_CHECK_NEAR(seen.saturation, 1.5f, 1e-5f);
                NF_CHECK_NEAR(seen.vignette, 0.25f, 1e-5f);
                NF_CHECK(seen.lens.enabled);
                NF_CHECK_NEAR(seen.lens.distortion, 0.3f, 1e-5f);
                NF_CHECK_NEAR(seen.lens.chromatic_aberration, 0.02f, 1e-6f);
                NF_CHECK(seen.dof.enabled);
                NF_CHECK_NEAR(seen.dof.focus_distance, 18.0f, 1e-5f);
                NF_CHECK_NEAR(seen.dof.focus_range, 3.5f, 1e-5f);
                NF_CHECK_NEAR(seen.dof.max_radius, 7.0f, 1e-5f);
                NF_CHECK(seen.motion.enabled);
                NF_CHECK_NEAR(seen.motion.intensity, 2.5f, 1e-5f);
                NF_CHECK_NEAR(seen.motion.max_length, 0.07f, 1e-6f);
                // Anti-aliasing is renderer state rather than a PostFxParams
                // stage (it is a separate pass on the tonemapped image), so it
                // is read back from the renderer.
                NF_CHECK(renderer->fxaa_enabled());
                NF_CHECK_NEAR(renderer->fxaa_threshold(), 0.2f, 1e-5f);
                NF_CHECK_NEAR(renderer->fxaa_threshold_min(), 0.05f, 1e-5f);
                // Render scale is a renderer property, not a PostFxParams
                // stage: the internal targets have already been rebuilt at
                // 1.5x the 64x64 offscreen target when this frame rendered.
                NF_CHECK_NEAR(renderer->render_scale(), 1.5f, 1e-5f);
                NF_CHECK_EQ(renderer->width(), 64u);
                NF_CHECK_EQ(renderer->render_width(), 96u);
                NF_CHECK_EQ(renderer->render_height(), 96u);
                // Exposure and the tonemap operator live on the renderer
                // rather than in PostFxParams, so they are read back from it.
                NF_CHECK_NEAR(renderer->exposure(), 2.25f, 1e-5f);
                NF_CHECK(renderer->tonemap_mode() == rendering::TonemapMode::ACES);
                NF_CHECK_NEAR(seen.lut_strength, 0.6f, 1e-5f);
                // The keys the line did not mention kept the component defaults,
                // which are the renderer's neutral values.
                NF_CHECK_NEAR(seen.bloom.knee, 0.5f, 1e-5f);
                NF_CHECK_NEAR(seen.grade.contrast, 1.0f, 1e-5f);
                // Fog is not in PostFxParams (the lighting pass applies it), so
                // it is read from the renderer's own state.
                NF_CHECK(renderer->fog().enabled);
                NF_CHECK_NEAR(renderer->fog().color.x, 0.7f, 1e-5f);
                NF_CHECK_NEAR(renderer->fog().color.y, 0.6f, 1e-5f);
                NF_CHECK_NEAR(renderer->fog().color.z, 0.5f, 1e-5f);
                NF_CHECK_NEAR(renderer->fog().start, 12.0f, 1e-5f);
                NF_CHECK_NEAR(renderer->fog().end, 40.0f, 1e-5f);
            }

            // A scene that says nothing about post-processing must not ERASE a
            // block an earlier scene set. "Absent means unchanged" is the rule
            // the sky follows too, and the opposite reading — absent means reset
            // — would silently strip the glow the moment a level without a
            // PostProcess line was loaded.
            NF_CHECK(vfs.write_text("content://Scenes/Plain.nfscene", kSceneHeader).ok);
            NF_CHECK(rt.load_scene("content://Scenes/Plain.nfscene", err));
            draw_frame(ok);
            NF_CHECK(ok);
            if (rt.renderer() != nullptr) {
                NF_CHECK_NEAR(rt.renderer()->postfx().bloom.intensity, 3.0f, 1e-5f);
                // And the OPT-IN pair survives too: a scene that says nothing
                // about exposure or the operator must not reset them to the
                // renderer's defaults, or loading any level would undo a
                // display preference the author set.
                NF_CHECK_NEAR(rt.renderer()->exposure(), 2.25f, 1e-5f);
                NF_CHECK(rt.renderer()->tonemap_mode() == rendering::TonemapMode::ACES);
            }
        }
        device->wait_idle();
        device->shutdown();
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_advances_dynamic_resolution_from_frame_time) {
    // The integration claim for Phase 38. The governor's own arithmetic is
    // covered in RHITests; this proves the Runtime actually advances it from
    // dt and pushes the resulting scale into the live renderer. Without this
    // test the feature could compile, parse, save, and do nothing.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_rt_dynamic_scale";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        std::string(kSceneHeader) +
        "  PostProcess: fxaa=true dynamic_resolution=true target_fps=240"
        " min_render_scale=0.5 render_scale=1.0\n";
    NF_CHECK(vfs.write_text("content://Scenes/Dynamic.nfscene", text).ok);

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
            NF_CHECK(rt.load_scene("content://Scenes/Dynamic.nfscene", err));

            auto draw_frame = [&](bool& ok) {
                rhi::TextureDesc td{};
                td.width = 64;
                td.height = 64;
                td.format = rhi::Format::R8G8B8A8_UNorm;
                td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
                auto target = device->create_texture(td);
                auto cmd = device->create_command_buffer();
                auto fence = device->create_fence(false);
                ok = (target != nullptr && cmd != nullptr && fence != nullptr);
                if (!ok) return;
                cmd->begin();
                rt.render_offscreen(*target, *cmd);
                cmd->end();
                device->submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
                ok = fence->wait(5000000000ULL);
                device->wait_idle();
            };

            bool ok = false;
            draw_frame(ok);
            NF_CHECK(ok);
            const rendering::Renderer3D* renderer = rt.renderer();
            NF_CHECK(renderer != nullptr);
            if (renderer != nullptr) {
                // The first frame configures the governor and starts at the
                // authored ceiling.
                NF_CHECK_NEAR(renderer->render_scale(), 1.0f, 1e-5f);
                NF_CHECK_EQ(renderer->width(), 64u);
                NF_CHECK_EQ(renderer->render_width(), 64u);
            }

            // 0.8 s of 20 ms frames against a 1/240 s budget: far outside the
            // band for longer than the 0.5 s cooldown. The governor must take
            // exactly one step (to 0.95) and no more.
            for (int i = 0; i < 40; ++i) {
                rt.update(0.020f);
            }
            draw_frame(ok);
            NF_CHECK(ok);
            if (rt.renderer() != nullptr) {
                NF_CHECK_NEAR(rt.renderer()->render_scale(), 0.95f, 1e-5f);
                NF_CHECK_EQ(rt.renderer()->width(), 64u); // output never moves
                NF_CHECK_EQ(rt.renderer()->render_width(), 61u);
            }

            // 0.8 s of 2 ms frames: under the lower edge of the band. One
            // upward step restores the ceiling and then stops there.
            for (int i = 0; i < 400; ++i) {
                rt.update(0.002f);
            }
            draw_frame(ok);
            NF_CHECK(ok);
            if (rt.renderer() != nullptr) {
                NF_CHECK_NEAR(rt.renderer()->render_scale(), 1.0f, 1e-5f);
                NF_CHECK_EQ(rt.renderer()->render_width(), 64u);
            }
        }
        device->wait_idle();
        device->shutdown();
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(runtime_feeds_gpu_frame_time_to_dynamic_resolution) {
    // Phase 40 integration. The governor's own fallback rules are covered in
    // RHITests; this test proves the Runtime actually enables frame timestamps
    // for a scene that asks for them, passes a fresh sample, and stops using it
    // when no new frame was rendered. A cheap 64x64 GPU frame against a 30 fps
    // budget is far inside the band, while every 40 ms CPU duration is outside
    // it — so a CPU fallback is visible as a scale step and a GPU decision is
    // visible as "keep 1.0".
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_rt_dynamic_gpu_timing";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    const std::string text =
        std::string(kSceneHeader) +
        "  PostProcess: fxaa=true dynamic_resolution=true target_fps=30"
        " min_render_scale=0.5 render_scale=1.0 dynamic_resolution_timing=gpu\n";
    NF_CHECK(vfs.write_text("content://Scenes/DynamicGpu.nfscene", text).ok);

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
    if (!device->supports_timestamp_queries()) {
        device->shutdown();
        std::filesystem::remove_all(tmp);
        NF_SKIP("timestamp queries unsupported by this device/queue");
    }

    {
        AssetRegistry reg;
        AssetManager manager(vfs, reg);
        {
            Runtime rt(vfs, reg, manager, *device, nullptr);
            std::string err;
            NF_CHECK(rt.load_scene("content://Scenes/DynamicGpu.nfscene", err));

            auto draw_frame = [&](bool& ok) {
                rhi::TextureDesc td{};
                td.width = 64;
                td.height = 64;
                td.format = rhi::Format::R8G8B8A8_UNorm;
                td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
                auto target = device->create_texture(td);
                auto cmd = device->create_command_buffer();
                auto fence = device->create_fence(false);
                ok = (target != nullptr && cmd != nullptr && fence != nullptr);
                if (!ok) return;
                cmd->begin();
                rt.render_offscreen(*target, *cmd);
                cmd->end();
                device->submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
                ok = fence->wait(5000000000ULL);
                device->wait_idle();
            };

            bool ok = false;
            draw_frame(ok);
            NF_CHECK(ok);
            draw_frame(ok);
            NF_CHECK(ok);

            const rendering::Renderer3D* renderer = rt.renderer();
            NF_CHECK(renderer != nullptr);
            if (renderer != nullptr) {
                // The scene asked for GPU timing; the developer did not ask for
                // profiling. Frame timestamps must be on, per-pass drains off.
                NF_CHECK(renderer->gpu_timing_enabled());
                NF_CHECK(!renderer->gpu_pass_timing_enabled());
                NF_CHECK(renderer->last_stats().gpu_timing_valid);
                NF_CHECK(renderer->last_stats().gpu_sample_serial > 0);

                // One update per render means each update sees the sample
                // published by the previous draw. The 40 ms CPU duration is
                // past the band; the tiny GPU frame is not, so a CPU decision
                // would step down.
                for (int i = 0; i < 40; ++i) {
                    rt.update(0.040f);
                    draw_frame(ok);
                    NF_CHECK(ok);
                }
                NF_CHECK_NEAR(rt.renderer()->render_scale(), 1.0f, 1e-5f);

                // Now update without rendering. There is only one unconsumed
                // sample, and the serial gate must not let the next nineteen
                // updates reuse it. The CPU fallback is outside the band long
                // enough to accept a step; a draw applies the new scale.
                for (int i = 0; i < 20; ++i) {
                    rt.update(0.040f);
                }
                draw_frame(ok);
                NF_CHECK(ok);
                NF_CHECK(rt.renderer()->render_scale() < 1.0f);
            }
        }
        device->wait_idle();
        device->shutdown();
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_fog_keeps_defaults_when_only_the_switch_is_written) {
    // `fog=true` alone must be a complete line: the colour and band inherit the
    // renderer's neutral defaults rather than becoming zeroes (which would be a
    // black wall of haze with a division by zero in the ramp).
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_fog_min";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    NF_CHECK(vfs.write_text("content://Scenes/fog.nfscene",
                            std::string(kSceneHeader) + "  PostProcess: fog=true\n")
                 .ok);
    auto result = load_scene_from_vfs(vfs, "content://Scenes/fog.nfscene");
    NF_CHECK(result.success);
    const auto* pp =
        result.scene->world().get<PostProcessComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(pp);
    if (pp != nullptr) {
        NF_CHECK(pp->fog_enabled);
        NF_CHECK_NEAR(pp->fog_r, 0.62f, 1e-5f);
        NF_CHECK_NEAR(pp->fog_g, 0.66f, 1e-5f);
        NF_CHECK_NEAR(pp->fog_b, 0.70f, 1e-5f);
        NF_CHECK_NEAR(pp->fog_start, 60.0f, 1e-5f);
        NF_CHECK_NEAR(pp->fog_end, 120.0f, 1e-5f);
    }
    const std::string text = serialize_scene_to_text(*result.scene);
    NF_CHECK(text.find("fog=true") != std::string::npos);
    NF_CHECK(text.find("fog_start=60") != std::string::npos);
    NF_CHECK(text.find("fog_end=120") != std::string::npos);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_fog_refuses_an_invalid_colour_or_a_zero_end) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_scene_fog_bad";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    NF_CHECK(vfs.write_text("content://Scenes/bad_color.nfscene",
                            std::string(kSceneHeader) +
                                "  PostProcess: fog=true fog_color(-1,0,0)\n")
                 .ok);
    auto color = load_scene_from_vfs(vfs, "content://Scenes/bad_color.nfscene");
    NF_CHECK(color.success);
    NF_CHECK(color.scene->world().get<PostProcessComponent>(
                  color.scene->world().all_entities()[0]) == nullptr);
    bool color_warned = false;
    for (const std::string& warning : color.warnings) {
        if (warning.find("fog_color=") != std::string::npos) color_warned = true;
    }
    NF_CHECK(color_warned);

    NF_CHECK(vfs.write_text("content://Scenes/bad_end.nfscene",
                            std::string(kSceneHeader) +
                                "  PostProcess: fog=true fog_end=0\n")
                 .ok);
    auto end = load_scene_from_vfs(vfs, "content://Scenes/bad_end.nfscene");
    NF_CHECK(end.success);
    NF_CHECK(end.scene->world().get<PostProcessComponent>(
                  end.scene->world().all_entities()[0]) == nullptr);
    bool end_warned = false;
    for (const std::string& warning : end.warnings) {
        if (warning.find("fog_end=") != std::string::npos) end_warned = true;
    }
    NF_CHECK(end_warned);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_frame_policy_is_authored_and_round_trips) {
    // Phase 42: the window policy is one switch that reuses the governor's
    // ceiling/floor/target, so the file needs one key and nothing else.
    auto tmp = nf::test::temp_root() / "nf_pp_frame_policy";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    {
        assets::VirtualFileSystem vfs;
        vfs.mount("content://", tmp);
        auto scene = std::make_unique<scene::Scene>("FramePolicy");
        scene::Scene& s = *scene;
        ecs::Entity e = s.world().create_entity();
        PostProcessComponent pp;
        pp.dynamic_resolution = true;
        pp.frame_policy = true;
        pp.target_fps = 72.0f;
        pp.render_scale = 0.9f;
        pp.min_render_scale = 0.6f;
        s.world().add<PostProcessComponent>(e, pp);

        std::string err;
        NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/fp.nfscene", s, err));
        NF_CHECK(err.empty());

        auto result = load_scene_from_vfs(vfs, "content://Scenes/fp.nfscene");
        NF_CHECK(result.success);
        NF_CHECK(result.scene);
        if (result.scene) {
            const auto* loaded = result.scene->world().get<PostProcessComponent>(
                result.scene->world().all_entities()[0]);
            NF_CHECK(loaded != nullptr);
            if (loaded != nullptr) {
                NF_CHECK(loaded->frame_policy);
                NF_CHECK(loaded->dynamic_resolution);
                NF_CHECK_NEAR(loaded->target_fps, 72.0f, 1e-5f);
                NF_CHECK_NEAR(loaded->render_scale, 0.9f, 1e-5f);
                NF_CHECK_NEAR(loaded->min_render_scale, 0.6f, 1e-5f);
            }
        }
        // Written only when authored: a scene that never names it stays clean.
        const std::string saved = serialize_scene_to_text(*scene);
        NF_CHECK(saved.find("frame_policy=true") != std::string::npos);
    }
    std::filesystem::remove_all(tmp);
}
