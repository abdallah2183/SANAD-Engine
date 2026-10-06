// Editor play mode: clone isolation and session guards.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/PlayMode.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <vector>

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

// --- Duplicate names are NOT a structural difference -------------------------
//
// Runtime assigns a name synthesized from the entity's KIND to every entity that
// has no `Name:` line, so a scene authored without names has several entities
// called "Mesh". `scenes_equal_structure` used to index the saved scene by name
// into a SINGLE slot, so every live "Mesh" was compared against the LAST saved
// "Mesh": a scene that round-tripped perfectly reported
// "transform values differ for 'Mesh'" and the editor exited 1 — pointing the
// reader at the serializer for a corruption that was never there.

static void build_duplicate_names(scene::Scene& scene) {
    for (int i = 0; i < 2; ++i) {
        ecs::Entity e = scene.world().create_entity();
        scene.world().add<scene::Transform>(e, scene::Transform{});
        scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Mesh"});
        // Genuinely different, so collapsing the two onto one slot cannot pass.
        scene.world().get<scene::Transform>(e)->local_x = static_cast<f32>(i) * 5.0f;
    }
}

NF_TEST(editor_play_duplicate_names_roundtrip) {
    scene::Scene scene("Dup");
    build_duplicate_names(scene);

    auto clone = editor::clone_scene(scene);
    NF_CHECK(clone != nullptr);

    std::string diff;
    NF_CHECK(editor::scenes_equal_structure(scene, *clone, diff));
    NF_CHECK(diff.empty());
}

NF_TEST(editor_play_duplicate_names_detect_real_difference) {
    scene::Scene scene("Dup");
    build_duplicate_names(scene);
    auto clone = editor::clone_scene(scene);

    // Move the SECOND duplicate only. A "fix" that collapsed duplicates (compare
    // only the first of each name) would pass the round-trip test above and miss
    // this one.
    std::vector<ecs::Entity> meshes;
    for (ecs::Entity e : clone->world().all_entities()) {
        meshes.push_back(e);
    }
    NF_CHECK(meshes.size() == 2);
    clone->world().get<scene::Transform>(meshes[1])->local_x = 42.0f;

    std::string diff;
    NF_CHECK(!editor::scenes_equal_structure(scene, *clone, diff));
    NF_CHECK(!diff.empty());
}

NF_TEST(editor_play_duplicate_names_detect_multiplicity_mismatch) {
    // Same entity count and the same set of names, but the multiplicity differs:
    // a has two "Mesh", b has one "Mesh" and one "Cube". The pairing must run
    // out of "Mesh" on the second live entity and say so, rather than quietly
    // pairing it with an unrelated entity.
    scene::Scene a("Dup");
    build_duplicate_names(a);

    scene::Scene b("Dup");
    {
        ecs::Entity e = b.world().create_entity();
        b.world().add<scene::Transform>(e, scene::Transform{});
        b.world().add<scene::NameComponent>(e, scene::NameComponent{"Mesh"});
    }
    {
        ecs::Entity e = b.world().create_entity();
        b.world().add<scene::Transform>(e, scene::Transform{});
        b.world().add<scene::NameComponent>(e, scene::NameComponent{"Cube"});
    }

    std::string diff;
    NF_CHECK(!editor::scenes_equal_structure(a, b, diff));
    NF_CHECK(!diff.empty());
}
