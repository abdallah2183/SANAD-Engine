// EditorTests — the editor path for the scene audio environment.
//
// RuntimeTests proves the three `.nfscene` lines reach the live mixer. Without
// this file they are loadable from a scene file and invisible in the editor, so
// authoring one means hand-writing a line — which is the gap this closes.
//
// The command-level cases need no device. The last one does, because it goes
// through `EditorApp::set_music`, which resolves the buffer path through the VFS
// (so an authored track is audible without a save/reload) and must PRESERVE the
// decoded samples when the path did not change.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Audio/Components.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::assets;
using namespace nf::scene;

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

/// A one-entity scene, so EditorApp has something to open.
ecs::Entity write_probe_scene(VirtualFileSystem& vfs, const std::string& logical) {
    scene::Scene scene("AudioEnvWork");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Cave"});
    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, logical, scene, err));
    return e;
}

} // namespace

// --- Reverb zone -------------------------------------------------------------

NF_TEST(inspector_reverb_zone_command_adds_edits_and_undoes_cleanly) {
    scene::Scene scene("ReverbInspector");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});

    // Absent until asked for: that is what keeps the `ReverbZone:` line out of
    // a saved scene the author never touched.
    NF_CHECK(!scene.world().has<audio::ReverbZoneComponent>(e));

    audio::ReverbZoneComponent edit;
    edit.radius = 20.0f;
    edit.inner_radius = 4.0f;
    edit.wet_gain = 0.6f;
    edit.decay_seconds = 2.5f;
    edit.pre_delay_seconds = 0.04f;
    edit.echo_spacing_seconds = 0.12f;
    std::string err;
    auto cmd = editor::make_reverb_zone_command(scene.world(), e, edit, err);
    NF_CHECK(cmd != nullptr);

    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* rz = scene.world().get<audio::ReverbZoneComponent>(e);
    NF_CHECK(rz != nullptr);
    if (rz != nullptr) {
        NF_CHECK_NEAR(rz->radius, 20.0f, 1e-6f);
        NF_CHECK_NEAR(rz->inner_radius, 4.0f, 1e-6f);
        NF_CHECK_NEAR(rz->wet_gain, 0.6f, 1e-6f);
        NF_CHECK_NEAR(rz->decay_seconds, 2.5f, 1e-6f);
    }
    // Undo of the edit that FIRST created it removes it, so the scene
    // round-trips to a file with no `ReverbZone:` line.
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(!scene.world().has<audio::ReverbZoneComponent>(e));
    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK(scene.world().has<audio::ReverbZoneComponent>(e));

    // Refusals, matching the loader's rules: a value the editor accepts and the
    // loader then drops would make the same scene load differently depending on
    // how it was authored.
    audio::ReverbZoneComponent zero_radius = edit;
    zero_radius.radius = 0.0f;
    NF_CHECK(editor::make_reverb_zone_command(scene.world(), e, zero_radius, err) == nullptr);
    NF_CHECK(!err.empty());

    audio::ReverbZoneComponent inner_past_outer = edit;
    inner_past_outer.inner_radius = edit.radius + 1.0f;
    NF_CHECK(editor::make_reverb_zone_command(scene.world(), e, inner_past_outer, err) == nullptr);

    audio::ReverbZoneComponent wet_high = edit;
    wet_high.wet_gain = 1.5f;
    NF_CHECK(editor::make_reverb_zone_command(scene.world(), e, wet_high, err) == nullptr);

    audio::ReverbZoneComponent nan = edit;
    nan.decay_seconds = std::numeric_limits<float>::quiet_NaN();
    NF_CHECK(editor::make_reverb_zone_command(scene.world(), e, nan, err) == nullptr);

    ecs::Entity dead{};
    NF_CHECK(editor::make_reverb_zone_command(scene.world(), dead, edit, err) == nullptr);
}

// --- Music / ambience --------------------------------------------------------

NF_TEST(inspector_music_command_adds_edits_and_undoes_cleanly) {
    scene::Scene scene("MusicInspector");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});

    audio::MusicComponent after;
    after.buffer_name = "content://Audio/theme.wav";
    after.volume = 0.7f;
    after.fade_in_seconds = 1.5f;

    std::string err;
    auto cmd = editor::make_music_command(scene.world(), e, after, err);
    NF_CHECK(cmd != nullptr);

    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* mus = scene.world().get<audio::MusicComponent>(e);
    NF_CHECK(mus != nullptr);
    if (mus != nullptr) {
        NF_CHECK(mus->buffer_name == "content://Audio/theme.wav");
        NF_CHECK_NEAR(mus->volume, 0.7f, 1e-6f);
        NF_CHECK_NEAR(mus->fade_in_seconds, 1.5f, 1e-6f);
    }
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(!scene.world().has<audio::MusicComponent>(e));

    // An empty path is refused: a component naming no file is silent, which
    // reads as a broken feature rather than an unfinished edit.
    audio::MusicComponent empty;
    NF_CHECK(editor::make_music_command(scene.world(), e, empty, err) == nullptr);
    NF_CHECK(!err.empty());

    audio::MusicComponent loud = after;
    loud.volume = 1.5f;
    NF_CHECK(editor::make_music_command(scene.world(), e, loud, err) == nullptr);

    audio::MusicComponent negative_fade = after;
    negative_fade.fade_in_seconds = -1.0f;
    NF_CHECK(editor::make_music_command(scene.world(), e, negative_fade, err) == nullptr);

    audio::MusicComponent nan = after;
    nan.volume = std::numeric_limits<float>::quiet_NaN();
    NF_CHECK(editor::make_music_command(scene.world(), e, nan, err) == nullptr);

    ecs::Entity dead{};
    NF_CHECK(editor::make_music_command(scene.world(), dead, after, err) == nullptr);
}

NF_TEST(inspector_ambience_command_adds_edits_and_undoes_cleanly) {
    scene::Scene scene("AmbienceInspector");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});

    audio::AmbienceComponent after;
    after.buffer_name = "content://Audio/wind.wav";
    after.fade_in_seconds = 3.0f;

    std::string err;
    auto cmd = editor::make_ambience_command(scene.world(), e, after, err);
    NF_CHECK(cmd != nullptr);

    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* amb = scene.world().get<audio::AmbienceComponent>(e);
    NF_CHECK(amb != nullptr);
    if (amb != nullptr) {
        NF_CHECK(amb->buffer_name == "content://Audio/wind.wav");
        NF_CHECK_NEAR(amb->fade_in_seconds, 3.0f, 1e-6f);
    }
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(!scene.world().has<audio::AmbienceComponent>(e));

    audio::AmbienceComponent empty;
    NF_CHECK(editor::make_ambience_command(scene.world(), e, empty, err) == nullptr);

    audio::AmbienceComponent negative_fade = after;
    negative_fade.fade_in_seconds = -0.5f;
    NF_CHECK(editor::make_ambience_command(scene.world(), e, negative_fade, err) == nullptr);

    ecs::Entity dead{};
    NF_CHECK(editor::make_ambience_command(scene.world(), dead, after, err) == nullptr);
}

// --- EditorApp: the path resolution and the sample-preservation rule ---------

NF_TEST(editor_set_music_resolves_the_path_and_keeps_the_samples) {
    const nf::test::GpuFixture& f = nf::test::require_gpu();
    auto& device = *f.device;

    auto tmp = std::filesystem::temp_directory_path() / "nf_ed_audio_env";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "Content" / "Audio");
    std::filesystem::create_directories(tmp / "Cache");

    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    const auto wav = make_wav_mono16(22050, 440.0f, 0.1f);
    write_file(tmp / "Content" / "Audio" / "theme.wav", wav.data(), wav.size());
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    editor::MusicEdit edit{};
    std::snprintf(edit.buffer, sizeof(edit.buffer), "%s", "content://Audio/theme.wav");
    edit.volume = 0.5f;
    edit.fade_in_seconds = 0.0f;

    // The setter decodes through the VFS, so the track is audible the moment it
    // is authored rather than after a save and reload.
    NF_CHECK(app.set_music(e, edit, err));
    NF_CHECK(err.empty());
    const auto* mus = world.get<audio::MusicComponent>(e);
    NF_CHECK(mus != nullptr);
    if (mus == nullptr) {
        std::filesystem::remove_all(tmp);
        return;
    }
    NF_CHECK(!mus->owned_buffer.samples.empty());
    NF_CHECK_NEAR(mus->volume, 0.5f, 1e-6f);
    const usize samples = mus->owned_buffer.samples.size();

    // Applying again with the SAME path must carry the decoded samples over
    // rather than re-reading the file — and must not drop them, which would
    // silence a track that is already playing. The file is DELETED first so the
    // two behaviours are distinguishable: a re-read would now fail, a carry-over
    // cannot notice.
    std::filesystem::remove(tmp / "Content" / "Audio" / "theme.wav");
    edit.volume = 0.9f;
    NF_CHECK(app.set_music(e, edit, err));
    NF_CHECK(err.empty());
    const auto* mus2 = world.get<audio::MusicComponent>(e);
    NF_CHECK(mus2 != nullptr);
    if (mus2 != nullptr) {
        NF_CHECK_NEAR(mus2->volume, 0.9f, 1e-6f);
        NF_CHECK(mus2->owned_buffer.samples.size() == samples);
        NF_CHECK(!mus2->owned_buffer.samples.empty());
    }

    // A path nothing can decode is refused and the live component is left
    // alone — the author is standing right here to be told.
    editor::MusicEdit bogus{};
    std::snprintf(bogus.buffer, sizeof(bogus.buffer), "%s", "content://Audio/missing.wav");
    bogus.volume = 0.25f;
    NF_CHECK(!app.set_music(e, bogus, err));
    NF_CHECK(!err.empty());
    const auto* mus3 = world.get<audio::MusicComponent>(e);
    NF_CHECK(mus3 != nullptr);
    if (mus3 != nullptr) {
        NF_CHECK(mus3->buffer_name == "content://Audio/theme.wav");
        NF_CHECK_NEAR(mus3->volume, 0.9f, 1e-6f);
    }

    std::filesystem::remove_all(tmp);
}
