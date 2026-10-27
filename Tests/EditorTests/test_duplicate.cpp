// Edit > Duplicate: subtree copies, unique names, one undo step, and a
// snapshot that survives deleting the originals (redo must not read them).

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Editor/SceneCopy.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <string>
#include <vector>

using namespace nf;

namespace {

ecs::Entity add_named(scene::Scene& scene, const std::string& name, ecs::Entity parent) {
    ecs::Entity e = scene.world().create_entity();
    scene::Transform t;
    scene.world().add<scene::Transform>(e, t);
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{name});
    if (parent.valid()) {
        scene::set_parent(scene.world(), e, parent);
    }
    return e;
}

ecs::Entity find_named(const scene::Scene& scene, const std::string& name) {
    for (ecs::Entity e : scene.world().all_entities()) {
        const auto* n = scene.world().get<scene::NameComponent>(e);
        if (n != nullptr && n->name == name) {
            return e;
        }
    }
    return ecs::kInvalidEntity;
}

size_t alive_count(const scene::Scene& scene) {
    size_t n = 0;
    for (ecs::Entity e : scene.world().all_entities()) {
        if (scene.world().is_alive(e)) {
            ++n;
        }
    }
    return n;
}

} // namespace

NF_TEST(duplicate_copies_a_whole_subtree_in_one_step) {
    scene::Scene scene("DupSubtree");
    editor::CommandStack stack;

    ecs::Entity root = add_named(scene, "Player", ecs::kInvalidEntity);
    ecs::Entity body = add_named(scene, "Body", root);
    ecs::Entity hand = add_named(scene, "Hand", body);
    auto* rt = scene.world().get<scene::Transform>(root);
    NF_CHECK(rt != nullptr);
    rt->local_x = 2.0f;
    rt->local_y = 1.0f;
    rt->local_z = -3.0f;

    runtime::MeshComponent mesh;
    mesh.material = "content://Materials/Default";
    scene.world().add<runtime::MeshComponent>(body, mesh);

    std::string err;
    auto cmd = editor::DuplicateEntitiesCommand::capture(scene, {root}, err);
    NF_CHECK(cmd != nullptr);
    if (cmd == nullptr) {
        return;
    }
    editor::DuplicateEntitiesCommand* raw = cmd.get();
    NF_CHECK_EQ(raw->created_roots().size(), 0u);
    stack.push(std::move(cmd), scene.world());

    NF_CHECK_EQ(alive_count(scene), 6u);
    NF_CHECK_EQ(raw->created_roots().size(), 1u);
    const ecs::Entity copy = raw->created_roots().front();
    NF_CHECK(scene.world().is_alive(copy));
    NF_CHECK(copy != root);

    const auto* nt = scene.world().get<scene::NameComponent>(copy);
    NF_CHECK(nt != nullptr && nt->name == "Player (1)");
    const auto* tc = scene.world().get<scene::Transform>(copy);
    NF_CHECK(tc != nullptr);
    NF_CHECK_NEAR(tc->local_x, 2.5f, 1e-6f); // visible offset, not on top
    NF_CHECK_NEAR(tc->local_y, 1.0f, 1e-6f);

    // Children were copied under the copy, and the mesh survived.
    const ecs::Entity body_copy = find_named(scene, "Body");
    const ecs::Entity hand_copy = find_named(scene, "Hand");
    NF_CHECK(body_copy.valid() && hand_copy.valid());
    // Names are not unique by design in the hierarchy, so resolve by parent.
    ecs::Entity copied_child = ecs::kInvalidEntity;
    ecs::Entity copied_hand = ecs::kInvalidEntity;
    for (ecs::Entity e : scene.world().all_entities()) {
        const auto* t = scene.world().get<scene::Transform>(e);
        if (t == nullptr) {
            continue;
        }
        if (t->parent == copy) {
            copied_child = e;
        } else if (copied_child.valid() && t->parent == copied_child) {
            copied_hand = e;
        }
    }
    NF_CHECK(copied_child.valid());
    NF_CHECK(copied_hand.valid());
    NF_CHECK(scene.world().has<runtime::MeshComponent>(copied_child));
    NF_CHECK_EQ(scene.world().get<runtime::MeshComponent>(copied_child)->material,
                std::string("content://Materials/Default"));

    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK_EQ(alive_count(scene), 3u);
    NF_CHECK(!scene.world().is_alive(copy));

    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK_EQ(alive_count(scene), 6u);
    NF_CHECK_EQ(raw->created_roots().size(), 1u);
    NF_CHECK(scene.world().is_alive(raw->created_roots().front()));
}

NF_TEST(duplicate_folds_a_selected_child_into_its_selected_ancestor) {
    scene::Scene scene("DupFold");
    editor::CommandStack stack;

    ecs::Entity root = add_named(scene, "Root", ecs::kInvalidEntity);
    ecs::Entity child = add_named(scene, "Child", root);
    add_named(scene, "Grandchild", child);

    std::string err;
    auto cmd = editor::DuplicateEntitiesCommand::capture(scene, {root, child}, err);
    NF_CHECK(cmd != nullptr);
    if (cmd == nullptr) {
        return;
    }
    editor::DuplicateEntitiesCommand* raw = cmd.get();
    stack.push(std::move(cmd), scene.world());

    // Exactly one full subtree (three entities), not root + child again.
    NF_CHECK_EQ(alive_count(scene), 6u);
    NF_CHECK_EQ(raw->created_roots().size(), 1u);
}

NF_TEST(duplicate_copies_multiple_roots_and_names_each_uniquely) {
    scene::Scene scene("DupMulti");
    editor::CommandStack stack;

    ecs::Entity a = add_named(scene, "Crate", ecs::kInvalidEntity);
    ecs::Entity b = add_named(scene, "Crate", ecs::kInvalidEntity);
    (void)a;

    std::string err;
    auto cmd = editor::DuplicateEntitiesCommand::capture(scene, {a, b}, err);
    NF_CHECK(cmd != nullptr);
    if (cmd == nullptr) {
        return;
    }
    editor::DuplicateEntitiesCommand* raw = cmd.get();
    stack.push(std::move(cmd), scene.world());
    NF_CHECK_EQ(raw->created_roots().size(), 2u);
    NF_CHECK_EQ(alive_count(scene), 4u);
    NF_CHECK(raw->created_roots()[0] != raw->created_roots()[1]);

    const auto* n0 = scene.world().get<scene::NameComponent>(raw->created_roots()[0]);
    const auto* n1 = scene.world().get<scene::NameComponent>(raw->created_roots()[1]);
    NF_CHECK(n0 != nullptr && n0->name == "Crate (1)");
    NF_CHECK(n1 != nullptr && n1->name == "Crate (2)");
}

NF_TEST(duplicate_redo_does_not_need_the_deleted_originals) {
    scene::Scene scene("DupSnapshot");
    editor::CommandStack stack;

    ecs::Entity root = add_named(scene, "Original", ecs::kInvalidEntity);
    ecs::Entity child = add_named(scene, "Child", root);

    std::string err;
    auto cmd = editor::DuplicateEntitiesCommand::capture(scene, {root}, err);
    NF_CHECK(cmd != nullptr);
    if (cmd == nullptr) {
        return;
    }
    stack.push(std::move(cmd), scene.world());

    // Remove the source subtree entirely, then undo/redo the duplicate.
    scene.world().destroy_entity(child);
    scene.world().destroy_entity(root);
    NF_CHECK_EQ(alive_count(scene), 2u); // the two copies remain

    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK_EQ(alive_count(scene), 0u);
    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK_EQ(alive_count(scene), 2u);

    ecs::Entity copy = find_named(scene, "Original (1)");
    NF_CHECK(copy.valid());
    NF_CHECK(scene.world().is_alive(copy));
}

NF_TEST(duplicate_with_nothing_selected_is_refused) {
    scene::Scene scene("DupEmpty");
    std::string err;
    auto cmd = editor::DuplicateEntitiesCommand::capture(scene, {}, err);
    NF_CHECK(cmd == nullptr);
    NF_CHECK(!err.empty());
}

NF_TEST(duplicate_carries_terrain_and_patrol_route_across_worlds) {
    // Phase 63/64: both components are scene data, not live handles, so they
    // must cross the source/destination worlds intact. A raw ECS record copy
    // could not do this; SceneCopy is the one place each component is listed.
    scene::Scene src("CopySrc");
    ecs::Entity from = add_named(src, "Walker", ecs::kInvalidEntity);

    runtime::TerrainComponent terrain;
    terrain.resolution = 64u;
    terrain.area = 75.0f;
    terrain.height_scale = 11.0f;
    terrain.noise_frequency = 0.025f;
    terrain.octaves = 4u;
    terrain.seed = 991u;
    terrain.heightmap = "content://Textures/Hills.png";
    terrain.material = "content://Materials/Ground.nfmat";
    src.world().add<runtime::TerrainComponent>(from, terrain);

    runtime::NavRouteComponent route;
    route.wait = 0.6f;
    route.loop = false;
    route.ping_pong = true;
    route.enabled = false;
    route.points.push_back(runtime::NavPointSpec{1.5f, 0.5f, -2.0f, -1.0f});
    route.points.push_back(runtime::NavPointSpec{9.0f, 0.5f, 8.0f, 2.25f});
    src.world().add<runtime::NavRouteComponent>(from, route);

    scene::Scene dst("CopyDst");
    ecs::Entity to = add_named(dst, "Walker", ecs::kInvalidEntity);
    editor::copy_entity_components(src.world(), from, dst.world(), to);

    const auto* copied_terrain = dst.world().get<runtime::TerrainComponent>(to);
    NF_CHECK(copied_terrain != nullptr);
    if (copied_terrain != nullptr) {
        NF_CHECK_EQ(copied_terrain->resolution, 64u);
        NF_CHECK_NEAR(copied_terrain->area, 75.0f, 1e-6f);
        NF_CHECK_NEAR(copied_terrain->height_scale, 11.0f, 1e-6f);
        NF_CHECK_NEAR(copied_terrain->noise_frequency, 0.025f, 1e-6f);
        NF_CHECK_EQ(copied_terrain->octaves, 4u);
        NF_CHECK_EQ(copied_terrain->seed, 991u);
        NF_CHECK(copied_terrain->heightmap == "content://Textures/Hills.png");
        NF_CHECK(copied_terrain->material == "content://Materials/Ground.nfmat");
    }

    const auto* copied_route = dst.world().get<runtime::NavRouteComponent>(to);
    NF_CHECK(copied_route != nullptr);
    if (copied_route != nullptr) {
        NF_CHECK_NEAR(copied_route->wait, 0.6f, 1e-6f);
        NF_CHECK(!copied_route->loop);
        NF_CHECK(copied_route->ping_pong);
        NF_CHECK(!copied_route->enabled);
        NF_CHECK_EQ(copied_route->points.size(), static_cast<usize>(2));
        if (copied_route->points.size() == 2) {
            NF_CHECK_NEAR(copied_route->points[0].x, 1.5f, 1e-6f);
            NF_CHECK_NEAR(copied_route->points[0].z, -2.0f, 1e-6f);
            NF_CHECK_NEAR(copied_route->points[0].wait, -1.0f, 1e-6f);
            NF_CHECK_NEAR(copied_route->points[1].y, 0.5f, 1e-6f);
            NF_CHECK_NEAR(copied_route->points[1].wait, 2.25f, 1e-6f);
        }
    }

    // The destination had neither before, and the source is untouched: a copy
    // is not a move.
    ecs::Entity other = add_named(dst, "Other", ecs::kInvalidEntity);
    NF_CHECK(!dst.world().has<runtime::TerrainComponent>(other));
    NF_CHECK(!dst.world().has<runtime::NavRouteComponent>(other));
    NF_CHECK(src.world().has<runtime::TerrainComponent>(from));
    NF_CHECK(src.world().has<runtime::NavRouteComponent>(from));
}
