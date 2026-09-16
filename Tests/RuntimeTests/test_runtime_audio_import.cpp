// RuntimeTests — scene audio import: buffer= resolves through VFS decode.
//
// Writes a real WAV + a hand-written .nfscene into a temp VFS mount, loads,
// and checks the component carries decoded samples (or a loud warning when
// the file is missing).

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Audio/Components.hpp>
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

} // namespace

NF_TEST(scene_audio_buffer_resolves_and_decodes) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_audio_import_test";
    std::filesystem::create_directories(tmp / "Audio");
    std::filesystem::create_directories(tmp / "Scenes");
    vfs.mount("content://", tmp);

    const auto wav = make_wav_mono16(22050, 440.0f, 0.1f);
    write_file(tmp / "Audio" / "shot.wav", wav.data(), wav.size());
    const std::string scene_text =
        "# NOVAForge Scene v1\nversion: 1\nname: AudioImport\nentity_count: 1\n---\n"
        "entity: 1:0\n"
        "  Transform: local(0,0,0) world(0,0,0) parent(4294967295:0)\n"
        "  Audio: buffer=content://Audio/shot.wav autoplay=true\n";
    write_file(tmp / "Scenes" / "a.nfscene", scene_text.data(), scene_text.size());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/a.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene);
    auto loaded_e = result.scene->world().all_entities()[0];
    const auto* aud = result.scene->world().get<audio::AudioComponent>(loaded_e);
    NF_CHECK(aud);
    // Decoded at the mix rate with real samples (no tone involved).
    NF_CHECK(aud->owned_buffer.sample_rate == audio::kDefaultSampleRate);
    NF_CHECK(aud->owned_buffer.frame_count() > 0);
    NF_CHECK(aud->resolved_buffer() != nullptr);
    for (const auto& w : result.warnings) {
        NF_CHECK(w.find("silent") == std::string::npos); // no silence warning
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(scene_audio_missing_buffer_warns_loudly) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_scene_audio_missing_test";
    std::filesystem::create_directories(tmp / "Scenes");
    vfs.mount("content://", tmp);

    const std::string scene_text =
        "# NOVAForge Scene v1\nversion: 1\nname: AudioMissing\nentity_count: 1\n---\n"
        "entity: 1:0\n"
        "  Transform: local(0,0,0) world(0,0,0) parent(4294967295:0)\n"
        "  Audio: buffer=content://Audio/ghost.wav\n";
    write_file(tmp / "Scenes" / "a.nfscene", scene_text.data(), scene_text.size());

    auto result = load_scene_from_vfs(vfs, "content://Scenes/a.nfscene");
    NF_CHECK(result.success); // missing audio never fails the scene
    auto loaded_e = result.scene->world().all_entities()[0];
    const auto* aud = result.scene->world().get<audio::AudioComponent>(loaded_e);
    NF_CHECK(aud);
    NF_CHECK(aud->owned_buffer.samples.empty());
    bool warned = false;
    for (const auto& w : result.warnings) {
        if (w.find("ghost.wav") != std::string::npos) warned = true;
    }
    NF_CHECK(warned);

    std::filesystem::remove_all(tmp);
}
