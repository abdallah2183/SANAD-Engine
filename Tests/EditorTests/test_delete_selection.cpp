// Editor delete: selection never survives its entity; undo restores content.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/Outliner.hpp>
#include <NF/Editor/Selection.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Audio/Components.hpp>

#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = nf::test::temp_root() / name;
    std::filesystem::create_directories(p);
    return p;
}

} // namespace

NF_TEST(editor_delete_clears_selection) {
    scene::Scene scene("DeleteSel");
    editor::CommandStack stack;
    editor::Selection sel;

    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Doomed"});
    sel.set_single(e);
    NF_CHECK(sel.has_selection());

    std::string err;
    auto del = editor::DeleteEntityCommand::capture(scene.world(), e, err);
    NF_CHECK(del != nullptr);
    sel.remove(e);
    stack.push(std::move(del), scene.world());
    sel.prune(scene.world());
    NF_CHECK(!sel.has_selection());
    NF_CHECK(sel.all().empty());
}

NF_TEST(editor_delete_multi_prune) {
    scene::Scene scene("MultiPrune");
    editor::Selection sel;
    ecs::Entity a = scene.world().create_entity();
    scene.world().add<scene::Transform>(a, scene::Transform{});
    ecs::Entity b = scene.world().create_entity();
    scene.world().add<scene::Transform>(b, scene::Transform{});
    sel.set_single(a);
    sel.add(b);
    NF_CHECK_EQ(sel.all().size(), 2u);

    scene.world().destroy_entity(a);
    sel.prune(scene.world());
    NF_CHECK(!sel.contains(a));
    NF_CHECK(sel.contains(b));
    NF_CHECK(sel.primary() == b);
}

NF_TEST(editor_delete_undo_restores) {
    scene::Scene scene("DeleteUndo");
    editor::CommandStack stack;
    ecs::Entity parent = scene.world().create_entity();
    scene.world().add<scene::Transform>(parent, scene::Transform{});
    scene.world().add<scene::NameComponent>(parent, scene::NameComponent{"Parent"});
    ecs::Entity victim = scene.world().create_entity();
    scene.world().add<scene::Transform>(victim, scene::Transform{});
    scene.world().add<scene::NameComponent>(victim, scene::NameComponent{"Victim"});
    runtime::MeshComponent mesh;
    mesh.mesh_id = assets::AssetId::from_string("f04e488b-4dc8-414b-baee-e3c50e8829ad");
    mesh.material = "content://Materials/Default";
    scene.world().add<runtime::MeshComponent>(victim, mesh);
    runtime::PointLightComponent light;
    light.intensity = 4.25f;
    scene.world().add<runtime::PointLightComponent>(victim, light);
    runtime::SpriteComponent sprite;
    sprite.texture = "content://Textures/example.png";
    scene.world().add<runtime::SpriteComponent>(victim, sprite);
    physics::RigidBodyComponent body;
    body.mass = 7.0f;
    scene.world().add<physics::RigidBodyComponent>(victim, body);
    scene.world().add<physics::ColliderComponent>(victim, physics::ColliderComponent{});
    audio::AudioComponent audio;
    audio.volume = 0.37f;
    scene.world().add<audio::AudioComponent>(victim, audio);
    scene::set_parent(scene.world(), victim, parent);
    ecs::Entity child = scene.world().create_entity();
    scene.world().add<scene::Transform>(child, scene::Transform{});
    scene::set_parent(scene.world(), child, victim);

    std::string err;
    auto del = editor::DeleteEntityCommand::capture(scene.world(), victim, err);
    NF_CHECK(del != nullptr);
    stack.push(std::move(del), scene.world());
    NF_CHECK(!scene.world().is_alive(victim));
    // Child subtree survives, re-attached to the grandparent.
    NF_CHECK(scene.world().is_alive(child));

    NF_CHECK(stack.undo(scene.world()));
    // The restored entity carries a fresh handle: resolve it by name.
    ecs::Entity restored = ecs::kInvalidEntity;
    for (ecs::Entity e : scene.world().all_entities()) {
        const auto* nm = scene.world().get<scene::NameComponent>(e);
        if (nm != nullptr && nm->name == "Victim") {
            restored = e;
        }
    }
    NF_CHECK(restored.valid() && scene.world().is_alive(restored));
    const auto* n = scene.world().get<scene::NameComponent>(restored);
    NF_CHECK(n != nullptr && n->name == "Victim");
    NF_CHECK(scene.world().has<runtime::MeshComponent>(restored));
    NF_CHECK(scene.world().has<runtime::PointLightComponent>(restored));
    NF_CHECK_NEAR(scene.world().get<runtime::PointLightComponent>(restored)->intensity, 4.25f, 1e-6f);
    NF_CHECK(scene.world().get<runtime::SpriteComponent>(restored)->texture == "content://Textures/example.png");
    NF_CHECK_NEAR(scene.world().get<physics::RigidBodyComponent>(restored)->mass, 7.0f, 1e-6f);
    NF_CHECK(scene.world().has<physics::ColliderComponent>(restored));
    NF_CHECK_NEAR(scene.world().get<audio::AudioComponent>(restored)->volume, 0.37f, 1e-6f);
    NF_CHECK(scene.world().get<scene::Transform>(restored)->parent == parent);
    // Child re-attached to the restored entity.
    const auto* tc = scene.world().get<scene::Transform>(child);
    NF_CHECK(tc != nullptr && tc->parent == restored);

    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK(!scene.world().is_alive(restored));
    NF_CHECK(scene.world().get<scene::Transform>(child)->parent == parent);
    NF_CHECK(stack.undo(scene.world()));
    const ecs::Entity restored_again = scene.world().get<scene::Transform>(child)->parent;
    NF_CHECK(scene.world().is_alive(restored_again));
    NF_CHECK(scene.world().has<physics::ColliderComponent>(restored_again));
    NF_CHECK_NEAR(scene.world().get<audio::AudioComponent>(restored_again)->volume, 0.37f, 1e-6f);
}

NF_TEST(editor_undo_delete_remaps_earlier_create_and_edit) {
    scene::Scene scene("CreateEditDeleteUndo");
    auto& world = scene.world();
    editor::CommandStack stack;
    stack.push(std::make_unique<editor::CreateEntityCommand>("Original"), world);
    const ecs::Entity original = stack.last_target();
    stack.push(std::make_unique<editor::RenameCommand>(original, "Renamed"), world);
    std::string err;
    auto deletion = editor::DeleteEntityCommand::capture(world, original, err);
    NF_CHECK(deletion != nullptr);
    stack.push(std::move(deletion), world);
    NF_CHECK(stack.undo(world));
    const ecs::Entity restored = stack.last_target();
    NF_CHECK(world.is_alive(restored));
    NF_CHECK(world.get<scene::NameComponent>(restored)->name == "Renamed");
    NF_CHECK(stack.undo(world));
    NF_CHECK(world.get<scene::NameComponent>(restored)->name == "Original");
    NF_CHECK(stack.undo(world));
    NF_CHECK(!world.is_alive(restored));
    NF_CHECK(stack.redo(world));
    const ecs::Entity recreated = stack.last_target();
    NF_CHECK(stack.redo(world));
    NF_CHECK(world.get<scene::NameComponent>(recreated)->name == "Renamed");
    NF_CHECK(stack.redo(world));
    NF_CHECK(!world.is_alive(recreated));
}

// --- A4: Delete is immediate and undoable -----------------------------------
//
// The lead's report was "added a mesh, cannot delete it". Delete used to only
// ARM a pending delete and then wait on an inline Confirm button that was easy
// to miss, so from the outside nothing happened. The contract now is: one call
// deletes, the entity leaves BOTH the outliner rows and the world the viewport
// renders from, and Ctrl+Z (EditorApp::undo) brings it back.
//
// This is the app-level path the panels actually call; the command-level
// behaviour is already pinned by the three tests above.

NF_TEST(editor_delete_is_immediate_and_undoable) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_delete_flow");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    scene::Scene scene("DeleteFlow");
    ecs::Entity cam = scene.world().create_entity();
    scene.world().add<scene::Transform>(cam, scene::Transform{});
    runtime::CameraComponent cc;
    cc.is_active = true;
    scene.world().add<runtime::CameraComponent>(cam, cc);

    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/DeleteFlow.nfscene", scene, err));

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);
    NF_CHECK(app.open_scene("content://Scenes/DeleteFlow.nfscene", err));
    NF_CHECK(app.world() != nullptr);

    const size_t before = app.status().entity_count;

    // "add a mesh" — the exact gesture from the report.
    NF_CHECK(app.create_primitive(editor::PrimitiveKind::Cube, ecs::kInvalidEntity, err));
    const ecs::Entity cube = app.stack().last_target();
    NF_CHECK(cube.valid());
    NF_CHECK(app.world()->is_alive(cube));
    NF_CHECK(app.status().entity_count == before + 1);

    // The outliner draws from these rows; the viewport draws from the world.
    auto in_outliner = [&](ecs::Entity e) {
        for (const editor::OutlinerRow& r : app.outliner_rows()) {
            if (r.entity == e) {
                return true;
            }
        }
        return false;
    };
    NF_CHECK(in_outliner(cube));

    const auto* nm = app.world()->get<scene::NameComponent>(cube);
    NF_CHECK(nm != nullptr);
    const std::string cube_name = nm->name;

    // ONE call deletes it — no confirm step in between.
    app.selection().set_single(cube);
    NF_CHECK(app.delete_entity(cube, err));

    NF_CHECK(!app.world()->is_alive(cube));   // gone from the rendered world
    NF_CHECK(!in_outliner(cube));             // gone from the outliner
    NF_CHECK(app.status().entity_count == before);

    // Ctrl+Z is the safety net.
    NF_CHECK(app.undo(err));

    // Undo restores the entity under a FRESH handle, so resolve it by name.
    const auto restored = editor::find_by_name(*runtime.scene(), cube_name);
    NF_CHECK(restored.has_value());
    NF_CHECK(app.world()->is_alive(*restored));
    NF_CHECK(in_outliner(*restored));
    NF_CHECK(app.status().entity_count == before + 1);
    // The restored entity is still a mesh, not a name-only husk.
    NF_CHECK(app.world()->has<runtime::MeshComponent>(*restored));

    // Undoing creation after undoing deletion must remove the newly restored
    // handle; redoing both steps must recreate and delete it again.
    NF_CHECK(app.undo(err));
    NF_CHECK(!app.world()->is_alive(*restored));
    NF_CHECK(app.redo(err));
    const ecs::Entity recreated_cube = app.stack().last_target();
    NF_CHECK(app.world()->has<runtime::MeshComponent>(recreated_cube));
    NF_CHECK(app.redo(err));
    NF_CHECK(!app.world()->is_alive(recreated_cube));

    NF_CHECK(app.create_camera(ecs::kInvalidEntity, err));
    const ecs::Entity new_camera = app.stack().last_target();
    NF_CHECK(app.delete_entity(new_camera, err));
    NF_CHECK(app.undo(err));
    // An authored scene may already contain a camera with the same name; the
    // preceding create command tracks the exact restored entity handle.
    const ecs::Entity restored_camera = app.stack().last_target();
    NF_CHECK(app.world()->is_alive(restored_camera));
    NF_CHECK(app.world()->has<runtime::CameraComponent>(restored_camera));
    NF_CHECK(app.undo(err));
    NF_CHECK(!app.world()->is_alive(restored_camera));
    NF_CHECK(app.redo(err));
    const ecs::Entity recreated_camera = app.stack().last_target();
    NF_CHECK(app.world()->has<runtime::CameraComponent>(recreated_camera));
    NF_CHECK(app.redo(err));
    NF_CHECK(!app.world()->is_alive(recreated_camera));
}