// Editor prefabs: subtree clone, instantiate, apply, revert, undo — templates
// are plain .nfscene files, instances are linked subtrees.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/PlayMode.hpp>
#include <NF/Editor/Prefabs.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/PrefabLink.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Audio/Components.hpp>

#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

static std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = nf::test::temp_root() / name;
    std::filesystem::create_directories(p);
    return p;
}

static ecs::Entity make_named(scene::Scene& scene, const std::string& name,
                              ecs::Entity parent = ecs::kInvalidEntity) {
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{name});
    if (parent.valid()) {
        scene::set_parent(scene.world(), e, parent);
    }
    return e;
}

NF_TEST(prefab_clone_subtree_shape) {
    scene::Scene src("Src");
    ecs::Entity root = make_named(src, "Root");
    src.world().get<scene::Transform>(root)->local_x = 3.0f;
    ecs::Entity child = make_named(src, "Child", root);
    make_named(src, "Grand", child);
    make_named(src, "Outside");
    scene::propagate_transforms(src.world());

    scene::Scene dst("Dst");
    const ecs::Entity more = make_named(dst, "Anchor");
    const ecs::Entity nr = editor::clone_subtree(src.world(), root, dst.world(), more);
    NF_CHECK(nr.valid());
    scene::propagate_transforms(dst.world());

    // Shape preserved: Anchor + Root(+Child+Grand); Outside was never copied.
    NF_CHECK(dst.world().alive_entity_count() == 4u);
    const auto* t = dst.world().get<scene::Transform>(nr);
    NF_CHECK(t != nullptr && t->parent == more);
    NF_CHECK_NEAR(t->world_x, 3.0f, 1e-5f);
    auto kids = scene::get_children(dst.world(), nr);
    NF_CHECK(kids.size() == 1u);
    auto grand = scene::get_children(dst.world(), kids[0]);
    NF_CHECK(grand.size() == 1u);

    // Dead root clones to invalid, outside entity never copied twice.
    NF_CHECK(!editor::clone_subtree(src.world(), ecs::kInvalidEntity, dst.world(), more).valid());
    NF_CHECK(editor::collect_subtree(src.world(), root).size() == 3u);
}

NF_TEST(prefab_instantiate_command_undo) {
    scene::Scene tpl("Tpl");
    ecs::Entity r = make_named(tpl, "TRoot");
    make_named(tpl, "TChild", r);

    scene::Scene world_holder("W");
    editor::CommandStack stack;
    stack.push(std::make_unique<editor::InstantiatePrefabCommand>(
                   std::move(tpl), "content://Prefabs/T.nfscene", ecs::kInvalidEntity),
               world_holder.world());
    ecs::Entity created = stack.last_target();
    NF_CHECK(created.valid() && world_holder.world().is_alive(created));
    const auto* link = world_holder.world().get<scene::PrefabLinkComponent>(created);
    NF_CHECK(link != nullptr && link->prefab_path == "content://Prefabs/T.nfscene");
    NF_CHECK(world_holder.world().alive_entity_count() == 2u);

    NF_CHECK(stack.undo(world_holder.world()));
    NF_CHECK(world_holder.world().alive_entity_count() == 0u);
    NF_CHECK(stack.redo(world_holder.world()));
    NF_CHECK(world_holder.world().alive_entity_count() == 2u);
    ecs::Entity recreated = stack.last_target();
    const auto* link2 = world_holder.world().get<scene::PrefabLinkComponent>(recreated);
    NF_CHECK(link2 != nullptr && link2->prefab_path == "content://Prefabs/T.nfscene");
}

NF_TEST(prefab_redo_preserves_edits_to_multiple_children) {
    scene::Scene tpl("Template");
    const ecs::Entity root = make_named(tpl, "Root");
    make_named(tpl, "Left", root);
    make_named(tpl, "Right", root);
    scene::Scene scene("History");
    auto& world = scene.world();
    editor::CommandStack stack;
    stack.push(std::make_unique<editor::InstantiatePrefabCommand>(
                   std::move(tpl), "content://Prefabs/Example.nfscene", ecs::kInvalidEntity), world);
    const ecs::Entity first_root = stack.last_target();
    const auto children = scene::get_children(world, first_root);
    NF_CHECK(children.size() == 2);
    for (const auto child : children) {
        const auto original_name = world.get<scene::NameComponent>(child)->name;
        stack.push(std::make_unique<editor::RenameCommand>(child, original_name + " edited"), world);
    }
    NF_CHECK(stack.undo(world));
    NF_CHECK(stack.undo(world));
    NF_CHECK(stack.undo(world));
    NF_CHECK(world.alive_entity_count() == 0);
    NF_CHECK(stack.redo(world));
    const auto restored_children = scene::get_children(world, stack.last_target());
    NF_CHECK(restored_children.size() == 2);
    NF_CHECK(stack.redo(world));
    NF_CHECK(stack.redo(world));
    bool left = false, right = false;
    for (const auto child : restored_children) {
        const auto& name = world.get<scene::NameComponent>(child)->name;
        left |= name == "Left edited";
        right |= name == "Right edited";
    }
    NF_CHECK(left && right);
}

NF_TEST(prefab_delete_subtree_command) {
    scene::Scene scene("Del");
    ecs::Entity root = make_named(scene, "Root");
    const ecs::Entity child = make_named(scene, "Child", root);
    physics::RigidBodyComponent body;
    body.mass = 9.0f;
    scene.world().add<physics::RigidBodyComponent>(root, body);
    audio::AudioComponent sound;
    sound.volume = 0.42f;
    scene.world().add<audio::AudioComponent>(child, sound);
    make_named(scene, "Outside");

    editor::CommandStack stack;
    std::string err;
    auto del = editor::DeleteSubtreeCommand::capture(scene.world(), root, err);
    NF_CHECK(del != nullptr);
    stack.push(std::move(del), scene.world());
    NF_CHECK(scene.world().alive_entity_count() == 1u); // only Outside survives

    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(scene.world().alive_entity_count() == 3u);
    // Hierarchy restored (child under a Root again).
    bool found = false;
    for (ecs::Entity e : scene.world().all_entities()) {
        const auto* n = scene.world().get<scene::NameComponent>(e);
        if (n != nullptr && n->name == "Child") {
            const auto* t = scene.world().get<scene::Transform>(e);
            if (t != nullptr && t->parent.valid()) {
                const auto* pn = scene.world().get<scene::NameComponent>(t->parent);
                found = (pn != nullptr && pn->name == "Root");
                NF_CHECK_NEAR(scene.world().get<physics::RigidBodyComponent>(t->parent)->mass, 9.0f, 1e-6f);
                NF_CHECK_NEAR(scene.world().get<audio::AudioComponent>(e)->volume, 0.42f, 1e-6f);
            }
        }
    }
    NF_CHECK(found);
    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK(scene.world().alive_entity_count() == 1u);

    // Dead roots are rejected cleanly.
    NF_CHECK(editor::DeleteSubtreeCommand::capture(scene.world(), root, err) == nullptr);
}

NF_TEST(prefab_delete_subtree_remaps_all_previous_commands) {
    scene::Scene scene("DeleteTreeHistory");
    auto& world = scene.world();
    editor::CommandStack stack;
    stack.push(std::make_unique<editor::CreateEntityCommand>("TreeRoot"), world);
    const ecs::Entity root = stack.last_target();
    stack.push(std::make_unique<editor::CreateEntityCommand>("TreeChild", root), world);
    const ecs::Entity child = stack.last_target();
    std::string err;
    auto del = editor::DeleteSubtreeCommand::capture(world, root, err);
    NF_CHECK(del != nullptr);
    stack.push(std::move(del), world);
    NF_CHECK(stack.undo(world));
    const ecs::Entity restored_child = stack.last_target();
    NF_CHECK(world.is_alive(restored_child));
    const ecs::Entity restored_root = world.get<scene::Transform>(restored_child)->parent;
    NF_CHECK(world.is_alive(restored_root));
    NF_CHECK(stack.undo(world));
    NF_CHECK(!world.is_alive(restored_child));
    NF_CHECK(stack.undo(world));
    NF_CHECK(!world.is_alive(restored_root));
    NF_CHECK(stack.redo(world));
    const ecs::Entity recreated_root = stack.last_target();
    NF_CHECK(stack.redo(world));
    const ecs::Entity recreated_child = stack.last_target();
    NF_CHECK(world.get<scene::Transform>(recreated_child)->parent == recreated_root);
    NF_CHECK(stack.redo(world));
    NF_CHECK(!world.is_alive(recreated_root) && !world.is_alive(recreated_child));
    NF_CHECK(stack.undo(world));
    NF_CHECK(world.alive_entity_count() == 2u);
    ecs::Entity child_again = ecs::kInvalidEntity;
    for (const ecs::Entity e : world.all_entities()) {
        const auto* name = world.get<scene::NameComponent>(e);
        if (name && name->name == "TreeChild") child_again = e;
    }
    NF_CHECK(child_again.valid());
    const ecs::Entity root_again = world.get<scene::Transform>(child_again)->parent;
    NF_CHECK(world.is_alive(root_again));
    NF_CHECK(world.get<scene::NameComponent>(root_again)->name == "TreeRoot");
    NF_CHECK(child.valid());
}

NF_TEST(prefab_revert_undo_remaps_original_creation) {
    scene::Scene scene("RevertHistory");
    auto& world = scene.world();
    editor::CommandStack stack;
    stack.push(std::make_unique<editor::CreateEntityCommand>("Original"), world);
    const ecs::Entity original = stack.last_target();
    std::string err;
    auto deletion = editor::DeleteSubtreeCommand::capture(world, original, err);
    NF_CHECK(deletion != nullptr);
    scene::Scene template_scene("Template");
    make_named(template_scene, "TemplateRoot");
    auto inst = std::make_unique<editor::InstantiatePrefabCommand>(
        std::move(template_scene), "content://Prefabs/Example.nfscene", ecs::kInvalidEntity);
    stack.push(std::make_unique<editor::RevertPrefabCommand>(std::move(deletion), std::move(inst)), world);
    NF_CHECK(!world.is_alive(original));
    NF_CHECK(stack.undo(world));
    const ecs::Entity restored = stack.last_target();
    NF_CHECK(world.is_alive(restored));
    NF_CHECK(world.get<scene::NameComponent>(restored)->name == "Original");
    NF_CHECK(stack.undo(world));
    NF_CHECK(!world.is_alive(restored));
    NF_CHECK(stack.redo(world));
    const ecs::Entity recreated = stack.last_target();
    NF_CHECK(stack.redo(world));
    NF_CHECK(!world.is_alive(recreated));
    NF_CHECK(stack.undo(world));
    const ecs::Entity restored_again = stack.last_target();
    NF_CHECK(world.is_alive(restored_again));
    NF_CHECK(world.get<scene::NameComponent>(restored_again)->name == "Original");
}

NF_TEST(prefab_app_create_apply_revert) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_prefab_app");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Content" / "Prefabs");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    scene::Scene scene("Work");
    ecs::Entity root = make_named(scene, "Tower");
    scene.world().get<scene::Transform>(root)->local_x = 2.0f;
    make_named(scene, "Top", root);
    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/Work.nfscene", scene, err));

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));

    auto tower = editor::find_by_name(*runtime.scene(), "Tower");
    NF_CHECK(tower.has_value());

    // Create template from the Tower subtree.
    NF_CHECK(app.create_prefab(*tower, "content://Prefabs/Tower.nfscene", err));
    NF_CHECK(vfs.exists("content://Prefabs/Tower.nfscene").value);
    auto tpl = runtime::load_scene_from_vfs(vfs, "content://Prefabs/Tower.nfscene");
    NF_CHECK(tpl.success && tpl.scene->world().alive_entity_count() == 2u);
    // Instance root got tagged.
    const auto* link =
        runtime.scene()->world().get<scene::PrefabLinkComponent>(*tower);
    NF_CHECK(link != nullptr && link->prefab_path == "content://Prefabs/Tower.nfscene");

    // Instantiate a second copy; outliner sees both (2 + 2).
    NF_CHECK(app.instantiate_prefab("content://Prefabs/Tower.nfscene", ecs::kInvalidEntity, err));
    NF_CHECK(app.status().entity_count == 4u);

    // Diverge the copy, then revert pulls the template state back.
    ecs::Entity copy = app.stack().last_target();
    NF_CHECK(copy.valid());
    editor::TransformEdit te = editor::read_transform(*app.world(), copy);
    te.px = 99.0f;
    NF_CHECK(app.set_transform(copy, te, err));
    NF_CHECK(app.revert_prefab(copy, err));
    ecs::Entity reverted = app.stack().last_target();
    NF_CHECK(reverted.valid());
    NF_CHECK_NEAR(app.world()->get<scene::Transform>(reverted)->local_x, 2.0f, 1e-5f);

    // Apply pushes instance state into the template file.
    editor::TransformEdit te2 = editor::read_transform(*app.world(), reverted);
    te2.px = 7.0f;
    NF_CHECK(app.set_transform(reverted, te2, err));
    NF_CHECK(app.apply_prefab(reverted, err));
    auto tpl2 = runtime::load_scene_from_vfs(vfs, "content://Prefabs/Tower.nfscene");
    NF_CHECK(tpl2.success);
    bool found_seven = false;
    for (ecs::Entity e : tpl2.scene->world().all_entities()) {
        const auto* t = tpl2.scene->world().get<scene::Transform>(e);
        const auto* n = tpl2.scene->world().get<scene::NameComponent>(e);
        if (t != nullptr && n != nullptr && n->name == "Tower" && t->local_x > 6.9f &&
            t->local_x < 7.1f) {
            found_seven = true;
        }
    }
    NF_CHECK(found_seven);

    // Bad paths and non-links are rejected without side effects.
    NF_CHECK(!app.create_prefab(reverted, "cache://X.nfscene", err)); // content:// required
    NF_CHECK(!app.create_prefab(reverted, "content://Prefabs/X.txt", err)); // .nfscene required
    NF_CHECK(!app.instantiate_prefab("content://Prefabs/Missing.nfscene", ecs::kInvalidEntity, err));

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}