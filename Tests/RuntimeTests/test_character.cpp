// Tests/RuntimeTests/test_character.cpp — the character: a glTF with a skin
// and animations, played by the engine.
//
// The engine could already SAMPLE a rig and turn a pose into a skin
// palette; what it could not do was put the two together and reach the GPU.
// These cases pin the whole chain: the file loads, the skeleton and the
// clips come out of it, the mesh deforms with the pose, and the movement
// decides which clip is playing.
//
// The character file is the one the repository ships (Tools/Character
// writes it), so the test exercises the real asset rather than a fixture
// that could drift away from it.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Animation/Components.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/GltfImport.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Runtime/CharacterComponent.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::scene;

#ifndef NF_CHARACTER_PATH
    #define NF_CHARACTER_PATH ""
#endif

namespace {

constexpr float kDt = 1.0f / 60.0f;

struct CharacterFixture {
    std::filesystem::path tmp;
    VirtualFileSystem vfs;
    AssetRegistry registry;
    AssetManager manager;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<Runtime> runtime;

    CharacterFixture() : manager(vfs, registry) {
        tmp = nf::test::temp_root() / "nf_character";
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp);
        vfs.mount("content://", tmp);

        device = rhi::create_device();
        if (!device) {
            return;
        }
        rhi::DeviceDesc desc{};
        desc.window_handle = nullptr;
        desc.enable_validation = false;
        if (!device->init(desc)) {
            device.reset();
            return;
        }
        runtime = std::make_unique<Runtime>(vfs, registry, manager, *device, nullptr);
    }

    ~CharacterFixture() {
        runtime.reset();
        if (device) {
            device->wait_idle();
            device->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }

    CharacterFixture(const CharacterFixture&) = delete;
    CharacterFixture& operator=(const CharacterFixture&) = delete;

    bool ok() const { return static_cast<bool>(runtime); }

    /// The character file, copied into the scene's own content so the scene
    /// refers to it the way a game would.
    std::string install_character() {
        const std::filesystem::path src(NF_CHARACTER_PATH);
        const std::filesystem::path dst = tmp / "Characters" / src.filename();
        std::filesystem::create_directories(tmp / "Characters");
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
        // The .bin sits beside the .gltf and the reader resolves it there.
        std::filesystem::path bin = src;
        bin.replace_extension(".bin");
        std::filesystem::path bin_dst = dst;
        bin_dst.replace_extension(".bin");
        if (std::filesystem::exists(bin)) {
            std::filesystem::copy_file(bin, bin_dst,
                                       std::filesystem::copy_options::overwrite_existing);
        }
        return "content://Characters/" + src.filename().string();
    }

    /// A scene with one character in it.
    ecs::Entity adopt_character(const std::string& source) {
        auto scene = std::make_unique<Scene>("Character");
        ecs::World& world = scene->world();
        ecs::Entity e = world.create_entity();
        world.add<Transform>(e, Transform{});
        CharacterComponent ch;
        ch.source = source;
        world.add<CharacterComponent>(e, ch);
        runtime->adopt_scene(std::move(scene), "content://Scenes/Character.nfscene");
        return e;
    }
};

} // namespace

NF_TEST(the_character_file_carries_a_skin_and_three_clips) {
    // The file is the source of truth: the engine reads a skin, a skeleton
    // and the clips from it, and nothing in the scene repeats them.
    const std::filesystem::path path(NF_CHARACTER_PATH);
    NF_CHECK(std::filesystem::exists(path));

    auto imported = import_gltf_file(path.string());
    NF_CHECK(imported.ok);
    if (!imported.ok) {
        return;
    }
    NF_CHECK_EQ(imported.meshes.size(), static_cast<size_t>(1));
    NF_CHECK_EQ(imported.skins.size(), static_cast<size_t>(1));
    NF_CHECK_EQ(imported.animations.size(), static_cast<size_t>(3));
    // Every clip is usable: the importer counts what it could not convert.
    NF_CHECK_EQ(imported.anim_channels_skipped, 0u);
    NF_CHECK_EQ(imported.skin_bindings_rejected, 0u);
    // The three clips are the ones the locomotion looks for.
    bool idle = false, walk = false, run = false;
    for (const auto& anim : imported.animations) {
        idle = idle || character_clip_is(anim.name, kCharacterIdleClip);
        walk = walk || character_clip_is(anim.name, kCharacterWalkClip);
        run = run || character_clip_is(anim.name, kCharacterRunClip);
    }
    NF_CHECK(idle);
    NF_CHECK(walk);
    NF_CHECK(run);
}

NF_TEST(a_character_in_a_scene_loads_its_skeleton_and_its_clips) {
    CharacterFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    const std::string source = f.install_character();
    f.adopt_character(source);

    ecs::World& world = f.runtime->edit_scene()->world();
    const ecs::Entity e = world.all_entities()[0];
    const auto* ch = world.get<CharacterComponent>(e);
    NF_CHECK(ch != nullptr);
    if (ch == nullptr) {
        return;
    }
    NF_CHECK(ch->loaded);
    NF_CHECK(ch->error.empty());
    // The skin data came through: four joints and four weights per vertex,
    // and one inverse bind matrix per bone.
    NF_CHECK(ch->joints.size() == ch->base_vertices.size() * 4);
    NF_CHECK(ch->weights.size() == ch->base_vertices.size() * 4);
    NF_CHECK(!ch->inverse_bind.empty());
    NF_CHECK(ch->mesh.valid());

    const auto* anim = world.get<animation::AnimationComponent>(e);
    NF_CHECK(anim != nullptr);
    if (anim != nullptr) {
        NF_CHECK(!anim->skeleton.bones.empty());
        NF_CHECK_EQ(anim->clips.size(), static_cast<size_t>(3));
        // The locomotion is wired: the states are the clips, and the speed
        // is the parameter that moves between them.
        NF_CHECK(anim->use_state_machine);
        NF_CHECK(anim->state_machine.state_count() >= 2);
    }
}

NF_TEST(a_character_mesh_deforms_with_its_pose) {
    CharacterFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    const std::string source = f.install_character();
    const ecs::Entity e = f.adopt_character(source);

    ecs::World& world = f.runtime->edit_scene()->world();
    const auto* ch = world.get<CharacterComponent>(e);
    NF_CHECK(ch != nullptr && ch->loaded);
    if (ch == nullptr || !ch->loaded) {
        return;
    }
    const std::vector<Vec3> bind_pose = [&] {
        std::vector<Vec3> out;
        for (const auto& v : ch->base_vertices) {
            out.push_back(Vec3{v.position[0], v.position[1], v.position[2]});
        }
        return out;
    }();

    // The idle clip bobs the hips, so a few frames of it must move the
    // vertices: a skinned character that never deforms is a character that
    // was never skinned.
    //
    // And the movement must be a MOVEMENT, not an explosion: a rig whose
    // inverse bind matrices are in the wrong layout bends the character out
    // of the world, and a test that only asks "did it move?" cannot tell the
    // two apart.
    bool moved = false;
    float worst = 0.0f;
    for (int frame = 0; frame < 30 && !moved; ++frame) {
        f.runtime->update(kDt);
        const auto* mesh_component = world.get<rendering::MeshComponent>(e);
        NF_CHECK(mesh_component != nullptr);
        if (mesh_component == nullptr) {
            return;
        }
        rendering::StaticMesh* mesh = f.runtime->mesh_library()
                                          ? f.runtime->mesh_library()->get(mesh_component->mesh)
                                          : nullptr;
        NF_CHECK(mesh != nullptr);
        if (mesh == nullptr || mesh->lods().empty()) {
            return;
        }
        const auto& vertices = mesh->lods()[0].vertices;
        NF_CHECK_EQ(vertices.size(), bind_pose.size());
        for (usize v = 0; v < vertices.size(); ++v) {
            const Vec3 now{vertices[v].position[0], vertices[v].position[1], vertices[v].position[2]};
            const float distance = (now - bind_pose[v]).length();
            if (distance > worst) {
                worst = distance;
            }
            if (distance > 1e-4f) {
                moved = true;
            }
        }
    }
    NF_CHECK(moved);
    // A character is 1.75 m tall. A vertex that moved further than a metre
    // is not walking; it is being flung.
    NF_CHECK(worst < 1.0f);
}

NF_TEST(a_moving_character_switches_to_the_walk_clip) {
    CharacterFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    const std::string source = f.install_character();
    const ecs::Entity e = f.adopt_character(source);

    ecs::World& world = f.runtime->edit_scene()->world();
    auto* anim = world.get<animation::AnimationComponent>(e);
    NF_CHECK(anim != nullptr);
    if (anim == nullptr) {
        return;
    }

    // Standing still: the idle clip is playing.
    f.runtime->update(kDt);
    const std::string idle_state = anim->state_machine.current_state();
    NF_CHECK(character_clip_is(idle_state, kCharacterIdleClip));

    // Moving: the state machine follows the speed, which is measured from
    // the transform. The character has to actually move for the speed to be
    // anything.
    auto* transform = world.get<Transform>(e);
    NF_CHECK(transform != nullptr);
    if (transform == nullptr) {
        return;
    }
    for (int frame = 0; frame < 30; ++frame) {
        transform->local_x += 0.05f;
        transform->dirty = true;
        f.runtime->update(kDt);
    }
    const std::string walking = anim->state_machine.current_state();
    NF_CHECK(character_clip_is(walking, kCharacterWalkClip));
}
