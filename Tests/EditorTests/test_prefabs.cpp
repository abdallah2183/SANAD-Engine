// Editor prefabs: subtree clone, instantiate, apply, revert, undo — templates
// are plain .nfscene files, instances are linked subtrees.

#include <NF/Test/TestFramework.hpp>
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

#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

static std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
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

NF_TEST(prefab_delete_subtree_command) {
    scene::Scene scene("Del");
    ecs::Entity root = make_named(scene, "Root");
    make_named(scene, "Child", root);
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
            }
        }
    }
    NF_CHECK(found);
    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK(scene.world().alive_entity_count() == 1u);

    // Dead roots are rejected cleanly.
    NF_CHECK(editor::DeleteSubtreeCommand::capture(scene.world(), root, err) == nullptr);
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
