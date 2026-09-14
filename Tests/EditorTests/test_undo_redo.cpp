// Editor undo/redo: ordering, redo-clearing, and label reporting.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

using namespace nf;

NF_TEST(editor_undo_redo_sequence) {
    scene::Scene scene("UndoRedo");
    editor::CommandStack stack;
    std::string err = "unset";

    NF_CHECK(!stack.can_undo() && !stack.can_redo());
    NF_CHECK(!stack.undo(scene.world()));
    NF_CHECK(!stack.redo(scene.world()));

    stack.push(std::make_unique<editor::CreateEntityCommand>("First"), scene.world());
    stack.push(std::make_unique<editor::CreateEntityCommand>("Second"), scene.world());
    NF_CHECK_EQ(scene.world().alive_entity_count(), 2u);
    NF_CHECK(stack.can_undo() && !stack.can_redo());

    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK_EQ(scene.world().alive_entity_count(), 1u);
    NF_CHECK(stack.can_redo());
    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK_EQ(scene.world().alive_entity_count(), 2u);

    // A new push clears the redo stack.
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(stack.can_redo());
    stack.push(std::make_unique<editor::CreateEntityCommand>("Third"), scene.world());
    NF_CHECK(!stack.can_redo());
    NF_CHECK_EQ(scene.world().alive_entity_count(), 2u);
    (void)err;
}

NF_TEST(editor_undo_redo_transform) {
    scene::Scene scene("UndoTransform");
    editor::CommandStack stack;
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});

    scene::Transform before = *scene.world().get<scene::Transform>(e);
    scene::Transform mid = before;
    mid.local_x = 5.0f;
    scene::Transform after = before;
    after.local_x = 9.0f;
    stack.push(std::make_unique<editor::SetTransformCommand>(e, before, mid), scene.world());
    stack.push(std::make_unique<editor::SetTransformCommand>(e, mid, after), scene.world());
    NF_CHECK_NEAR(scene.world().get<scene::Transform>(e)->local_x, 9.0f, 1e-6f);
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK_NEAR(scene.world().get<scene::Transform>(e)->local_x, 5.0f, 1e-6f);
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK_NEAR(scene.world().get<scene::Transform>(e)->local_x, 0.0f, 1e-6f);
    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK_NEAR(scene.world().get<scene::Transform>(e)->local_x, 9.0f, 1e-6f);
}

NF_TEST(editor_undo_redo_labels) {
    scene::Scene scene("Labels");
    editor::CommandStack stack;
    NF_CHECK(stack.undo_label().empty() && stack.redo_label().empty());
    stack.push(std::make_unique<editor::CreateEntityCommand>("Named"), scene.world());
    NF_CHECK(!stack.undo_label().empty());
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(!stack.redo_label().empty());
    NF_CHECK(stack.undo_label().empty());
}
