// Tests/EditorTests/test_new_features_ui.cpp — UI and Editor authoring tests for Phases 115-122
//
// Verifies entity quick-add creation, Inspector component CRUD, parameter validation,
// and scene entity cloning for the newly implemented engine features.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Test/RHITestCommon.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <filesystem>
#include <string>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

std::filesystem::path feature_temp_dir(const std::string& name) {
    auto p = nf::test::temp_root() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_test_scene(VirtualFileSystem& vfs, const std::string& logical) {
    scene::Scene scene("FeatureTestWork");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Root"});

    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, logical, scene, err));
}

} // namespace

NF_TEST(editor_create_feature_entities) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_create_features");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity root = world.all_entities().front();

    // 1. Weather
    NF_CHECK(app.create_weather(root, err));
    ecs::Entity weather_ent = app.selection().primary();
    NF_CHECK(weather_ent.valid());
    NF_CHECK(world.has<runtime::WeatherComponent>(weather_ent));
    NF_CHECK(world.get<scene::NameComponent>(weather_ent)->name == "WeatherAtmosphere");

    // 2. Cable
    NF_CHECK(app.create_cable(root, err));
    ecs::Entity cable_ent = app.selection().primary();
    NF_CHECK(cable_ent.valid());
    NF_CHECK(world.has<runtime::CableComponent>(cable_ent));
    NF_CHECK(world.get<scene::NameComponent>(cable_ent)->name == "Cable");

    // 3. Rule Tile
    NF_CHECK(app.create_rule_tile(root, err));
    ecs::Entity rt_ent = app.selection().primary();
    NF_CHECK(rt_ent.valid());
    NF_CHECK(world.has<runtime::RuleTileComponent>(rt_ent));
    NF_CHECK(world.has<runtime::TilemapComponent>(rt_ent));
    NF_CHECK(world.get<scene::NameComponent>(rt_ent)->name == "RuleTile");

    // 4. Flow Field
    NF_CHECK(app.create_flow_field(root, err));
    ecs::Entity ff_ent = app.selection().primary();
    NF_CHECK(ff_ent.valid());
    NF_CHECK(world.has<runtime::FlowFieldComponent>(ff_ent));
    NF_CHECK(world.get<scene::NameComponent>(ff_ent)->name == "FlowFieldNav");

    // 5. Talent Tree
    NF_CHECK(app.create_talent_tree(root, err));
    ecs::Entity tt_ent = app.selection().primary();
    NF_CHECK(tt_ent.valid());
    NF_CHECK(world.has<runtime::TalentComponent>(tt_ent));
    NF_CHECK(world.get<scene::NameComponent>(tt_ent)->name == "TalentTree");
}

NF_TEST(editor_cable_component_crud) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_cable_crud");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_cable(e, err));
    NF_CHECK(world.has<runtime::CableComponent>(e));

    // Reject bad particle count (< 2)
    runtime::CableComponent bad;
    bad.particle_count = 1;
    NF_CHECK(!app.set_cable(e, bad, err));
    NF_CHECK(!err.empty());

    // Valid edit
    runtime::CableComponent good;
    good.total_length = 20.0f;
    good.particle_count = 32;
    good.stiffness = 0.88f;
    good.damping = 0.04f;
    good.linear_density = 1.2f;
    good.max_tension = 4200.0f;
    good.breakable = true;
    good.pin_start = true;
    good.pin_end = false;
    good.end_offset = Vec3{2.0f, -6.0f, 4.0f};
    good.enabled = true;
    NF_CHECK(app.set_cable(e, good, err));

    const auto* c = world.get<runtime::CableComponent>(e);
    NF_CHECK(c != nullptr);
    NF_CHECK(c->total_length == 20.0f);
    NF_CHECK(c->particle_count == 32);
    NF_CHECK(c->breakable);
    NF_CHECK(!c->pin_end);

    NF_CHECK(app.detach_cable(e, err));
    NF_CHECK(!world.has<runtime::CableComponent>(e));
}

NF_TEST(editor_weather_component_crud) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_weather_crud");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_weather(e, err));
    NF_CHECK(world.has<runtime::WeatherComponent>(e));

    runtime::WeatherComponent wc;
    wc.weather_type = 3; // Rain
    wc.fog_density = 0.04f;
    wc.fog_height_falloff = 0.06f;
    wc.fog_base_height = 5.0f;
    wc.fog_max_opacity = 0.95f;
    wc.fog_color[0] = 0.4f;
    wc.fog_color[1] = 0.5f;
    wc.fog_color[2] = 0.6f;
    wc.sun_inscattering = 0.45f;
    wc.ground_wetness = 0.85f;
    wc.wind_strength = 15.0f;
    wc.wind_direction[0] = 1.0f;
    wc.wind_direction[1] = 0.0f;
    wc.wind_direction[2] = 0.5f;
    wc.enabled = true;
    NF_CHECK(app.set_weather(e, wc, err));

    const auto* read = world.get<runtime::WeatherComponent>(e);
    NF_CHECK(read != nullptr);
    NF_CHECK(read->weather_type == 3);
    NF_CHECK(read->ground_wetness == 0.85f);
    NF_CHECK(read->wind_strength == 15.0f);

    NF_CHECK(app.detach_weather(e, err));
    NF_CHECK(!world.has<runtime::WeatherComponent>(e));
}

NF_TEST(editor_rule_tile_component_crud) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_ruletile_crud");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_rule_tile(e, err));
    NF_CHECK(world.has<runtime::RuleTileComponent>(e));

    runtime::RuleTileComponent rt;
    rt.default_tile_id = 42;
    rt.rule_preset = 1; // 47-blob Wang autotile
    rt.rule_count = 47;
    rt.auto_update = true;
    rt.enabled = true;
    NF_CHECK(app.set_rule_tile(e, rt, err));

    const auto* read = world.get<runtime::RuleTileComponent>(e);
    NF_CHECK(read != nullptr);
    NF_CHECK(read->default_tile_id == 42);
    NF_CHECK(read->rule_preset == 1);
    NF_CHECK(read->rule_count == 47);

    NF_CHECK(app.detach_rule_tile(e, err));
    NF_CHECK(!world.has<runtime::RuleTileComponent>(e));
}

NF_TEST(editor_flow_field_component_crud) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_flow_crud");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_flow_field(e, err));
    NF_CHECK(world.has<runtime::FlowFieldComponent>(e));

    // Reject 0 dimension
    runtime::FlowFieldComponent bad;
    bad.grid_width = 0;
    NF_CHECK(!app.set_flow_field(e, bad, err));

    runtime::FlowFieldComponent good;
    good.grid_width = 64;
    good.grid_height = 64;
    good.cell_size = 0.5f;
    good.target_x = 32.0f;
    good.target_y = 32.0f;
    good.enabled = true;
    NF_CHECK(app.set_flow_field(e, good, err));

    const auto* read = world.get<runtime::FlowFieldComponent>(e);
    NF_CHECK(read != nullptr);
    NF_CHECK(read->grid_width == 64);
    NF_CHECK(read->cell_size == 0.5f);
    NF_CHECK(read->target_x == 32.0f);

    NF_CHECK(app.detach_flow_field(e, err));
    NF_CHECK(!world.has<runtime::FlowFieldComponent>(e));
}

NF_TEST(editor_talent_tree_component_crud) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_talent_crud");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_talent_tree(e, err));
    NF_CHECK(world.has<runtime::TalentComponent>(e));

    runtime::TalentComponent tt;
    tt.tree_id = "mage_spells";
    tt.tree_name = "Arcane Specialization";
    tt.available_points = 8;
    tt.spent_points = 12;
    tt.unlocked_count = 5;
    tt.enabled = true;
    NF_CHECK(app.set_talent_tree(e, tt, err));

    const auto* read = world.get<runtime::TalentComponent>(e);
    NF_CHECK(read != nullptr);
    NF_CHECK(read->tree_id == "mage_spells");
    NF_CHECK(read->available_points == 8);
    NF_CHECK(read->spent_points == 12);

    NF_CHECK(app.detach_talent_tree(e, err));
    NF_CHECK(!world.has<runtime::TalentComponent>(e));
}

NF_TEST(editor_ragdoll_component_crud) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_ragdoll_crud");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_ragdoll(e, err));
    NF_CHECK(world.has<runtime::RagdollComponent>(e));

    runtime::RagdollComponent rd;
    rd.blend_weight = 0.7f;
    rd.pose_state = 2; // Get-Up Face Up
    rd.blend_duration = 0.8f;
    rd.preserve_momentum = true;
    rd.enabled = true;
    NF_CHECK(app.set_ragdoll(e, rd, err));

    const auto* read = world.get<runtime::RagdollComponent>(e);
    NF_CHECK(read != nullptr);
    NF_CHECK(read->blend_weight == 0.7f);
    NF_CHECK(read->pose_state == 2);
    NF_CHECK(read->preserve_momentum);

    NF_CHECK(app.detach_ragdoll(e, err));
    NF_CHECK(!world.has<runtime::RagdollComponent>(e));
}

NF_TEST(editor_inventory_component_crud) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_inv_crud");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_inventory(e, err));
    NF_CHECK(world.has<runtime::InventoryComponent>(e));

    runtime::InventoryComponent bad;
    bad.columns = 0;
    NF_CHECK(!app.set_inventory(e, bad, err));

    runtime::InventoryComponent good;
    good.columns = 12;
    good.rows = 6;
    good.max_weight = 75.0f;
    good.current_weight = 22.5f;
    good.item_count = 8;
    good.auto_pack = true;
    good.enabled = true;
    NF_CHECK(app.set_inventory(e, good, err));

    const auto* read = world.get<runtime::InventoryComponent>(e);
    NF_CHECK(read != nullptr);
    NF_CHECK(read->columns == 12);
    NF_CHECK(read->max_weight == 75.0f);
    NF_CHECK(read->current_weight == 22.5f);

    NF_CHECK(app.detach_inventory(e, err));
    NF_CHECK(!world.has<runtime::InventoryComponent>(e));
}

NF_TEST(editor_surface_audio_component_crud) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_sa_crud");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity e = world.all_entities().front();

    NF_CHECK(app.attach_surface_audio(e, err));
    NF_CHECK(world.has<runtime::SurfaceAudioComponent>(e));

    runtime::SurfaceAudioComponent sa;
    sa.surface_material = 4; // Water
    sa.volume_scale = 1.35f;
    sa.pitch_variation = 0.12f;
    sa.wetness_modulation = true;
    sa.enabled = true;
    NF_CHECK(app.set_surface_audio(e, sa, err));

    const auto* read = world.get<runtime::SurfaceAudioComponent>(e);
    NF_CHECK(read != nullptr);
    NF_CHECK(read->surface_material == 4);
    NF_CHECK(read->volume_scale == 1.35f);
    NF_CHECK(read->pitch_variation == 0.12f);

    NF_CHECK(app.detach_surface_audio(e, err));
    NF_CHECK(!world.has<runtime::SurfaceAudioComponent>(e));
}

NF_TEST(editor_new_components_duplication) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    const auto tmp = feature_temp_dir("nf_ed_features_duplication");
    VirtualFileSystem vfs;
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");
    write_test_scene(vfs, "content://Scenes/Work.nfscene");

    AssetRegistry reg;
    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    editor::ConsoleBuffer console;
    editor::EditorApp app(vfs, reg, manager, console);
    app.attach_runtime(&runtime);

    std::string err;
    NF_CHECK(app.open_scene("content://Scenes/Work.nfscene", err));
    ecs::World& world = runtime.edit_scene()->world();
    const ecs::Entity src = world.all_entities().front();

    // Attach components to src
    NF_CHECK(app.attach_cable(src, err));
    NF_CHECK(app.attach_weather(src, err));
    NF_CHECK(app.attach_rule_tile(src, err));
    NF_CHECK(app.attach_flow_field(src, err));
    NF_CHECK(app.attach_talent_tree(src, err));
    NF_CHECK(app.attach_ragdoll(src, err));
    NF_CHECK(app.attach_inventory(src, err));
    NF_CHECK(app.attach_surface_audio(src, err));

    // Select src and duplicate
    app.selection().set_single(src);
    NF_CHECK(app.duplicate_selection(err));
    const ecs::Entity dst = app.selection().primary();
    NF_CHECK(dst.valid() && dst != src);

    // Verify all 8 components replicated onto cloned entity
    NF_CHECK(world.has<runtime::CableComponent>(dst));
    NF_CHECK(world.has<runtime::WeatherComponent>(dst));
    NF_CHECK(world.has<runtime::RuleTileComponent>(dst));
    NF_CHECK(world.has<runtime::FlowFieldComponent>(dst));
    NF_CHECK(world.has<runtime::TalentComponent>(dst));
    NF_CHECK(world.has<runtime::RagdollComponent>(dst));
    NF_CHECK(world.has<runtime::InventoryComponent>(dst));
    NF_CHECK(world.has<runtime::SurfaceAudioComponent>(dst));
}
