// Tests/EditorTests/test_wiring_editor.cpp — Phase 25 (wiring authoring)
//
// Rule 0 for the editor: a component the inspector cannot author might as
// well not exist. These tests prove the Phase 25 authoring path end to end
// through EditorApp — destructible editing (whose setter existed with zero
// UI), audio buffer binding (previously unbindable), rigid body / collider
// attach-from-scratch, sky clear color, and the three new systems —
// including the save/load round trip that makes an authored scene survive a
// reopen.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/Inspector.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Vfx/Components.hpp>
#include <NF/Audio/Components.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

std::filesystem::path wiring_temp_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_probe_scene(VirtualFileSystem& vfs, const std::string& logical) {
    scene::Scene scene("WiringWork");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Rig"});

    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, logical, scene, err));
}

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

/// Minimal valid mono16 WAV so set_audio_buffer has something to decode.
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

} // namespace

NF_TEST(editor_destructible_attach_edit_detach) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = wiring_temp_dir("nf_ed_wiring_dst");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
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

    // One chunk cannot come apart: rejected at the door, nothing written.
    runtime::DestructibleComponent bad;
    bad.chunks = 1u;
    err.clear();
    NF_CHECK(!app.set_destructible(e, bad, err));
    NF_CHECK(!err.empty());
    NF_CHECK(!world.has<runtime::DestructibleComponent>(e));

    runtime::DestructibleComponent spec;
    spec.chunks = 8u;
    spec.damage_threshold = 4.0f;
    NF_CHECK(app.set_destructible(e, spec, err));
    const auto* comp = world.get<runtime::DestructibleComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_EQ(comp->chunks, 8u);
        NF_CHECK_NEAR(comp->damage_threshold, 4.0f, 1e-6f);
    }

    NF_CHECK(app.detach_destructible(e, err));
    NF_CHECK(!world.has<runtime::DestructibleComponent>(e));
    err.clear();
    NF_CHECK(!app.detach_destructible(e, err));
    NF_CHECK(!err.empty());

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_audio_buffer_attach_decodes_and_round_trips) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = wiring_temp_dir("nf_ed_wiring_audio");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");
    const auto wav = make_wav_mono16(22050, 440.0f, 0.1f);
    NF_CHECK(vfs.write_bytes("content://Audio/shot.wav", std::span<const u8>(wav)).ok);

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

    // Missing files are refused rather than stored as a silent source.
    err.clear();
    NF_CHECK(!app.set_audio_buffer(e, "content://Audio/ghost.wav", err));
    NF_CHECK(!err.empty());

    NF_CHECK(app.set_audio_buffer(e, "content://Audio/shot.wav", err));
    const auto* comp = world.get<audio::AudioComponent>(e);
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_EQ(comp->buffer_name, std::string("content://Audio/shot.wav"));
        NF_CHECK(!comp->owned_buffer.samples.empty());
    }

    // Tune the falloff, save, and reopen through a fresh app: the buffer and
    // the range must both survive (the range never did before Phase 25).
    audio::AudioComponent tuned = *world.get<audio::AudioComponent>(e);
    tuned.spatial = true;
    tuned.spatial_settings.min_distance = 2.0f;
    tuned.spatial_settings.max_distance = 30.0f;
    NF_CHECK(app.set_audio(e, tuned, err));
    NF_CHECK(app.save(err));

    AssetRegistry reg2;
    AssetManager manager2(vfs, reg2);
    runtime::Runtime runtime2(vfs, reg2, manager2, device, nullptr);
    editor::ConsoleBuffer console2;
    editor::EditorApp app2(vfs, reg2, manager2, console2);
    app2.attach_runtime(&runtime2);
    NF_CHECK(app2.open_scene("content://Scenes/Work.nfscene", err));
    const auto* comp2 =
        runtime2.edit_scene()->world().get<audio::AudioComponent>(
            runtime2.edit_scene()->world().all_entities().front());
    NF_CHECK(comp2 != nullptr);
    if (comp2 != nullptr) {
        NF_CHECK_EQ(comp2->buffer_name, std::string("content://Audio/shot.wav"));
        NF_CHECK(!comp2->owned_buffer.samples.empty());
        NF_CHECK_NEAR(comp2->spatial_settings.min_distance, 2.0f, 1e-5f);
        NF_CHECK_NEAR(comp2->spatial_settings.max_distance, 30.0f, 1e-5f);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_sky_clear_round_trips) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = wiring_temp_dir("nf_ed_wiring_sky");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    editor::SkyEdit sky;
    sky.clear[0] = 0.5f;
    sky.clear[1] = 0.1f;
    sky.clear[2] = 0.9f;
    NF_CHECK(app.apply_sky_edit(sky, err));
    NF_CHECK(app.save(err));

    AssetRegistry reg2;
    AssetManager manager2(vfs, reg2);
    runtime::Runtime runtime2(vfs, reg2, manager2, device, nullptr);
    editor::ConsoleBuffer console2;
    editor::EditorApp app2(vfs, reg2, manager2, console2);
    app2.attach_runtime(&runtime2);
    NF_CHECK(app2.open_scene("content://Scenes/Work.nfscene", err));
    const auto* sky2 = runtime2.edit_scene()->world().get<runtime::SkyComponent>(
        runtime2.edit_scene()->world().all_entities().front());
    // apply_sky_edit creates the sky on its own entity when the scene has
    // none, so find it by component rather than by entity index.
    if (sky2 == nullptr) {
        for (ecs::Entity cand : runtime2.edit_scene()->world().all_entities()) {
            sky2 = runtime2.edit_scene()->world().get<runtime::SkyComponent>(cand);
            if (sky2 != nullptr) {
                break;
            }
        }
    }
    NF_CHECK(sky2 != nullptr);
    if (sky2 != nullptr) {
        NF_CHECK_NEAR(sky2->clear_r, 0.5f, 1e-5f);
        NF_CHECK_NEAR(sky2->clear_g, 0.1f, 1e-5f);
        NF_CHECK_NEAR(sky2->clear_b, 0.9f, 1e-5f);
    }

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_particles_cloth_character_round_trip) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = wiring_temp_dir("nf_ed_wiring_systems");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
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

    // Particles: invalid configs are rejected, valid ones persist.
    vfx::ParticleComponent bad_pc;
    bad_pc.config.rate = -1.0f;
    err.clear();
    NF_CHECK(!app.set_particles(e, bad_pc, err));
    vfx::ParticleComponent pc;
    pc.config.rate = 90.0f;
    NF_CHECK(app.set_particles(e, pc, err));

    physics::ClothComponent cc;
    cc.config.res_x = 8;
    NF_CHECK(app.set_cloth(e, cc, err));

    physics::CharacterComponent ch;
    ch.config.max_speed = 7.0f;
    NF_CHECK(app.set_character(e, ch, err));
    NF_CHECK(app.set_character_input(e, Vec3{0.0f, 0.0f, 1.0f}, false, err));
    NF_CHECK(app.save(err));

    AssetRegistry reg2;
    AssetManager manager2(vfs, reg2);
    runtime::Runtime runtime2(vfs, reg2, manager2, device, nullptr);
    editor::ConsoleBuffer console2;
    editor::EditorApp app2(vfs, reg2, manager2, console2);
    app2.attach_runtime(&runtime2);
    NF_CHECK(app2.open_scene("content://Scenes/Work.nfscene", err));
    const ecs::World& world2 = runtime2.edit_scene()->world();
    const ecs::Entity e2 = world2.all_entities().front();
    const auto* pc2 = world2.get<vfx::ParticleComponent>(e2);
    NF_CHECK(pc2 != nullptr);
    if (pc2 != nullptr) {
        NF_CHECK_NEAR(pc2->config.rate, 90.0f, 1e-5f);
    }
    const auto* cc2 = world2.get<physics::ClothComponent>(e2);
    NF_CHECK(cc2 != nullptr);
    if (cc2 != nullptr) {
        NF_CHECK_EQ(cc2->config.res_x, 8);
    }
    const auto* ch2 = world2.get<physics::CharacterComponent>(e2);
    NF_CHECK(ch2 != nullptr);
    if (ch2 != nullptr) {
        NF_CHECK_NEAR(ch2->config.max_speed, 7.0f, 1e-5f);
        // Live input is per-session: it must not have persisted.
        NF_CHECK_NEAR(ch2->wish_dir.z, 0.0f, 1e-6f);
    }

    // Detach removes all three.
    NF_CHECK(app2.detach_particles(e2, err));
    NF_CHECK(app2.detach_cloth(e2, err));
    NF_CHECK(app2.detach_character(e2, err));
    NF_CHECK(!world2.has<vfx::ParticleComponent>(e2));

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_rigid_body_and_collider_attach_from_scratch) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = wiring_temp_dir("nf_ed_wiring_physics");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    NF_CHECK(app.create_entity("Fresh", ecs::kInvalidEntity, err));
    const ecs::Entity e = app.stack().last_target();
    NF_CHECK(e.valid());

    // The inspector used to show these sections only when the component
    // already existed, with no way to create one. The setters always were
    // add-or-replace — the UI was the gap.
    physics::RigidBodyComponent rb;
    rb.mass = 5.0f;
    NF_CHECK(app.set_rigid_body(e, rb, err));
    physics::ColliderComponent col;
    col.shape = physics::Shape::make_box(Vec3{1.0f, 1.0f, 1.0f});
    NF_CHECK(app.set_collider(e, col, err));

    ecs::World& world = runtime.edit_scene()->world();
    NF_CHECK(world.has<physics::RigidBodyComponent>(e));
    NF_CHECK(world.has<physics::ColliderComponent>(e));

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_asset_folder_create_and_project_root) {
    // Unity parity: opening a project navigates the browser to its files, a
    // folder can be created from the panel, and a script created inside it
    // shows up in the tree (project listings scan the disk directly).
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = wiring_temp_dir("nf_ed_wiring_browser");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    vfs.mount("project://", tmp / "Proj");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));

    // Invalid folders are refused before touching the VFS.
    err.clear();
    NF_CHECK(!app.create_asset_folder("project://My Stuff", err));
    NF_CHECK(!err.empty());

    NF_CHECK(app.create_asset_folder("project://Scripts", err));
    err.clear();
    NF_CHECK(!app.create_asset_folder("project://Scripts", err));

    std::string created;
    NF_CHECK(app.create_unique_script_file("project://Scripts", "spin", created, err));
    NF_CHECK_EQ(created, std::string("project://Scripts/spin.lua"));

    // Opening the project flips the browser to its files.
    app.set_project((tmp / "Proj" / "Game.nfproj").string(), "Game");
    NF_CHECK_EQ(app.browser().browser_root, 1);
    const auto entries = app.browser_entries();
    bool saw_script = false;
    for (const auto& e : entries) {
        if (e.logical_path == "project://Scripts/spin.lua") {
            saw_script = true;
        }
    }
    NF_CHECK(saw_script);
    const auto folders = editor::asset_folders(entries);
    bool saw_folder = false;
    for (const auto& folder : folders) {
        if (folder == "project://Scripts") {
            saw_folder = true;
        }
    }
    NF_CHECK(saw_folder);

    std::filesystem::remove_all(tmp);
}

NF_TEST(editor_every_add_menu_component_attaches) {
    // The inspector's Add Component menu is one entry point over the same
    // validated setters the sections use. This test drives every menu item
    // through EditorApp on a fresh entity: whatever the menu offers must
    // actually attach (the user's "exists but not wired" complaint).
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = wiring_temp_dir("nf_ed_wiring_addmenu");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_probe_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    NF_CHECK(app.create_entity("Menu", ecs::kInvalidEntity, err));
    const ecs::Entity e = app.stack().last_target();
    NF_CHECK(e.valid());

    NF_CHECK(app.set_camera(e, editor::CameraEdit{}, err));
    NF_CHECK(app.set_light(e, editor::LightEdit{}, err));
    NF_CHECK(app.apply_sky_edit(editor::SkyEdit{}, err));
    NF_CHECK(app.set_rigid_body(e, physics::RigidBodyComponent{}, err));
    NF_CHECK(app.set_collider(e, physics::ColliderComponent{}, err));
    NF_CHECK(app.set_destructible(e, runtime::DestructibleComponent{}, err));
    NF_CHECK(app.attach_particles(e, err));
    NF_CHECK(app.attach_cloth(e, err));
    NF_CHECK(app.attach_character(e, err));

    ecs::World& world = runtime.edit_scene()->world();
    NF_CHECK(world.has<runtime::CameraComponent>(e));
    NF_CHECK(world.has<runtime::DirectionalLight>(e));
    NF_CHECK(world.has<physics::RigidBodyComponent>(e));
    NF_CHECK(world.has<physics::ColliderComponent>(e));
    NF_CHECK(world.has<runtime::DestructibleComponent>(e));
    NF_CHECK(world.has<vfx::ParticleComponent>(e));
    NF_CHECK(world.has<physics::ClothComponent>(e));
    NF_CHECK(world.has<physics::CharacterComponent>(e));
    // Sky attaches to its own entity when the scene has none.
    NF_CHECK(!world.query<runtime::SkyComponent>().empty());

    std::filesystem::remove_all(tmp);
}
