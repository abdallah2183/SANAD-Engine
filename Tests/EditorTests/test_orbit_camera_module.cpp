// Tests/EditorTests/test_orbit_camera_module.cpp — Phase 10, W6 (sample module)
//
// OrbitCameraModule is the worked example of the gameplay API, so it is held to
// the same standard as engine code rather than being allowed to be "just a
// sample". It is driven directly with a hand-built GameplayContext: the module
// only needs a world, an input source and a delta, so a device would be noise.

#include <NF/Test/TestFramework.hpp>

#include <NF/Editor/OrbitCameraModule.hpp>

#include <NF/Gameplay/GameplayState.hpp>
#include <unordered_map>

#include <NF/ECS/ECS.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <string>

using namespace nf;
using namespace nf::editor;

namespace {

/// Input source with fixed axes, so a test states what the player is holding
/// rather than simulating a device.
class StubInput final : public gameplay::IInputSource {
public:
    f32 look = 0.0f;
    f32 zoom = 0.0f;

    [[nodiscard]] bool action_pressed(std::string_view action) const override {
        if (action == "look") return look != 0.0f;
        if (action == "zoom") return zoom != 0.0f;
        return false;
    }
    [[nodiscard]] f32 action_axis(std::string_view action) const override {
        if (action == "look") return look;
        if (action == "zoom") return zoom;
        return 0.0f;
    }
};

struct OrbitFixture {
    scene::Scene scene{"OrbitWork"};
    ecs::Entity camera{};
    ecs::Entity pivot{};
    StubInput input;

    OrbitFixture() {
        camera = scene.world().create_entity();
        scene.world().add<scene::Transform>(camera, scene::Transform{});
        scene.world().add<runtime::CameraComponent>(camera, runtime::CameraComponent{});

        pivot = scene.world().create_entity();
        scene::Transform t;
        t.local_x = 10.0f;
        t.world_x = 10.0f;
        scene.world().add<scene::Transform>(pivot, t);
    }

    gameplay::GameplayContext context(f32 dt) {
        gameplay::GameplayContext ctx{};
        ctx.world = &scene.world();
        ctx.scene = &scene;
        ctx.input = &input;
        ctx.dt = dt;
        // The module drives only while playing (GameplayContext::playing):
        // every placement test below asserts driven behaviour, so the
        // fixture opts in. The not-playing case has its own test.
        ctx.playing = true;
        return ctx;
    }

    /// A temporary cannot bind to on_update's GameplayContext    const scene::Transform* camera_transform() const {, so the fixture
    /// owns the context for the call.
    void step(OrbitCameraModule& module, f32 dt) {
        gameplay::GameplayContext ctx = context(dt);
        module.on_update(ctx);
    }

    const scene::Transform* camera_transform() const {
        return scene.world().get<scene::Transform>(camera);
    }
};

} // namespace

NF_TEST(orbit_camera_places_the_camera_on_the_orbit_radius) {
    OrbitFixture f;
    OrbitCameraModule module;
    module.settings.radius = 5.0f;
    module.settings.yaw_degrees = 0.0f;
    module.settings.pitch_degrees = 0.0f;
    module.settings.target_entity = f.pivot.id;
    module.settings.use_input = false;

    gameplay::GameplayContext ctx = f.context(1.0f / 60.0f);
    module.on_update(ctx);

    NF_CHECK_EQ(module.placements(), 1u);
    NF_CHECK_EQ(module.last_target(), f.pivot.id);

    const scene::Transform* t = f.camera_transform();
    NF_CHECK(t != nullptr);
    if (t != nullptr) {
        // yaw 0, pitch 0: straight out along +Z from the pivot at x = 10.
        NF_CHECK_NEAR(t->local_x, 10.0f, 1e-3f);
        NF_CHECK_NEAR(t->local_y, 0.0f, 1e-3f);
        NF_CHECK_NEAR(t->local_z, 5.0f, 1e-3f);
    }

    // The radius is the distance from the pivot, not from the origin — the
    // distinction that catches a module that forgot to add the pivot back.
    const Vec3 p = module.last_camera_position();
    const f32 distance = std::sqrt((p.x - 10.0f) * (p.x - 10.0f) + p.y * p.y + p.z * p.z);
    NF_CHECK_NEAR(distance, 5.0f, 1e-3f);
}

NF_TEST(orbit_camera_moves_with_yaw_and_pitch) {
    OrbitFixture f;
    OrbitCameraModule module;
    module.settings.radius = 4.0f;
    module.settings.target_entity = f.pivot.id;
    module.settings.use_input = false;

    // yaw 90, pitch 0: the orbit point swings onto +X.
    module.settings.yaw_degrees = 90.0f;
    module.settings.pitch_degrees = 0.0f;
    f.step(module, 1.0f / 60.0f);
    NF_CHECK_NEAR(module.last_camera_position().x, 14.0f, 1e-3f);
    NF_CHECK_NEAR(module.last_camera_position().z, 0.0f, 1e-3f);

    // pitch 90 would be the pole; 30 degrees lifts the camera. The pivot sits at
    // (10, 0, 0), so the horizontal offset lands on z, not on x.
    module.settings.yaw_degrees = 0.0f;
    module.settings.pitch_degrees = 30.0f;
    f.step(module, 1.0f / 60.0f);
    NF_CHECK_NEAR(module.last_camera_position().x, 10.0f, 1e-3f);
    NF_CHECK_NEAR(module.last_camera_position().y, 2.0f, 1e-3f);   // 4 * sin(30)
    NF_CHECK_NEAR(module.last_camera_position().z, 4.0f * std::cos(30.0f * DEG_TO_RAD), 1e-3f);
}

NF_TEST(orbit_camera_aims_back_at_the_pivot) {
    OrbitFixture f;
    OrbitCameraModule module;
    module.settings.radius = 3.0f;
    module.settings.yaw_degrees = 45.0f;
    module.settings.pitch_degrees = 25.0f;
    module.settings.target_entity = f.pivot.id;
    module.settings.use_input = false;

    f.step(module, 1.0f / 60.0f);

    const scene::Transform* t = f.camera_transform();
    NF_CHECK(t != nullptr);
    if (t == nullptr) return;

    // Rotating the camera's forward by its euler angles must point from the
    // camera at the pivot. This is the assertion that catches a sign slip in the
    // yaw+180 aim, which a position-only test would not see.
    const Quat orientation =
        scene::quat_from_euler_xyz_degrees(t->rot_x, t->rot_y, t->rot_z);
    const Vec3 forward = orientation.rotate(Vec3::forward);

    const Vec3 position = module.last_camera_position();
    const Vec3 to_pivot = (Vec3{10.0f, 0.0f, 0.0f} - position).normalized();

    NF_CHECK_NEAR(forward.x, to_pivot.x, 1e-2f);
    NF_CHECK_NEAR(forward.y, to_pivot.y, 1e-2f);
    NF_CHECK_NEAR(forward.z, to_pivot.z, 1e-2f);
}

NF_TEST(orbit_camera_applies_input_and_clamps_it) {
    OrbitFixture f;
    OrbitCameraModule module;
    module.settings.radius = 5.0f;
    module.settings.yaw_degrees = 0.0f;
    module.settings.yaw_speed = 90.0f;   // degrees per second
    module.settings.zoom_speed = 2.0f;   // metres per second
    module.settings.min_radius = 4.0f;
    module.settings.max_radius = 6.0f;
    module.settings.target_entity = f.pivot.id;

    f.input.look = 1.0f;
    f.input.zoom = 1.0f;

    // Half a second at 90 deg/s is 45 degrees; 2 m/s of zoom-in pulls the radius
    // from 5 to 4, which is the minimum and must clamp there.
    f.step(module, 0.5f);

    NF_CHECK_NEAR(module.settings.yaw_degrees, 45.0f, 1e-3f);
    NF_CHECK_NEAR(module.settings.radius, 4.0f, 1e-3f);

    // A full second more of zoom-in cannot go below min_radius.
    f.step(module, 1.0f);
    NF_CHECK_NEAR(module.settings.radius, 4.0f, 1e-3f);

    // Zooming out is clamped at max_radius.
    f.input.zoom = -1.0f;
    f.step(module, 5.0f);
    NF_CHECK_NEAR(module.settings.radius, 6.0f, 1e-3f);

    // Pitch is clamped just short of the pole, where the aim would be undefined.
    module.settings.pitch_degrees = 400.0f;
    f.input.zoom = 0.0f;
    f.step(module, 1.0f / 60.0f);
    NF_CHECK(module.settings.pitch_degrees <= 89.0f);
    NF_CHECK(module.settings.pitch_degrees >= -89.0f);
}

NF_TEST(orbit_camera_ignores_input_when_disabled_or_absent) {
    OrbitFixture f;
    OrbitCameraModule module;
    module.settings.yaw_degrees = 0.0f;
    module.settings.target_entity = f.pivot.id;
    module.settings.use_input = false;
    f.input.look = 1.0f;

    f.step(module, 1.0f);
    NF_CHECK_NEAR(module.settings.yaw_degrees, 0.0f, 1e-5f);

    // And with input enabled but no source at all — the headless case.
    module.settings.use_input = true;
    gameplay::GameplayContext ctx = f.context(1.0f);
    ctx.input = nullptr;
    module.on_update(ctx);
    NF_CHECK_NEAR(module.settings.yaw_degrees, 0.0f, 1e-5f);

    // A source with no input still leaves the camera placed.
    NF_CHECK_EQ(module.placements(), 2u);
}

NF_TEST(orbit_camera_stays_out_of_the_editor_camera_while_not_playing) {
    // The reported defect: while editing (playing == false) the module must
    // not touch the camera Transform at all. It used to place every frame,
    // snapping each viewport drag straight back — which reads as a viewport
    // that refuses to move. Default context, no component attached.
    OrbitFixture f;
    OrbitCameraModule module;
    module.settings.radius = 5.0f;
    module.settings.yaw_degrees = 0.0f;
    module.settings.pitch_degrees = 0.0f;

    gameplay::GameplayContext ctx{};
    ctx.world = &f.scene.world();
    ctx.scene = &f.scene;
    ctx.dt = 1.0f;
    ctx.playing = false; // editing
    NF_CHECK(!ctx.playing);

    module.on_update(ctx);

    NF_CHECK_EQ(module.placements(), 0u);
    const scene::Transform* t = f.camera_transform();
    NF_CHECK(t != nullptr);
    if (t != nullptr) {
        // Untouched: the fixture builds the camera transform at the origin.
        NF_CHECK_NEAR(t->local_x, 0.0f, 1e-5f);
        NF_CHECK_NEAR(t->local_y, 0.0f, 1e-5f);
        NF_CHECK_NEAR(t->local_z, 0.0f, 1e-5f);
    }
}

NF_TEST(orbit_camera_does_nothing_without_a_camera_entity) {
    scene::Scene bare("Bare");
    bare.world().create_entity();  // no CameraComponent

    OrbitCameraModule module;
    gameplay::GameplayContext ctx{};
    ctx.world = &bare.world();
    ctx.dt = 1.0f / 60.0f;

    module.on_update(ctx);

    // The observable that separates "ran" from "ran and found nothing to move".
    NF_CHECK_EQ(module.placements(), 0u);
}

NF_TEST(orbit_camera_falls_back_to_a_non_camera_entity_as_pivot) {
    OrbitFixture f;
    OrbitCameraModule module;
    module.settings.radius = 2.0f;
    module.settings.yaw_degrees = 0.0f;
    module.settings.pitch_degrees = 0.0f;
    module.settings.use_input = false;
    // target_entity stays u32_max.
    NF_CHECK_EQ(module.settings.target_entity, u32_max);

    f.step(module, 1.0f / 60.0f);

    NF_CHECK_EQ(module.placements(), 1u);
    // The pivot defaults to the first entity that is not the camera — never the
    // camera itself, which would feed its own output back in and drift.
    NF_CHECK_EQ(module.last_target(), f.pivot.id);
    NF_CHECK_NEAR(module.last_camera_position().z, 2.0f, 1e-3f);
}

NF_TEST(orbit_camera_is_registered_and_offers_its_settings) {
    // Registration happened at static-init, with no engine code naming this type.
    NF_CHECK(gameplay::GameplayModuleRegistry::instance().contains("OrbitCamera"));

    auto instance = gameplay::GameplayModuleRegistry::instance().create("OrbitCamera");
    NF_CHECK(instance != nullptr);
    if (instance != nullptr) {
        NF_CHECK_EQ(std::string(instance->name()), std::string("OrbitCamera"));

        const gameplay::GameplayStateBinding binding = instance->state();
        NF_CHECK(binding.valid());
        if (binding.valid()) {
            NF_CHECK_EQ(std::string(binding.meta->name), std::string("OrbitCameraSettings"));

            // Every setting must be serializable, or a save would silently drop
            // the camera's configured position.
            std::unordered_map<std::string, std::string> properties;
            NF_CHECK(gameplay::capture_state(binding, properties));
            NF_CHECK_EQ(properties.size(), binding.meta->property_count);
        }
    }
}
