// Editor drag & drop: mesh asset drop creates a wired entity (undoable).

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/AssetBrowser.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

using namespace nf;

static editor::AssetEntry mesh_entry() {
    editor::AssetEntry e;
    e.logical_path = "content://Meshes/cube.nfmesh";
    e.type = assets::AssetType::Mesh;
    e.id = assets::AssetId::generate();
    e.has_id = true;
    e.has_cooked = true;
    return e;
}

NF_TEST(editor_drop_mesh_creates_entity) {
    scene::Scene scene("Drop");
    editor::CommandStack stack;
    std::string err;
    auto cmd =
        editor::make_drop_mesh_command(mesh_entry(), "Dropped", ecs::kInvalidEntity, err);
    NF_CHECK(cmd != nullptr);
    stack.push(std::move(cmd), scene.world());

    ecs::Entity created = stack.last_target();
    NF_CHECK(created.valid() && scene.world().is_alive(created));
    const auto* mc = scene.world().get<runtime::MeshComponent>(created);
    NF_CHECK(mc != nullptr && mc->mesh_id.valid());
    NF_CHECK(scene.world().has<scene::Transform>(created));

    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(!scene.world().is_alive(created));
    NF_CHECK(stack.redo(scene.world()));
    ecs::Entity recreated = stack.last_target();
    NF_CHECK(recreated.valid() && scene.world().is_alive(recreated));
}

NF_TEST(editor_drop_mesh_rejects_non_mesh) {
    std::string err;
    editor::AssetEntry scene_file;
    scene_file.logical_path = "content://Scenes/Level.nfscene";
    scene_file.type = assets::AssetType::Scene;
    scene_file.is_scene_file = true;
    NF_CHECK(editor::make_drop_mesh_command(scene_file, "X", ecs::kInvalidEntity, err) == nullptr);
    NF_CHECK(!err.empty());

    editor::AssetEntry no_id;
    no_id.logical_path = "content://Meshes/broken.nfmesh";
    no_id.type = assets::AssetType::Mesh;
    NF_CHECK(editor::make_drop_mesh_command(no_id, "X", ecs::kInvalidEntity, err) == nullptr);
}

NF_TEST(editor_drop_mesh_under_parent) {
    scene::Scene scene("DropParent");
    ecs::Entity parent = scene.world().create_entity();
    scene.world().add<scene::Transform>(parent, scene::Transform{});
    editor::CommandStack stack;
    std::string err;
    auto cmd = editor::make_drop_mesh_command(mesh_entry(), "Child", parent, err);
    NF_CHECK(cmd != nullptr);
    stack.push(std::move(cmd), scene.world());
    ecs::Entity created = stack.last_target();
    const auto* t = scene.world().get<scene::Transform>(created);
    NF_CHECK(t != nullptr && t->parent == parent);
}
