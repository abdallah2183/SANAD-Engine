// Editor play mode: clone isolation and session guards.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/PlayMode.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

using namespace nf;

static void build_sample(scene::Scene& scene) {
    ecs::Entity root = scene.world().create_entity();
    scene.world().add<scene::Transform>(root, scene::Transform{});
    scene.world().add<scene::NameComponent>(root, scene::NameComponent{"Root"});
    scene.world().get<scene::Transform>(root)->local_x = 1.0f;
    ecs::Entity child = scene.world().create_entity();
    scene.world().add<scene::Transform>(child, scene::Transform{});
    scene.world().add<scene::NameComponent>(child, scene::NameComponent{"Child"});
    scene::set_parent(scene.world(), child, root);
    scene::propagate_transforms(scene.world());
}

NF_TEST(editor_play_clone_isolation) {
    scene::Scene edit("Edit");
    build_sample(edit);

    auto clone = editor::clone_scene(edit);
    NF_CHECK(clone != nullptr);
    // Mutating the clone must not touch the edit world.
    auto child = editor::find_by_name(*clone, "Child");
    NF_CHECK(child.has_value());
    clone->world().get<scene::Transform>(*child)->local_x = 99.0f;
    scene::propagate_transforms(clone->world());

    auto orig_child = editor::find_by_name(edit, "Child");
    NF_CHECK(orig_child.has_value());
    NF_CHECK_NEAR(edit.world().get<scene::Transform>(*orig_child)->local_x, 0.0f, 1e-6f);
    // Hierarchy survived the copy (parent link by name).
    const auto* tc = clone->world().get<scene::Transform>(*child);
    NF_CHECK(tc != nullptr && tc->parent.valid());
    std::string diff;
    NF_CHECK(!editor::scenes_equal_structure(edit, *clone, diff)); // diverged by edit
    NF_CHECK(!diff.empty());
}

NF_TEST(editor_play_clone_roundtrip) {
    scene::Scene edit("Edit");
    build_sample(edit);
    auto clone = editor::clone_scene(edit);
    std::string diff;
    NF_CHECK(editor::scenes_equal_structure(edit, *clone, diff));
    NF_CHECK(diff.empty());
}

NF_TEST(editor_play_session_guards) {
    scene::Scene edit("Edit");
    build_sample(edit);
    editor::PlaySession session;
    std::string err;
    NF_CHECK(!session.playing());
    NF_CHECK(!session.stop(err)); // stop without play fails
    NF_CHECK(session.play(edit, err));
    NF_CHECK(session.playing());
    NF_CHECK(session.snapshot() != nullptr);
    NF_CHECK(!session.play(edit, err)); // double play fails
    NF_CHECK(session.stop(err));
    NF_CHECK(!session.playing());

    scene::Scene empty("Empty");
    NF_CHECK(!session.play(empty, err)); // empty scene refuses to play
}
