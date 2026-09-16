// Editor inspector: transform/camera/light/mesh edits apply, propagate, and
// reject invalid input without mutating the scene.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Editor/Inspector.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <limits>

using namespace nf;

NF_TEST(editor_inspector_transform_edit) {
    scene::Scene scene("TransformEdit");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<scene::NameComponent>(e, scene::NameComponent{"Box"});

    editor::TransformEdit edit;
    edit.px = 1.0f;
    edit.py = 2.0f;
    edit.pz = 3.0f;
    edit.rx = 10.0f;
    edit.rz = 45.0f;
    edit.sx = 2.0f;
    edit.sy = 2.0f;
    edit.sz = 2.0f;
    std::string err;
    auto cmd = editor::make_transform_command(scene.world(), e, edit, err);
    NF_CHECK(cmd != nullptr);
    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    scene::propagate_transforms(scene.world());

    const auto* t = scene.world().get<scene::Transform>(e);
    NF_CHECK(t != nullptr);
    NF_CHECK_NEAR(t->local_x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(t->world_x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(t->rot_z, 45.0f, 1e-6f);
    NF_CHECK_NEAR(t->scale_x, 2.0f, 1e-6f);
    NF_CHECK(stack.undo(scene.world()));
    const auto* t0 = scene.world().get<scene::Transform>(e);
    NF_CHECK(t0 != nullptr);
    NF_CHECK_NEAR(t0->local_x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(t0->scale_x, 1.0f, 1e-6f);
}

NF_TEST(editor_inspector_transform_invalid) {
    scene::Scene scene("TransformInvalid");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    std::string err;

    editor::TransformEdit bad;
    bad.sx = 0.0f; // zero scale is rejected
    NF_CHECK(editor::make_transform_command(scene.world(), e, bad, err) == nullptr);
    NF_CHECK(!err.empty());

    editor::TransformEdit nan;
    nan.px = std::numeric_limits<float>::quiet_NaN();
    NF_CHECK(editor::make_transform_command(scene.world(), e, nan, err) == nullptr);

    // Scene untouched.
    const auto* t = scene.world().get<scene::Transform>(e);
    NF_CHECK(t != nullptr);
    NF_CHECK_NEAR(t->local_x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(t->scale_x, 1.0f, 1e-6f);
}

NF_TEST(editor_inspector_camera_edit) {
    scene::Scene scene("CameraEdit");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    runtime::CameraComponent cam;
    cam.is_active = true;
    scene.world().add<runtime::CameraComponent>(e, cam);

    editor::CameraEdit edit;
    edit.active = true;
    edit.fov_y = 45.0f;
    edit.near_plane = 0.5f;
    edit.far_plane = 500.0f;
    std::string err;
    auto cmd = editor::make_camera_command(scene.world(), e, edit, err);
    NF_CHECK(cmd != nullptr);
    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* c = scene.world().get<runtime::CameraComponent>(e);
    NF_CHECK(c != nullptr);
    NF_CHECK_NEAR(c->fov_y, 45.0f, 1e-6f);
    NF_CHECK(stack.undo(scene.world()));
    const auto* c0 = scene.world().get<runtime::CameraComponent>(e);
    NF_CHECK(c0 != nullptr);
    NF_CHECK_NEAR(c0->fov_y, 60.0f, 1e-6f);

    editor::CameraEdit bad_fov = edit;
    bad_fov.fov_y = 0.0f;
    NF_CHECK(editor::make_camera_command(scene.world(), e, bad_fov, err) == nullptr);
    editor::CameraEdit bad_range = edit;
    bad_range.far_plane = 0.1f;
    NF_CHECK(editor::make_camera_command(scene.world(), e, bad_range, err) == nullptr);
}

NF_TEST(editor_inspector_light_edit) {
    scene::Scene scene("LightEdit");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<runtime::DirectionalLight>(e, runtime::DirectionalLight{});

    editor::LightEdit edit;
    edit.dir_x = 0.0f;
    edit.dir_y = -1.0f;
    edit.dir_z = 0.0f;
    edit.intensity = 2.5f;
    std::string err;
    auto cmd = editor::make_light_command(scene.world(), e, edit, err);
    NF_CHECK(cmd != nullptr);
    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* l = scene.world().get<runtime::DirectionalLight>(e);
    NF_CHECK(l != nullptr);
    NF_CHECK_NEAR(l->intensity, 2.5f, 1e-6f);
    NF_CHECK(l->cast_shadows); // default on

    // Shadow toggle flows through the same command and undoes cleanly.
    editor::LightEdit no_shadow = edit;
    no_shadow.cast_shadows = false;
    auto shadow_cmd = editor::make_light_command(scene.world(), e, no_shadow, err);
    NF_CHECK(shadow_cmd != nullptr);
    stack.push(std::move(shadow_cmd), scene.world());
    NF_CHECK(!scene.world().get<runtime::DirectionalLight>(e)->cast_shadows);
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(scene.world().get<runtime::DirectionalLight>(e)->cast_shadows);

    editor::LightEdit zero_dir = edit;
    zero_dir.dir_x = 0.0f;
    zero_dir.dir_y = 0.0f;
    zero_dir.dir_z = 0.0f;
    NF_CHECK(editor::make_light_command(scene.world(), e, zero_dir, err) == nullptr);
    editor::LightEdit bad_color = edit;
    bad_color.color_r = 4.0f;
    NF_CHECK(editor::make_light_command(scene.world(), e, bad_color, err) == nullptr);
}

NF_TEST(editor_inspector_mesh_edit) {
    scene::Scene scene("MeshEdit");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    std::string err;
    const std::string id = "f04e488b-4dc8-414b-baee-e3c50e8829ad";
    auto cmd = editor::make_mesh_command(scene.world(), e, id, "content://Materials/Default", err);
    NF_CHECK(cmd != nullptr);
    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* m = scene.world().get<runtime::MeshComponent>(e);
    NF_CHECK(m != nullptr);
    NF_CHECK(m->mesh_id.to_string() == id);
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(!scene.world().has<runtime::MeshComponent>(e));

    NF_CHECK(editor::make_mesh_command(scene.world(), e, "not-a-uuid", "x", err) == nullptr);
    NF_CHECK(!err.empty());
}

NF_TEST(editor_inspector_light_shadow_tuning) {
    scene::Scene scene("LightShadow");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<runtime::DirectionalLight>(e, runtime::DirectionalLight{});

    editor::LightEdit edit;
    edit.shadow_strength = 0.35f;
    edit.shadow_bias = 0.002f;
    std::string err;
    auto cmd = editor::make_light_command(scene.world(), e, edit, err);
    NF_CHECK(cmd != nullptr);
    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* l = scene.world().get<runtime::DirectionalLight>(e);
    NF_CHECK(l != nullptr);
    NF_CHECK_NEAR(l->shadow_strength, 0.35f, 1e-6f);
    NF_CHECK_NEAR(l->shadow_bias, 0.002f, 1e-7f);
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK_NEAR(scene.world().get<runtime::DirectionalLight>(e)->shadow_strength, 1.0f, 1e-6f);

    editor::LightEdit bad_strength = edit;
    bad_strength.shadow_strength = 1.5f;
    NF_CHECK(editor::make_light_command(scene.world(), e, bad_strength, err) == nullptr);
    editor::LightEdit bad_bias = edit;
    bad_bias.shadow_bias = -1.0f;
    NF_CHECK(editor::make_light_command(scene.world(), e, bad_bias, err) == nullptr);
}

NF_TEST(editor_inspector_sky_edit_add_and_undo) {
    scene::Scene scene("SkyEdit");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});

    // Read path: absent component reports has=false with defaults.
    bool has = true;
    editor::SkyEdit def = editor::read_sky(scene.world(), e, has);
    NF_CHECK(!has);
    NF_CHECK_NEAR(def.zenith[1], 0.42f, 1e-6f);

    // Apply path: adds the component, undoes by removing it.
    editor::SkyEdit edit;
    edit.zenith[0] = 0.05f;
    edit.sun_disk = 3.0f;
    edit.enabled = false;
    std::string err;
    auto cmd = editor::make_sky_command(scene.world(), e, edit, err);
    NF_CHECK(cmd != nullptr);
    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* s = scene.world().get<runtime::SkyComponent>(e);
    NF_CHECK(s != nullptr);
    NF_CHECK_NEAR(s->zenith_r, 0.05f, 1e-6f);
    NF_CHECK_NEAR(s->sun_disk, 3.0f, 1e-6f);
    NF_CHECK(!s->enabled);
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(!scene.world().has<runtime::SkyComponent>(e));

    // Re-add, then read back through read_sky.
    auto cmd2 = editor::make_sky_command(scene.world(), e, edit, err);
    NF_CHECK(cmd2 != nullptr);
    stack.push(std::move(cmd2), scene.world());
    bool has2 = false;
    editor::SkyEdit back = editor::read_sky(scene.world(), e, has2);
    NF_CHECK(has2);
    NF_CHECK_NEAR(back.sun_disk, 3.0f, 1e-6f);

    // Validation: out-of-range multiplier and NaN color rejected.
    editor::SkyEdit bad = edit;
    bad.sun_glow = 99.0f;
    NF_CHECK(editor::make_sky_command(scene.world(), e, bad, err) == nullptr);
    editor::SkyEdit bad2 = edit;
    bad2.horizon[0] = std::numeric_limits<float>::quiet_NaN();
    NF_CHECK(editor::make_sky_command(scene.world(), e, bad2, err) == nullptr);
}
