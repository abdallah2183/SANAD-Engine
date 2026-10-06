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

NF_TEST(editor_inspector_light_cascade_tuning) {
    scene::Scene scene("LightCascades");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});
    scene.world().add<runtime::DirectionalLight>(e, runtime::DirectionalLight{});

    editor::LightEdit edit;
    edit.shadow_cascades = 2;
    edit.shadow_distance = 60.0f;
    std::string err;
    auto cmd = editor::make_light_command(scene.world(), e, edit, err);
    NF_CHECK(cmd != nullptr);
    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* l = scene.world().get<runtime::DirectionalLight>(e);
    NF_CHECK(l != nullptr);
    NF_CHECK_EQ(l->shadow_cascades, 2u);
    NF_CHECK_NEAR(l->shadow_distance, 60.0f, 1e-5f);

    // The panel's edit flow is read -> display -> write back, so the read path
    // must hand back exactly what was stored and the result must survive
    // validation unchanged. A read that loses or rounds a value makes the panel
    // silently rewrite the artist's setting every time the selection changes.
    bool has = false;
    const editor::LightEdit read_back = editor::read_light(scene.world(), e, has);
    NF_CHECK(has);
    NF_CHECK_EQ(read_back.shadow_cascades, 2);
    NF_CHECK_NEAR(read_back.shadow_distance, 60.0f, 1e-5f);
    std::string round_trip_err;
    NF_CHECK(editor::make_light_command(scene.world(), e, read_back, round_trip_err) != nullptr);

    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK_EQ(scene.world().get<runtime::DirectionalLight>(e)->shadow_cascades, 4u);
    NF_CHECK_NEAR(scene.world().get<runtime::DirectionalLight>(e)->shadow_distance, 0.0f, 1e-7f);

    // The count indexes a fixed 2x2 atlas, so it is bounded at both ends: 0 tiles
    // means no shadow map at all and 5 has nowhere to go. Rejecting here is the
    // point — the renderer would otherwise silently clamp and the artist would
    // see a number in the panel that is not the number being rendered.
    editor::LightEdit too_many = edit;
    too_many.shadow_cascades = 5;
    NF_CHECK(editor::make_light_command(scene.world(), e, too_many, err) == nullptr);
    NF_CHECK(!err.empty());
    editor::LightEdit none = edit;
    none.shadow_cascades = 0;
    NF_CHECK(editor::make_light_command(scene.world(), e, none, err) == nullptr);

    // 0 is the documented "cast to the camera's far plane" sentinel, not a
    // distance, so only a negative one is invalid.
    editor::LightEdit negative = edit;
    negative.shadow_distance = -1.0f;
    NF_CHECK(editor::make_light_command(scene.world(), e, negative, err) == nullptr);
    editor::LightEdit far_plane = edit;
    far_plane.shadow_distance = 0.0f;
    NF_CHECK(editor::make_light_command(scene.world(), e, far_plane, err) != nullptr);
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

NF_TEST(inspector_time_of_day_command_adds_edits_and_undoes_cleanly) {
    // The editor path for the day/night cycle. The runtime already honours the
    // component (RuntimeTests), but that is the *game* path — without this, the
    // cycle is loadable from a scene file and invisible in the editor, so the only
    // way to author one is to hand-write a line into the .nfscene.
    scene::Scene scene("TodInspector");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});

    // Absent component: has=false, defaults still queryable.
    bool has = true;
    editor::TimeOfDayEdit def = editor::read_time_of_day(scene.world(), e, has);
    NF_CHECK(!has);
    NF_CHECK_NEAR(def.time_hours, 12.0f, 1e-6f);

    // Apply adds the component; undo removes it entirely — not leaves a clock
    // parked at some hour, which would round-trip into a TimeOfDay line the
    // author never asked for.
    editor::TimeOfDayEdit edit;
    edit.time_hours = 17.5f;
    edit.day_length_seconds = 120.0f;
    edit.drive_light = false;
    std::string err;
    auto cmd = editor::make_time_of_day_command(scene.world(), e, edit, err);
    NF_CHECK(cmd != nullptr);

    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* t = scene.world().get<runtime::TimeOfDayComponent>(e);
    NF_CHECK(t != nullptr);
    NF_CHECK_NEAR(t->time_hours, 17.5f, 1e-6f);
    NF_CHECK_NEAR(t->day_length_seconds, 120.0f, 1e-6f);
    NF_CHECK(!t->drive_light);
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(!scene.world().has<runtime::TimeOfDayComponent>(e));

    // Re-apply, then read back.
    auto cmd2 = editor::make_time_of_day_command(scene.world(), e, edit, err);
    NF_CHECK(cmd2 != nullptr);
    stack.push(std::move(cmd2), scene.world());
    bool has2 = false;
    const editor::TimeOfDayEdit back = editor::read_time_of_day(scene.world(), e, has2);
    NF_CHECK(has2);
    NF_CHECK_NEAR(back.time_hours, 17.5f, 1e-6f);
    NF_CHECK_NEAR(back.day_length_seconds, 120.0f, 1e-6f);
    NF_CHECK(!back.drive_light);

    // Hours wrap rather than clamp, matching the scene loader: 25.0 is 1am, and a
    // clamp would silently turn "an hour later" into 24:00.
    editor::TimeOfDayEdit wrap = edit;
    wrap.time_hours = 25.0f;
    auto cmd3 = editor::make_time_of_day_command(scene.world(), e, wrap, err);
    NF_CHECK(cmd3 != nullptr);
    stack.push(std::move(cmd3), scene.world());
    const auto* w = scene.world().get<runtime::TimeOfDayComponent>(e);
    NF_CHECK(w != nullptr);
    NF_CHECK_NEAR(w->time_hours, 1.0f, 1e-5f);

    // A negative day length is "freeze the clock", not an error — same rule the
    // loader applies, so the two paths cannot disagree about the same value.
    editor::TimeOfDayEdit frozen = edit;
    frozen.day_length_seconds = -5.0f;
    auto cmd4 = editor::make_time_of_day_command(scene.world(), e, frozen, err);
    NF_CHECK(cmd4 != nullptr);
    stack.push(std::move(cmd4), scene.world());
    const auto* f = scene.world().get<runtime::TimeOfDayComponent>(e);
    NF_CHECK(f != nullptr);
    NF_CHECK_NEAR(f->day_length_seconds, 0.0f, 1e-6f);

    // A non-finite hour is refused: that is a broken widget, not an author choice.
    editor::TimeOfDayEdit bad = edit;
    bad.time_hours = std::numeric_limits<float>::quiet_NaN();
    NF_CHECK(editor::make_time_of_day_command(scene.world(), e, bad, err) == nullptr);
    NF_CHECK(!err.empty());
}

NF_TEST(inspector_post_process_command_adds_edits_and_undoes_cleanly) {
    // The editor path for the §206 post stack. RuntimeTests already covers the
    // game path; without this the stack is loadable from a scene file and
    // invisible in the editor, so authoring one means hand-writing a line.
    scene::Scene scene("PostInspector");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<scene::Transform>(e, scene::Transform{});

    // Absent component: has=false, defaults still queryable.
    bool has = true;
    runtime::PostProcessComponent def = editor::read_post_process(scene.world(), e, has);
    NF_CHECK(!has);
    NF_CHECK(!def.bloom_enabled);
    NF_CHECK_NEAR(def.bloom_threshold, 1.0f, 1e-6f);
    NF_CHECK_NEAR(def.saturation, 1.0f, 1e-6f);

    // Apply adds the component; undo removes it entirely — not leaves a neutral
    // block behind, which would round-trip into a PostProcess line the author
    // never asked for.
    runtime::PostProcessComponent edit;
    edit.bloom_enabled = true;
    edit.bloom_threshold = 2.5f;
    edit.bloom_intensity = 1.5f;
    edit.grade_enabled = true;
    edit.grade_temperature = -0.75f;
    edit.sharpen_enabled = true;
    edit.sharpen_amount = 0.5f;
    edit.vignette = 0.3f;
    edit.lens_enabled = true;
    edit.lens_distortion = -0.2f; // pincushion: the sign is part of the edit
    edit.lens_chromatic_aberration = 0.012f;
    edit.dof_enabled = true;
    edit.dof_focus_distance = 15.0f;
    edit.dof_focus_range = 3.0f;
    edit.dof_max_radius = 7.5f;
    edit.motion_enabled = true;
    edit.motion_intensity = 2.0f;
    edit.motion_max_length = 0.06f;
    edit.exposure = 1.75f;
    edit.tonemap = 2; // Reinhard
    edit.lut_path = "content://LUTs/grade.png";
    edit.lut_strength = 0.4f;
    std::string err;
    auto cmd = editor::make_post_process_command(scene.world(), e, edit, err);
    NF_CHECK(cmd != nullptr);

    editor::CommandStack stack;
    stack.push(std::move(cmd), scene.world());
    const auto* pp = scene.world().get<runtime::PostProcessComponent>(e);
    NF_CHECK(pp != nullptr);
    if (pp != nullptr) {
        NF_CHECK(pp->bloom_enabled);
        NF_CHECK_NEAR(pp->bloom_threshold, 2.5f, 1e-6f);
        NF_CHECK_NEAR(pp->bloom_intensity, 1.5f, 1e-6f);
        NF_CHECK(pp->grade_enabled);
        NF_CHECK_NEAR(pp->grade_temperature, -0.75f, 1e-6f);
        NF_CHECK(pp->sharpen_enabled);
        NF_CHECK_NEAR(pp->sharpen_amount, 0.5f, 1e-6f);
        NF_CHECK_NEAR(pp->vignette, 0.3f, 1e-6f);
        NF_CHECK(pp->lens_enabled);
        NF_CHECK_NEAR(pp->lens_distortion, -0.2f, 1e-6f);
        NF_CHECK_NEAR(pp->lens_chromatic_aberration, 0.012f, 1e-6f);
        NF_CHECK(pp->dof_enabled);
        NF_CHECK_NEAR(pp->dof_focus_distance, 15.0f, 1e-6f);
        NF_CHECK_NEAR(pp->dof_focus_range, 3.0f, 1e-6f);
        NF_CHECK_NEAR(pp->dof_max_radius, 7.5f, 1e-6f);
        NF_CHECK(pp->motion_enabled);
        NF_CHECK_NEAR(pp->motion_intensity, 2.0f, 1e-6f);
        NF_CHECK_NEAR(pp->motion_max_length, 0.06f, 1e-6f);
        NF_CHECK_NEAR(pp->exposure, 1.75f, 1e-6f);
        NF_CHECK(pp->tonemap == 2);
        NF_CHECK(pp->lut_path == "content://LUTs/grade.png");
        NF_CHECK_NEAR(pp->lut_strength, 0.4f, 1e-6f);
    }
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(!scene.world().has<runtime::PostProcessComponent>(e));

    // Re-apply, then read back through the inspector's own reader.
    auto cmd2 = editor::make_post_process_command(scene.world(), e, edit, err);
    NF_CHECK(cmd2 != nullptr);
    stack.push(std::move(cmd2), scene.world());
    bool has2 = false;
    const runtime::PostProcessComponent back = editor::read_post_process(scene.world(), e, has2);
    NF_CHECK(has2);
    NF_CHECK(back.bloom_enabled);
    NF_CHECK_NEAR(back.bloom_threshold, 2.5f, 1e-6f);
    NF_CHECK_NEAR(back.grade_temperature, -0.75f, 1e-6f);

    // Redo restores the same block.
    NF_CHECK(stack.undo(scene.world()));
    NF_CHECK(stack.redo(scene.world()));
    NF_CHECK(scene.world().has<runtime::PostProcessComponent>(e));

    // Out-of-range values are refused, and refused for the same reason the
    // scene loader refuses them — a value the editor accepted and the loader
    // then dropped would make the same scene load differently depending on how
    // it was authored.
    runtime::PostProcessComponent zero_radius = edit;
    zero_radius.bloom_radius = 0.0f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, zero_radius, err) == nullptr);
    NF_CHECK(!err.empty());

    runtime::PostProcessComponent negative = edit;
    negative.bloom_intensity = -1.0f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, negative, err) == nullptr);

    runtime::PostProcessComponent nan = edit;
    nan.grade_contrast = std::numeric_limits<float>::quiet_NaN();
    NF_CHECK(editor::make_post_process_command(scene.world(), e, nan, err) == nullptr);
    NF_CHECK(!err.empty());

    // Lens bounds match the scene loader's: past +/-0.5 the warp folds the
    // corners over themselves, and a negative chromatic aberration would swap
    // which end of the spectrum fringes outward.
    runtime::PostProcessComponent folded = edit;
    folded.lens_distortion = 0.9f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, folded, err) == nullptr);
    NF_CHECK(!err.empty());

    runtime::PostProcessComponent negative_chroma = edit;
    negative_chroma.lens_chromatic_aberration = -0.01f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, negative_chroma, err) == nullptr);

    // Depth of field: the focus RANGE has a real floor because the ramp divides
    // by it, and a negative radius is not a blur in the other direction.
    runtime::PostProcessComponent zero_range = edit;
    zero_range.dof_focus_range = 0.0f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, zero_range, err) == nullptr);
    NF_CHECK(!err.empty());

    runtime::PostProcessComponent negative_radius = edit;
    negative_radius.dof_max_radius = -3.0f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, negative_radius, err) == nullptr);

    runtime::PostProcessComponent negative_focus = edit;
    negative_focus.dof_focus_distance = -1.0f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, negative_focus, err) == nullptr);

    // Motion blur: a negative intensity is not "blur the other way", and a
    // zero cap would mean the same thing a zero intensity already says.
    runtime::PostProcessComponent negative_intensity = edit;
    negative_intensity.motion_intensity = -1.0f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, negative_intensity, err) == nullptr);

    runtime::PostProcessComponent zero_cap = edit;
    zero_cap.motion_max_length = 0.0f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, zero_cap, err) == nullptr);

    // The tonemap operator is a closed set: -1 (not authored) and 0..3 are the
    // only values, so anything else is refused rather than mapped to one.
    runtime::PostProcessComponent bad_tonemap = edit;
    bad_tonemap.tonemap = 4;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, bad_tonemap, err) == nullptr);
    NF_CHECK(!err.empty());

    runtime::PostProcessComponent negative_exposure = edit;
    negative_exposure.exposure = -1.0f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, negative_exposure, err) == nullptr);

    // The LUT strength is a blend weight, so it is bounded by definition. The
    // path is deliberately NOT validated here: the runtime resolves it through
    // the VFS and warns, which is the only place that knows whether it exists.
    runtime::PostProcessComponent lut_over = edit;
    lut_over.lut_strength = 1.5f;
    NF_CHECK(editor::make_post_process_command(scene.world(), e, lut_over, err) == nullptr);

    runtime::PostProcessComponent lut_missing_path = edit;
    lut_missing_path.lut_path = "content://LUTs/nope.png";
    NF_CHECK(editor::make_post_process_command(scene.world(), e, lut_missing_path, err) != nullptr);

    // A dead entity is refused rather than silently editing nothing.
    ecs::Entity dead{};
    NF_CHECK(editor::make_post_process_command(scene.world(), dead, edit, err) == nullptr);
}
