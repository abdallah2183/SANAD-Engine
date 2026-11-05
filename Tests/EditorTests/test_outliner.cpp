// Editor outliner: hierarchy traversal, labels, rename, create/delete rows,
// collapse, and cyclic-hierarchy rejection.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Editor/Outliner.hpp>
#include <NF/Editor/Selection.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>

#include <filesystem>

using namespace nf;

static ecs::Entity make_named(scene::Scene& scene, const std::string& name, ecs::Entity parent = ecs::kInvalidEntity) {
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{name});
    if (parent.valid()) {
        scene::set_parent(scene.world(), e, parent);
    }
    return e;
}

NF_TEST(editor_outliner_cache_tracks_collapse_search_and_scene_switch) {
    const auto& fixture = nf::test::require_gpu();
    const auto tmp = nf::test::temp_root() / "nf_outliner_cache_navigation";
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    assets::VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    scene::Scene first("First");
    const auto root = make_named(first, "Root");
    make_named(first, "Hidden Child", root);
    scene::Scene second("Second");
    const auto other = make_named(second, "Other Root");
    make_named(second, "Other Child", other);
    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://first.nfscene", first, err));
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://second.nfscene", second, err));
    assets::AssetRegistry registry;
    assets::AssetManager manager(vfs, registry);
    runtime::Runtime runtime(vfs, registry, manager, *fixture.device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, registry, manager, console);
    app.attach_runtime(&runtime);
    NF_CHECK(app.open_scene("content://first.nfscene", err));
    auto rows = app.outliner_rows();
    NF_CHECK_EQ(rows.size(), 2u);
    const auto live_root = rows[0].entity;
    app.outliner().set_expanded(live_root, false);
    NF_CHECK_EQ(app.outliner_rows().size(), 1u);
    app.outliner().filter_text = "Hidden";
    rows = app.outliner_rows();
    NF_CHECK_EQ(rows.size(), 1u);
    NF_CHECK(rows[0].label == "Hidden Child");
    app.outliner().filter_text.clear();
    NF_CHECK_EQ(app.outliner_rows().size(), 1u);
    app.outliner().set_expanded(live_root, true);
    NF_CHECK_EQ(app.outliner_rows().size(), 2u);
    NF_CHECK(app.open_scene("content://second.nfscene", err));
    rows = app.outliner_rows();
    NF_CHECK_EQ(rows.size(), 2u);
    NF_CHECK(rows[0].label == "Other Root");
}

NF_TEST(editor_outliner_hierarchy) {
    scene::Scene scene("Outliner");
    ecs::Entity root = make_named(scene, "Root");
    ecs::Entity child = make_named(scene, "Child", root);
    ecs::Entity grand = make_named(scene, "Grand", child);
    ecs::Entity lone = make_named(scene, "Lone");
    (void)lone;
    scene::propagate_transforms(scene.world());

    editor::OutlinerState state;
    const auto rows = editor::build_outliner_rows(scene.world(), state);
    NF_CHECK_EQ(rows.size(), 4u);
    // DFS from id-sorted roots: Root, Child, Grand, then Lone.
    NF_CHECK(rows[0].label == "Root" && rows[0].depth == 0 && rows[0].has_children);
    NF_CHECK(rows[1].label == "Child" && rows[1].depth == 1 && rows[1].has_children);
    NF_CHECK(rows[2].label == "Grand" && rows[2].depth == 2 && !rows[2].has_children);
    NF_CHECK(rows[3].label == "Lone" && rows[3].depth == 0 && !rows[3].has_children);
    NF_CHECK(grand.valid());
}

NF_TEST(editor_outliner_collapse) {
    scene::Scene scene("Collapse");
    ecs::Entity root = make_named(scene, "Root");
    make_named(scene, "Child", root);

    editor::OutlinerState state;
    NF_CHECK_EQ(editor::build_outliner_rows(scene.world(), state).size(), 2u);
    state.set_expanded(root, false);
    const auto collapsed = editor::build_outliner_rows(scene.world(), state);
    NF_CHECK_EQ(collapsed.size(), 1u);
    NF_CHECK(collapsed[0].label == "Root");
    state.set_expanded(root, true);
    NF_CHECK_EQ(editor::build_outliner_rows(scene.world(), state).size(), 2u);
}

NF_TEST(editor_outliner_rename_label) {
    scene::Scene scene("Rename");
    ecs::Entity e = make_named(scene, "Before");
    NF_CHECK(editor::entity_label(scene.world(), e) == "Before");

    editor::CommandStack stack;
    auto cmd = std::make_unique<editor::RenameCommand>(e, std::string("After"));
    NF_CHECK(cmd->capture_old(scene.world()));
    stack.push(std::move(cmd), scene.world());
    NF_CHECK(editor::entity_label(scene.world(), e) == "After");
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(editor::entity_label(scene.world(), e) == "Before");

    // Unnamed entities fall back to a generated label.
    ecs::Entity bare = scene.world().create_entity();
    scene.world().add<scene::Transform>(bare, scene::Transform{});
    NF_CHECK(editor::entity_label(scene.world(), bare) == "Entity " + std::to_string(bare.id));
}

NF_TEST(editor_outliner_create_delete_rows) {
    scene::Scene scene("CreateDelete");
    editor::CommandStack stack;
    editor::OutlinerState state;

    stack.push(std::make_unique<editor::CreateEntityCommand>("A"), scene.world());
    stack.push(std::make_unique<editor::CreateEntityCommand>("B"), scene.world());
    NF_CHECK_EQ(editor::build_outliner_rows(scene.world(), state).size(), 2u);

    ecs::Entity a = stack.last_target();
    NF_CHECK(a.valid());
    std::string err;
    auto del = editor::DeleteEntityCommand::capture(scene.world(), a, err);
    NF_CHECK(del != nullptr);
    stack.push(std::move(del), scene.world());
    NF_CHECK_EQ(editor::build_outliner_rows(scene.world(), state).size(), 1u);
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK_EQ(editor::build_outliner_rows(scene.world(), state).size(), 2u);
}

NF_TEST(editor_outliner_empty_state) {
    scene::Scene scene("Empty");
    editor::OutlinerState state;
    NF_CHECK(editor::build_outliner_rows(scene.world(), state).empty());
}

NF_TEST(editor_outliner_cyclic_rejected) {
    scene::Scene scene("Cycle");
    ecs::Entity a = make_named(scene, "A");
    ecs::Entity b = make_named(scene, "B", a);
    // B is a descendant of A: making A a child of B must be rejected.
    NF_CHECK(editor::is_descendant_of(scene.world(), b, a));
    NF_CHECK(!editor::is_descendant_of(scene.world(), a, b));
    scene::set_parent(scene.world(), a, b);
    const auto* ta = scene.world().get<scene::Transform>(a);
    NF_CHECK(ta != nullptr && !ta->parent.valid());

    // Same guard through the command path.
    editor::CommandStack stack;
    stack.push(std::make_unique<editor::ReparentCommand>(a, ecs::kInvalidEntity, b), scene.world());
    const auto* ta2 = scene.world().get<scene::Transform>(a);
    NF_CHECK(ta2 != nullptr && !ta2->parent.valid());
}

NF_TEST(editor_outliner_filter_matches_case_insensitively) {
    scene::Scene scene("Filter");
    make_named(scene, "PlayerCube");
    make_named(scene, "Ground");
    make_named(scene, "player_shadow");

    editor::OutlinerState state;
    const auto rows = editor::build_outliner_rows(scene.world(), state);
    NF_CHECK_EQ(rows.size(), 3u);

    const auto all = editor::filter_outliner_rows(rows, "");
    NF_CHECK_EQ(all.size(), 3u);

    const auto hits = editor::filter_outliner_rows(rows, "player");
    NF_CHECK_EQ(hits.size(), 2u);
    if (hits.size() == 2) {
        // Flattened in entity-id order: the match is reachable even nested
        // under a collapsed parent.
        NF_CHECK(hits[0].depth == 0 && hits[1].depth == 0);
        NF_CHECK(hits[0].entity.id < hits[1].entity.id);
    }

    NF_CHECK_EQ(editor::filter_outliner_rows(rows, "GROUND").size(), 1u);
    NF_CHECK(editor::filter_outliner_rows(rows, "no_such_name").empty());
}
