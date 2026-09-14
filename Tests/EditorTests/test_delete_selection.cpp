// Editor delete: selection never survives its entity; undo restores content.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Editor/Selection.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

using namespace nf;

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
