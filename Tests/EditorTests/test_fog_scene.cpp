// The `PostProcess:` line's distance-fog keys, without a GPU: parse, defaults,
// save/load round trip, and the two values the line must refuse.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::assets;
using namespace nf::runtime;

namespace {

const char* kSceneHeader =
    "# SANAD Scene v1\nversion: 1\nname: Fog\nentity_count: 1\n"
    "---\nentity: 1:0\n  Name: Env\n"
    "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n";

} // namespace

NF_TEST(scene_fog_round_trips_through_the_writer) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_ed_fog_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    scene::Scene scene("FogScene");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    PostProcessComponent pp;
    pp.fog_enabled = true;
    pp.fog_r = 0.55f;
    pp.fog_g = 0.65f;
    pp.fog_b = 0.75f;
    pp.fog_start = 14.0f;
    pp.fog_end = 55.0f;
    scene.world().add<PostProcessComponent>(e, pp);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/fog.nfscene", scene, err));
    NF_CHECK(err.empty());

    auto loaded = load_scene_from_vfs(vfs, "content://Scenes/fog.nfscene");
    NF_CHECK(loaded.success);
    NF_CHECK(loaded.scene != nullptr);
    const auto* back = loaded.scene->world().get<PostProcessComponent>(
        loaded.scene->world().all_entities()[0]);
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK(back->fog_enabled);
        NF_CHECK_NEAR(back->fog_r, 0.55f, 1e-5f);
        NF_CHECK_NEAR(back->fog_g, 0.65f, 1e-5f);
        NF_CHECK_NEAR(back->fog_b, 0.75f, 1e-5f);
        NF_CHECK_NEAR(back->fog_start, 14.0f, 1e-5f);
        NF_CHECK_NEAR(back->fog_end, 55.0f, 1e-5f);
    }
    const std::string text = serialize_scene_to_text(*loaded.scene);
    NF_CHECK(text.find("fog=true") != std::string::npos);
    NF_CHECK(text.find("fog_color(") != std::string::npos);

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_fog_one_key_keeps_neutral_defaults) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_ed_fog_min";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    NF_CHECK(vfs.write_text("content://Scenes/fog.nfscene",
                            std::string(kSceneHeader) + "  PostProcess: fog=true\n")
                 .ok);
    auto loaded = load_scene_from_vfs(vfs, "content://Scenes/fog.nfscene");
    NF_CHECK(loaded.success);
    const auto* pp = loaded.scene->world().get<PostProcessComponent>(
        loaded.scene->world().all_entities()[0]);
    NF_CHECK(pp != nullptr);
    if (pp != nullptr) {
        NF_CHECK(pp->fog_enabled);
        NF_CHECK_NEAR(pp->fog_r, 0.62f, 1e-5f);
        NF_CHECK_NEAR(pp->fog_g, 0.66f, 1e-5f);
        NF_CHECK_NEAR(pp->fog_b, 0.70f, 1e-5f);
        NF_CHECK_NEAR(pp->fog_start, 60.0f, 1e-5f);
        NF_CHECK_NEAR(pp->fog_end, 120.0f, 1e-5f);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_fog_refuses_negative_colour_and_zero_end) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_ed_fog_bad";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    NF_CHECK(vfs.write_text("content://Scenes/bad_color.nfscene",
                            std::string(kSceneHeader) +
                                "  PostProcess: fog=true fog_color(-1,0,0)\n")
                 .ok);
    auto color = load_scene_from_vfs(vfs, "content://Scenes/bad_color.nfscene");
    NF_CHECK(color.success); // the scene is valid; the line is not
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
