// Editor delete: selection never survives its entity; undo restores content.

#include <NF/Test/TestFramework.hpp>
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

#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
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
    // Child re-attached to the restored entity.
    const auto* tc = scene.world().get<scene::Transform>(child);
    NF_CHECK(tc != nullptr && tc->parent == restored);

    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK(!scene.world().is_alive(restored));
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
}
