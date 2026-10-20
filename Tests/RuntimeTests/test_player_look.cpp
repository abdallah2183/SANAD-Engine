// Tests/RuntimeTests/test_player_look.cpp — aiming the camera.
//
// The 0.3.0 notes ended with one known gap: "the camera cannot be aimed yet.
// It follows a fixed orbit; a mouse look and a player-driven yaw come next."
// These cases pin the four decisions that close it — what a mouse pixel is
// worth, what a held stick is worth, how far the orbit may pitch, and which
// way "forward" is once the player can turn — and then prove the runtime
// actually applies them to a scene's camera.
//
// The maths cases need no device, no window and no filesystem, so they run
// everywhere. The runtime cases are the wiring: a camera in a scene has to
// END UP turned, or the maths is a library nobody calls.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/PlayerLook.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::scene;

namespace {

constexpr float kEps = 1e-4f;

/// A source that answers whatever the test tells it to, and nothing else.
///
/// It is a real IInputSource, so the runtime's own code path is exercised —
/// a test that called the maths directly would prove the maths and nothing
/// about whether the frame loop asks it anything.
class FakeInput final : public gameplay::IInputSource {
public:
    bool has_pointer = true;
    f32 dx = 0.0f;
    f32 dy = 0.0f;
    f32 wheel = 0.0f;
    f32 move_x = 0.0f;
    f32 move_z = 0.0f;
    f32 look_x = 0.0f;
    f32 look_y = 0.0f;
    bool captured = false;

    [[nodiscard]] bool action_pressed(std::string_view action) const override {
        (void)action;
        return false;
    }
    [[nodiscard]] f32 action_axis(std::string_view action) const override {
        if (action == "move_x") return move_x;
        if (action == "move_z") return move_z;
        if (action == "look_x") return look_x;
        if (action == "look_y") return look_y;
        return 0.0f;
    }
    [[nodiscard]] bool look_delta(f32& out_dx, f32& out_dy) const override {
        if (!has_pointer) {
            out_dx = 0.0f;
            out_dy = 0.0f;
            return false;
        }
        out_dx = dx;
        out_dy = dy;
        return true;
    }
    [[nodiscard]] f32 wheel_delta() const override { return wheel; }
    void set_pointer_capture(bool on) override { captured = on; }
};

/// A headless runtime with one player entity and one camera that follows it.
struct LookFixture {
    std::filesystem::path tmp;
    VirtualFileSystem vfs;
    AssetRegistry registry;
    AssetManager manager;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<Runtime> runtime;
    FakeInput input;

    LookFixture() : manager(vfs, registry) {
        tmp = nf::test::temp_root() / "nf_player_look";
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp);
        vfs.mount("content://", tmp);

        device = rhi::create_device();
        if (!device) {
            return;
        }
        rhi::DeviceDesc desc{};
        desc.window_handle = nullptr;
        desc.enable_validation = false;
        if (!device->init(desc)) {
            device.reset();
            return;
        }
        runtime = std::make_unique<Runtime>(vfs, registry, manager, *device, nullptr);
    }

    ~LookFixture() {
        runtime.reset();
        if (device) {
            device->wait_idle();
            device->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }

    LookFixture(const LookFixture&) = delete;
    LookFixture& operator=(const LookFixture&) = delete;

    bool ok() const { return static_cast<bool>(runtime); }

    /// Builds the scene and installs the fake device as the runtime's input.
    ecs::Entity adopt(bool mouse_look = true) {
        auto scene = std::make_unique<Scene>("PlayerLook");
        ecs::World& world = scene->world();

        ecs::Entity player = world.create_entity();
        world.add<Transform>(player, Transform{});
        world.add<NameComponent>(player, NameComponent{"Player"});

        ecs::Entity cam = world.create_entity();
        CameraComponent c;
        c.is_active = true;
        c.follow_target = "Player";
        c.pivot_y = 1.5f;
        c.distance = 5.0f;
        c.yaw = 0.0f;
        c.pitch = -15.0f;
        c.smoothing = 0.0f; // snap, so a test reads the geometry and not a lerp
        c.mouse_look = mouse_look;
        world.add<CameraComponent>(cam, c);
        world.add<Transform>(cam, Transform{});

        runtime->adopt_scene(std::move(scene), "content://Scenes/PlayerLook.nfscene");
        runtime->set_playing(true);
        runtime->set_input_source(&input);
        return cam;
    }

    CameraComponent camera(ecs::Entity cam) {
        const auto* c = runtime->edit_scene()->world().get<CameraComponent>(cam);
        return c != nullptr ? *c : CameraComponent{};
    }
};

} // namespace

// --- the maths ---------------------------------------------------------------

NF_TEST(look_wrap_keeps_one_representation_of_an_angle) {
    NF_CHECK_NEAR(look_wrap_deg(0.0f), 0.0f, kEps);
    NF_CHECK_NEAR(look_wrap_deg(180.0f), 180.0f, kEps);
    NF_CHECK_NEAR(look_wrap_deg(-180.0f), 180.0f, kEps);
    NF_CHECK_NEAR(look_wrap_deg(181.0f), -179.0f, kEps);
    // Ten full turns is not 3600 degrees, and a comparison against an
    // authored angle has to see the same number the scene would.
    NF_CHECK_NEAR(look_wrap_deg(3600.0f), 0.0f, kEps);
    NF_CHECK_NEAR(look_wrap_deg(-725.0f), -5.0f, kEps);
    // A NaN cannot be wrapped; reporting zero is the only finite answer.
    NF_CHECK_NEAR(look_wrap_deg(std::nanf("")), 0.0f, kEps);
}

NF_TEST(look_angle_delta_takes_the_short_way_round) {
    // From 350 to 10 is twenty degrees forward, not three hundred and forty
    // back: auto-align that turned the long way would spin the camera.
    NF_CHECK_NEAR(look_angle_delta(350.0f, 10.0f), 20.0f, kEps);
    NF_CHECK_NEAR(look_angle_delta(10.0f, 350.0f), -20.0f, kEps);
    NF_CHECK_NEAR(look_angle_delta(0.0f, 90.0f), 90.0f, kEps);
    NF_CHECK_NEAR(look_angle_delta(0.0f, -90.0f), -90.0f, kEps);
}

NF_TEST(mouse_right_turns_right_and_mouse_down_looks_down) {
    LookSettings s;
    s.mouse_sensitivity = 0.5f; // 0.5 degrees per pixel, so the numbers are obvious
    const LookState start{0.0f, 0.0f};

    // yaw grows counter-clockwise, so turning RIGHT subtracts.
    const LookState right = look_apply_mouse(start, 100.0f, 0.0f, s);
    NF_CHECK_NEAR(right.yaw, -50.0f, kEps);
    NF_CHECK_NEAR(right.pitch, 0.0f, kEps);

    // Negative pitch is "camera above, looking down", so a downward mouse
    // push has to make pitch more negative.
    const LookState down = look_apply_mouse(start, 0.0f, 40.0f, s);
    NF_CHECK_NEAR(down.pitch, -20.0f, kEps);
    NF_CHECK_NEAR(down.yaw, 0.0f, kEps);
}

NF_TEST(invert_y_flips_the_vertical_axis_and_only_that) {
    LookSettings s;
    s.mouse_sensitivity = 0.5f;
    s.invert_y = true;
    const LookState start{0.0f, 0.0f};

    const LookState down = look_apply_mouse(start, 0.0f, 40.0f, s);
    NF_CHECK_NEAR(down.pitch, 20.0f, kEps);
    // The horizontal axis is not a preference: inverting it is a bug.
    const LookState right = look_apply_mouse(start, 100.0f, 0.0f, s);
    NF_CHECK_NEAR(right.yaw, -50.0f, kEps);
}

NF_TEST(stick_turns_at_a_constant_rate_regardless_of_frame_rate) {
    LookSettings s;
    s.stick_sensitivity = 180.0f; // a half turn per second

    // One second of full deflection, in one step and in sixty.
    LookState one = look_apply_stick(LookState{0.0f, 0.0f}, 1.0f, 0.0f, 1.0f, s);
    // A half turn — measured as a delta, because the wrap maps -180 to +180
    // and both spell the same angle.
    NF_CHECK_NEAR(std::abs(look_angle_delta(0.0f, one.yaw)), 180.0f, 1e-3f);

    LookState many{0.0f, 0.0f};
    for (int i = 0; i < 60; ++i) {
        many = look_apply_stick(many, 1.0f, 0.0f, 1.0f / 60.0f, s);
    }
    NF_CHECK_NEAR(look_angle_delta(one.yaw, many.yaw), 0.0f, 1e-2f);

    // No time passed: nothing moves. A stick read twice in one frame must not
    // turn the camera twice.
    const LookState still = look_apply_stick(LookState{30.0f, -10.0f}, 1.0f, 1.0f, 0.0f, s);
    NF_CHECK_NEAR(still.yaw, 30.0f, kEps);
    NF_CHECK_NEAR(still.pitch, -10.0f, kEps);
}

NF_TEST(look_clamp_keeps_the_orbit_legal) {
    LookSettings s;
    s.pitch_min = -60.0f;
    s.pitch_max = 30.0f;

    const LookState high = look_clamp(LookState{0.0f, -200.0f}, s);
    NF_CHECK_NEAR(high.pitch, -60.0f, kEps);
    const LookState low = look_clamp(LookState{0.0f, 200.0f}, s);
    NF_CHECK_NEAR(low.pitch, 30.0f, kEps);

    // A scene may author the range upside down; the order is repaired rather
    // than the camera being frozen.
    LookSettings flipped;
    flipped.pitch_min = 40.0f;
    flipped.pitch_max = -40.0f;
    const LookState repaired = look_clamp(LookState{0.0f, 0.0f}, flipped);
    NF_CHECK(repaired.pitch <= 40.0f);
    NF_CHECK(repaired.pitch >= -40.0f);

    // Past the pole the orbit flips, so even an absurd range stops short.
    LookSettings wide;
    wide.pitch_min = -180.0f;
    wide.pitch_max = 180.0f;
    const LookState polar = look_clamp(LookState{0.0f, 120.0f}, wide);
    NF_CHECK(polar.pitch < 90.0f);
}

NF_TEST(yaw_for_direction_puts_the_camera_behind_the_movement) {
    // Moving towards -Z means the camera belongs on +Z looking along -Z,
    // which is yaw 0.
    NF_CHECK_NEAR(look_yaw_for_direction(0.0f, -1.0f, 999.0f), 0.0f, kEps);
    // Moving towards +X: the camera belongs on -X, which is yaw -90.
    NF_CHECK_NEAR(look_yaw_for_direction(1.0f, 0.0f, 999.0f), -90.0f, kEps);
    // A standing player has no direction, so the fallback survives.
    NF_CHECK_NEAR(look_yaw_for_direction(0.0f, 0.0f, 42.0f), 42.0f, kEps);
}

NF_TEST(look_align_turns_the_short_way_and_never_overshoots) {
    // 10 degrees per second, one second: ten degrees, not ninety.
    NF_CHECK_NEAR(look_align(0.0f, 90.0f, 10.0f, 1.0f), 10.0f, kEps);
    // A tenth of a second turns a tenth as far, so the drift is smooth.
    NF_CHECK_NEAR(look_align(0.0f, 90.0f, 10.0f, 0.1f), 1.0f, kEps);
    // It must take the short way round: 350 -> 10 is +20, not -340.
    NF_CHECK_NEAR(look_align(350.0f, 10.0f, 100.0f, 1.0f), 10.0f, kEps);
    // Rate 0 disables it, which is what every scene authored before 0.4.0 has.
    NF_CHECK_NEAR(look_align(37.0f, 200.0f, 0.0f, 1.0f), 37.0f, kEps);
}

NF_TEST(zoom_moves_towards_the_scene_and_stops_at_the_limits) {
    // Forward on the wheel is closer.
    NF_CHECK_NEAR(follow_zoom(5.0f, 1.0f, 0.6f, 1.5f, 12.0f), 4.4f, kEps);
    NF_CHECK_NEAR(follow_zoom(5.0f, -1.0f, 0.6f, 1.5f, 12.0f), 5.6f, kEps);
    // The limits hold however hard the wheel is spun.
    NF_CHECK_NEAR(follow_zoom(1.6f, 100.0f, 0.6f, 1.5f, 12.0f), 1.5f, kEps);
    NF_CHECK_NEAR(follow_zoom(11.9f, -100.0f, 0.6f, 1.5f, 12.0f), 12.0f, kEps);
    // An unordered range is repaired, not obeyed backwards.
    const f32 repaired = follow_zoom(5.0f, 0.0f, 0.6f, 12.0f, 1.5f);
    NF_CHECK(repaired >= 1.5f);
    NF_CHECK(repaired <= 12.0f);
}

NF_TEST(forward_press_follows_the_camera_and_keeps_its_length) {
    // yaw 0: the camera is on +Z looking towards -Z, so W must go to -Z.
    f32 x = 0.0f;
    f32 z = 0.0f;
    move_relative(0.0f, -1.0f, 0.0f, x, z);
    NF_CHECK_NEAR(x, 0.0f, kEps);
    NF_CHECK_NEAR(z, -1.0f, kEps);

    // yaw -90: the camera looks towards +X, so W must go to +X.
    move_relative(0.0f, -1.0f, -90.0f, x, z);
    NF_CHECK_NEAR(x, 1.0f, kEps);
    NF_CHECK_NEAR(z, 0.0f, kEps);

    // Right is right, whatever the camera's yaw.
    move_relative(1.0f, 0.0f, 0.0f, x, z);
    NF_CHECK_NEAR(x, 1.0f, kEps);
    NF_CHECK_NEAR(z, 0.0f, kEps);

    // A rotation keeps the length: a diagonal must not be faster than a
    // straight line, which is the bug every hand-written rotation has.
    const f32 len = std::sqrt(0.5f * 0.5f + 0.5f * 0.5f);
    move_relative(0.5f, -0.5f, 37.0f, x, z);
    NF_CHECK_NEAR(std::sqrt(x * x + z * z), len, kEps);
}

NF_TEST(stick_deadzone_ignores_a_resting_stick_and_rescales_the_rest) {
    f32 x = 0.05f;
    f32 y = 0.05f;
    stick_deadzone(x, y, 0.18f);
    NF_CHECK_NEAR(x, 0.0f, kEps);
    NF_CHECK_NEAR(y, 0.0f, kEps);

    // Fully deflected stays fully deflected...
    x = 1.0f;
    y = 0.0f;
    stick_deadzone(x, y, 0.18f);
    NF_CHECK_NEAR(x, 1.0f, kEps);

    // ...and the first millimetre past the deadzone is slow, not a jump.
    x = 0.28f;
    y = 0.0f;
    stick_deadzone(x, y, 0.18f);
    NF_CHECK(x > 0.0f);
    NF_CHECK(x < 0.2f);

    // Garbage in, centred out.
    x = std::nanf("");
    y = 1.0f;
    stick_deadzone(x, y, 0.18f);
    NF_CHECK_NEAR(x, 0.0f, kEps);
    NF_CHECK_NEAR(y, 0.0f, kEps);
}

// --- the scene format --------------------------------------------------------

NF_TEST(camera_aiming_fields_round_trip_through_scene_text) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_camera_aim_round_trip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("AimRoundTrip");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<Transform>(e, Transform{});
    CameraComponent cam;
    cam.is_active = true;
    cam.follow_target = "Player";
    cam.mouse_look = false;
    cam.look_sensitivity = 0.25f;
    cam.stick_sensitivity = 90.0f;
    cam.pitch_min = -55.0f;
    cam.pitch_max = 35.0f;
    cam.invert_look_y = true;
    cam.zoom_min = 2.0f;
    cam.zoom_max = 9.0f;
    cam.zoom_step = 0.4f;
    cam.align_speed = 12.0f;
    scene.world().add<CameraComponent>(e, cam);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/aim.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/aim.nfscene");
    NF_CHECK(result.success);
    const auto* loaded =
        result.scene->world().get<CameraComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK(!loaded->mouse_look);
        NF_CHECK_NEAR(loaded->look_sensitivity, 0.25f, kEps);
        NF_CHECK_NEAR(loaded->stick_sensitivity, 90.0f, kEps);
        NF_CHECK_NEAR(loaded->pitch_min, -55.0f, kEps);
        NF_CHECK_NEAR(loaded->pitch_max, 35.0f, kEps);
        NF_CHECK(loaded->invert_look_y);
        NF_CHECK_NEAR(loaded->zoom_min, 2.0f, kEps);
        NF_CHECK_NEAR(loaded->zoom_max, 9.0f, kEps);
        NF_CHECK_NEAR(loaded->zoom_step, 0.4f, kEps);
        NF_CHECK_NEAR(loaded->align_speed, 12.0f, kEps);
    }
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

NF_TEST(a_camera_line_without_aiming_keys_keeps_the_defaults) {
    // Every scene written before 0.4.0 has no aiming keys, and none of them
    // may start behaving differently: the defaults ARE the aiming.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_camera_aim_legacy";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "Scenes");
    vfs.mount("content://", tmp);

    const std::string text =
        "# SANAD Scene v1\n"
        "version: 1\n"
        "name: Legacy\n"
        "entity_count: 1\n"
        "---\n"
        "entity: 0:0\n"
        "  Name: LegacyCamera\n"
        "  Transform: local(0,2,5) world(0,2,5) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  Camera: fov=60 aspect=1.777778 near=0.1 far=1000 active=true follow=Player\n";
    {
        std::ofstream out(tmp / "Scenes" / "legacy.nfscene", std::ios::binary | std::ios::trunc);
        out << text;
    }
    auto result = load_scene_from_physical((tmp / "Scenes" / "legacy.nfscene").string());
    NF_CHECK(result.success);
    const auto* loaded =
        result.scene->world().get<CameraComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK(loaded->mouse_look);
        NF_CHECK_NEAR(loaded->look_sensitivity, 0.12f, kEps);
        NF_CHECK_NEAR(loaded->pitch_min, -80.0f, kEps);
        NF_CHECK_NEAR(loaded->pitch_max, 60.0f, kEps);
        NF_CHECK(!loaded->invert_look_y);
        NF_CHECK_NEAR(loaded->align_speed, 0.0f, kEps);
    }
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// --- the runtime -------------------------------------------------------------

NF_TEST(mouse_turns_the_scenes_camera_while_playing) {
    LookFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    const ecs::Entity cam = f.adopt(true);
    const f32 before = f.camera(cam).yaw;

    // 100 pixels right at the default 0.12 deg/px is 12 degrees right, which
    // is 12 degrees less yaw.
    f.input.dx = 100.0f;
    f.runtime->update(1.0f / 60.0f);

    const f32 after = f.camera(cam).yaw;
    NF_CHECK_NEAR(look_angle_delta(before, after), -12.0f, 0.01f);
    // And the runtime asked for the pointer while a camera is being aimed.
    NF_CHECK(f.input.captured);
}

NF_TEST(a_camera_with_mouse_look_off_ignores_the_mouse) {
    LookFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    const ecs::Entity cam = f.adopt(false);
    const f32 before = f.camera(cam).yaw;

    f.input.dx = 250.0f;
    f.input.dy = 90.0f;
    f.runtime->update(1.0f / 60.0f);

    const CameraComponent after = f.camera(cam);
    NF_CHECK_NEAR(after.yaw, before, kEps);
    // A fixed camera must not steal the pointer either.
    NF_CHECK(!f.input.captured);
}

NF_TEST(the_wheel_zooms_within_the_scenes_limits) {
    LookFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    const ecs::Entity cam = f.adopt(true);
    const f32 before = f.camera(cam).distance;

    f.input.wheel = 1.0f;
    f.runtime->update(1.0f / 60.0f);
    NF_CHECK_NEAR(f.camera(cam).distance, before - 0.6f, kEps);

    // Spin it far past the near limit: the clamp is the scene's, not the
    // caller's.
    f.input.wheel = 1000.0f;
    f.runtime->update(1.0f / 60.0f);
    NF_CHECK_NEAR(f.camera(cam).distance, 1.5f, kEps);
}

NF_TEST(the_stick_turns_the_camera_too) {
    LookFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    const ecs::Entity cam = f.adopt(true);
    const f32 before = f.camera(cam).yaw;

    // Half a second of full right stick at the default 180 deg/s.
    f.input.look_x = 1.0f;
    for (int i = 0; i < 30; ++i) {
        f.runtime->update(1.0f / 60.0f);
    }
    NF_CHECK_NEAR(look_angle_delta(before, f.camera(cam).yaw), -90.0f, 0.5f);
}

NF_TEST(a_script_sees_the_camera_yaw_and_a_rotated_move_vector) {
    LookFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    f.adopt(true);
    // Aim the camera a quarter turn right of its authored yaw.
    f.input.dx = 90.0f / 0.12f;
    f.runtime->update(1.0f / 60.0f);
    NF_CHECK_NEAR(f.runtime->camera_yaw(), -90.0f, 0.05f);

    // The movement a script would read: forward now means +X.
    f32 x = 0.0f;
    f32 z = 0.0f;
    move_relative(0.0f, -1.0f, f.runtime->camera_yaw(), x, z);
    NF_CHECK_NEAR(x, 1.0f, 0.01f);
    NF_CHECK_NEAR(z, 0.0f, 0.01f);
}
