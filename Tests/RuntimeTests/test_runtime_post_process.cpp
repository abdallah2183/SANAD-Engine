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
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_pp_rt";
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
    pp.exposure = 2.5f;
    pp.tonemap = 1; // ACES
    pp.lut_path = "content://LUTs/teal_orange.png";
    pp.lut_strength = 0.75f;
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
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_post_process_line_keeps_defaults_for_absent_keys) {
    // The line is written by hand as often as by the editor, so a one-key line
    // must be a complete line: `bloom=true` alone means "bloom with the default
    // threshold, knee, intensity and radius", not "bloom with zeroes".
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_pp_min";
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
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_post_process_line_refuses_a_zero_radius) {
    // A bloom radius of 0 collapses all thirteen taps onto one texel — a blur
    // that blurs nothing, which reads as "bloom is broken" rather than "bloom is
    // misconfigured". The line is ignored with a warning that names the key.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_pp_bad";
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

NF_TEST(scene_without_post_process_writes_no_line) {
    // The compatibility half. A serializer that always emitted the line would
    // add a post block to every existing level on its first save, and — because
    // the defaults are neutral — nobody would notice until someone edited one.
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_pp_absent";
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
    auto tmp = std::filesystem::temp_directory_path() / "nf_rt_pp_apply";
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
        " exposure=2.25 tonemap=aces lut=content://LUTs/grade.png lut_strength=0.6\n";
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
                // Exposure and the tonemap operator live on the renderer
                // rather than in PostFxParams, so they are read back from it.
                NF_CHECK_NEAR(renderer->exposure(), 2.25f, 1e-5f);
                NF_CHECK(renderer->tonemap_mode() == rendering::TonemapMode::ACES);
                NF_CHECK_NEAR(seen.lut_strength, 0.6f, 1e-5f);
                // The keys the line did not mention kept the component defaults,
                // which are the renderer's neutral values.
                NF_CHECK_NEAR(seen.bloom.knee, 0.5f, 1e-5f);
                NF_CHECK_NEAR(seen.grade.contrast, 1.0f, 1e-5f);
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
