// Tests/RuntimeTests/test_runtime_vehicles.cpp — Step 11 (vehicle wiring)
//
// Rule 0 for cars. JoltVehicle and the JoltWorld vehicle API are unit-tested
// in PhysicsTests (creation, drive, wheel states, reset, replication) — what
// that suite cannot see is whether a shipped game can use any of it: no scene
// could name a car, no frame stepped one. This file is the integration half,
// following the Phase 25/28 pattern exactly — a scene that merely DECLARES a
// VehicleComponent ends up bound, driven by update() and written back into
// its Transform, with no game code driving the seam.
//
// Headless like the destruction/nav reachability suites: NF_SKIP when no
// Vulkan device is available (the Runtime constructs a renderer on adopt).

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Networking/PhysicsReplication.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Physics/Components.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::scene;

namespace {

constexpr float kDt = 1.0f / 60.0f;

struct HeadlessRuntime {
    std::filesystem::path tmp;
    VirtualFileSystem vfs;
    AssetRegistry registry;
    AssetManager manager;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<Runtime> runtime;

    explicit HeadlessRuntime(const char* dir) : manager(vfs, registry) {
        tmp = std::filesystem::temp_directory_path() / dir;
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

    ~HeadlessRuntime() {
        runtime.reset();
        if (device) {
            device->wait_idle();
            device->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }

    HeadlessRuntime(const HeadlessRuntime&) = delete;
    HeadlessRuntime& operator=(const HeadlessRuntime&) = delete;

    bool ok() const { return static_cast<bool>(runtime); }
};

/// A flat 200x200 slab (top at y = 0) plus one car dropped at (0, 2, 0).
/// The slab is wide so a full-throttle run never leaves it; the drop gives
/// the suspension something to settle on before any input is applied.
struct VehicleScene {
    ecs::Entity car = ecs::kInvalidEntity;
};

VehicleScene add_vehicle_scene(Scene& s, bool enabled = true) {
    VehicleScene out;
    ecs::Entity ground = s.world().create_entity();
    Transform gt{};
    gt.local_y = -0.5f;
    gt.scale_x = 200.0f;
    gt.scale_y = 1.0f;
    gt.scale_z = 200.0f;
    s.world().add<Transform>(ground, gt);
    physics::RigidBodyComponent rb;
    rb.type = physics::BodyType::Static;
    s.world().add<physics::RigidBodyComponent>(ground, rb);
    physics::ColliderComponent col;
    col.shape = physics::Shape::make_box(Vec3{100.0f, 0.5f, 100.0f});
    s.world().add<physics::ColliderComponent>(ground, col);

    ecs::Entity car = s.world().create_entity();
    Transform ct{};
    ct.local_y = 2.0f;
    s.world().add<Transform>(car, ct);
    physics::VehicleComponent vc;
    vc.enabled = enabled;
    s.world().add<physics::VehicleComponent>(car, vc);
    out.car = car;
    return out;
}

const Transform* car_transform(HeadlessRuntime& env, ecs::Entity car) {
    return env.runtime->scene()->world().get<Transform>(car);
}

physics::VehicleComponent* car_component(HeadlessRuntime& env, ecs::Entity car) {
    scene::Scene* s = env.runtime->edit_scene();
    if (s == nullptr) {
        return nullptr;
    }
    return s->world().get<physics::VehicleComponent>(car);
}

} // namespace

// --- Scene format ------------------------------------------------------------

NF_TEST(vehicle_line_round_trips_through_scene_text) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_vehicle_rt";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("VehicleRoundTrip");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<Transform>(e, Transform{});
    physics::VehicleComponent vc;
    vc.config.chassis_mass = 1800.0f;
    vc.config.wheel_radius = 0.4f;
    vc.config.max_steer_deg = 25.0f;
    vc.config.engine_max_torque = 650.0f;
    vc.config.suspension_frequency_hz = 2.0f;
    vc.config.tire_peak_friction = 1.3f;
    vc.enabled = false;
    // Live inputs must not survive the trip: they are session state.
    vc.throttle = 1.0f;
    vc.steer = -0.5f;
    scene.world().add<physics::VehicleComponent>(e, vc);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/v.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/v.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.warnings.empty());

    const auto* back = result.scene->world().get<physics::VehicleComponent>(e);
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK_NEAR(back->config.chassis_mass, 1800.0f, 1e-4f);
        NF_CHECK_NEAR(back->config.wheel_radius, 0.4f, 1e-5f);
        NF_CHECK_NEAR(back->config.max_steer_deg, 25.0f, 1e-4f);
        NF_CHECK_NEAR(back->config.engine_max_torque, 650.0f, 1e-4f);
        NF_CHECK_NEAR(back->config.suspension_frequency_hz, 2.0f, 1e-5f);
        NF_CHECK_NEAR(back->config.tire_peak_friction, 1.3f, 1e-5f);
        NF_CHECK(!back->enabled);
        // Scrubbed on write: a reload starts parked, never mid-throttle.
        NF_CHECK_EQ(back->throttle, 0.0f);
        NF_CHECK_EQ(back->steer, 0.0f);
        NF_CHECK_EQ(back->brake, 0.0f);
        NF_CHECK_EQ(back->handbrake, 0.0f);
        NF_CHECK(back->vehicle == nullptr);
    }
    std::filesystem::remove_all(tmp);
}

NF_TEST(vehicle_line_refuses_impossible_values) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_vehicle_bad";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    // A negative mass is a formally-valid float the solver cannot use, and a
    // minimum suspension travel past the maximum is a spring that can never
    // extend: both refuse the whole line rather than clamping silently.
    const std::string text =
        "# SANAD Scene v1\n"
        "version: 1\n"
        "name: Bad\n"
        "entity_count: 2\n"
        "---\n"
        "entity: 0:0\n"
        "  Name: Heavy\n"
        "  Vehicle: mass=-4\n"
        "---\n"
        "entity: 1:0\n"
        "  Name: Spring\n"
        "  Vehicle: susp_min=1.0 susp_max=0.5\n";
    NF_CHECK(vfs.write_text("content://Scenes/bad.nfscene", text).ok);

    auto result = load_scene_from_vfs(vfs, "content://Scenes/bad.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene->world().query<physics::VehicleComponent>().empty());
    NF_CHECK(!result.warnings.empty());
    std::filesystem::remove_all(tmp);
}

// --- The frame drives the car -------------------------------------------------

NF_TEST(scene_vehicle_drives_forward_on_throttle) {
    HeadlessRuntime env("nf_runtime_vehicles");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    rhi::reset_validation_error_count();
    auto s = std::make_unique<Scene>("Drive");
    const VehicleScene vs = add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Drive.nfscene");

    // The bind: one car, one Jolt world behind it.
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(1));
    physics::VehicleComponent* vc = car_component(env, vs.car);
    NF_CHECK(vc != nullptr);
    NF_CHECK(vc->vehicle != nullptr);
    if (vc == nullptr) {
        return;
    }
    vc->throttle = 1.0f;

    // Five seconds at full throttle from a 2 m drop: the car lands, grips and
    // leaves along +Z (front = +Z). A car that never moved is a system that
    // never stepped.
    for (int i = 0; i < 300; ++i) {
        env.runtime->update(kDt);
    }
    const Transform* t = car_transform(env, vs.car);
    NF_CHECK(t != nullptr);
    if (t != nullptr) {
        NF_CHECK(t->local_z > 2.0f);
        NF_CHECK(std::abs(t->local_x) < 2.0f);
        // Still on the slab, not through it: the statics synced.
        NF_CHECK(t->local_y > -0.5f && t->local_y < 3.0f);
    }
    NF_CHECK(rhi::validation_error_count() == 0);
    rhi::reset_validation_error_count();
}

NF_TEST(vehicle_sits_still_without_input) {
    HeadlessRuntime env("nf_runtime_vehicles_parked");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Parked");
    const VehicleScene vs = add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Parked.nfscene");
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(1));

    // Two seconds, no throttle: the drop settles onto the suspension and the
    // car stays where it landed. An XZ drift here is a solver that moves
    // parked cars.
    for (int i = 0; i < 120; ++i) {
        env.runtime->update(kDt);
    }
    const Transform* t = car_transform(env, vs.car);
    NF_CHECK(t != nullptr);
    if (t != nullptr) {
        NF_CHECK_NEAR(t->local_x, 0.0f, 0.1f);
        NF_CHECK_NEAR(t->local_z, 0.0f, 0.1f);
    }
}

NF_TEST(edit_freeze_holds_the_car_still) {
    HeadlessRuntime env("nf_runtime_vehicles_frozen");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Frozen");
    const VehicleScene vs = add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Frozen.nfscene");
    physics::VehicleComponent* vc = car_component(env, vs.car);
    NF_CHECK(vc != nullptr);
    if (vc != nullptr) {
        vc->throttle = 1.0f;
    }
    // dt = 0 is the editor's edit mode: full throttle and all, nothing
    // simulates. A car that drove here would make an unplayed scene move.
    for (int i = 0; i < 30; ++i) {
        env.runtime->update(0.0f);
    }
    const Transform* t = car_transform(env, vs.car);
    NF_CHECK(t != nullptr);
    if (t != nullptr) {
        NF_CHECK_EQ(t->local_x, 0.0f);
        NF_CHECK_EQ(t->local_y, 2.0f);
        NF_CHECK_EQ(t->local_z, 0.0f);
    }
}

NF_TEST(disabled_vehicle_never_binds_or_drives) {
    HeadlessRuntime env("nf_runtime_vehicles_disabled");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Showroom");
    const VehicleScene vs = add_vehicle_scene(*s, false);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Showroom.nfscene");

    // No Jolt world, no car, no cost — and no crash stepping the absence.
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(0));
    physics::VehicleComponent* vc = car_component(env, vs.car);
    NF_CHECK(vc != nullptr);
    if (vc != nullptr) {
        vc->throttle = 1.0f;
    }
    for (int i = 0; i < 120; ++i) {
        env.runtime->update(kDt);
    }
    const Transform* t = car_transform(env, vs.car);
    NF_CHECK(t != nullptr);
    if (t != nullptr) {
        NF_CHECK_EQ(t->local_x, 0.0f);
        NF_CHECK_EQ(t->local_z, 0.0f);
    }
}

NF_TEST(scene_without_vehicles_tears_down_the_world) {
    HeadlessRuntime env("nf_runtime_vehicles_teardown");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Drive");
    add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Drive.nfscene");
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(1));

    // A new scene with no car drops the previous scene's world, statics and
    // cars — per-scene artefacts never carry over.
    auto quiet = std::make_unique<Scene>("Quiet");
    env.runtime->adopt_scene(std::move(quiet), "content://Scenes/Quiet.nfscene");
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(0));
    env.runtime->update(kDt);
}

NF_TEST(two_runs_drive_the_same_line) {
    // Jolt steps deterministically (pinned in PhysicsTests): two identical
    // runs land the car on the identical transform, bit for bit.
    auto run = [](const char* dir, Transform& out_pose) {
        HeadlessRuntime env(dir);
        if (!env.ok()) {
            return false;
        }
        auto s = std::make_unique<Scene>("Repeatable");
        const VehicleScene vs = add_vehicle_scene(*s);
        env.runtime->adopt_scene(std::move(s), "content://Scenes/Repeatable.nfscene");
        physics::VehicleComponent* vc = car_component(env, vs.car);
        if (vc == nullptr) {
            return false;
        }
        vc->throttle = 0.8f;
        vc->steer = 0.1f;
        for (int i = 0; i < 240; ++i) {
            env.runtime->update(kDt);
        }
        const Transform* t = car_transform(env, vs.car);
        if (t == nullptr) {
            return false;
        }
        out_pose = *t;
        return true;
    };
    Transform a{}, b{};
    NF_CHECK(run("nf_runtime_vehicles_rep_a", a));
    NF_CHECK(run("nf_runtime_vehicles_rep_b", b));
    NF_CHECK_EQ(a.local_x, b.local_x);
    NF_CHECK_EQ(a.local_y, b.local_y);
    NF_CHECK_EQ(a.local_z, b.local_z);
    NF_CHECK_EQ(a.rot_x, b.rot_x);
    NF_CHECK_EQ(a.rot_y, b.rot_y);
    NF_CHECK_EQ(a.rot_z, b.rot_z);
    // ...and it actually went somewhere (a shared no-op is identically equal
    // too, which is why the drive test above exists alongside this one).
    NF_CHECK(std::abs(a.local_z) > 0.5f);
}

NF_TEST(reset_vehicle_uprights_the_car_in_place) {
    HeadlessRuntime env("nf_runtime_vehicles_reset");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Reset");
    const VehicleScene vs = add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Reset.nfscene");
    physics::VehicleComponent* vc = car_component(env, vs.car);
    NF_CHECK(vc != nullptr);
    if (vc == nullptr) {
        return;
    }
    vc->throttle = 1.0f;
    for (int i = 0; i < 120; ++i) {
        env.runtime->update(kDt);
    }
    const Transform* before = car_transform(env, vs.car);
    NF_CHECK(before != nullptr);
    const f32 reset_x = before != nullptr ? before->local_x : 0.0f;
    const f32 reset_z = before != nullptr ? before->local_z : 0.0f;

    NF_CHECK(env.runtime->reset_vehicle(vs.car));
    const Transform* after = car_transform(env, vs.car);
    NF_CHECK(after != nullptr);
    if (after != nullptr) {
        // Upright where it stands: identity rotation, same ground position.
        NF_CHECK_EQ(after->rot_x, 0.0f);
        NF_CHECK_EQ(after->rot_y, 0.0f);
        NF_CHECK_EQ(after->rot_z, 0.0f);
        NF_CHECK_NEAR(after->local_x, reset_x, 1e-4f);
        NF_CHECK_NEAR(after->local_z, reset_z, 1e-4f);
    }
    NF_CHECK(!env.runtime->reset_vehicle(ecs::kInvalidEntity));
}

NF_TEST(wheel_states_report_contact_while_parked) {
    HeadlessRuntime env("nf_runtime_vehicles_wheels");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Wheels");
    const VehicleScene vs = add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Wheels.nfscene");

    // Settle first: mid-air wheels touch nothing, which is true, not useful.
    for (int i = 0; i < 120; ++i) {
        env.runtime->update(kDt);
    }
    std::vector<physics::JoltWheelState> wheels;
    NF_CHECK(env.runtime->vehicle_wheel_states(vs.car, wheels));
    NF_CHECK_EQ(wheels.size(), static_cast<size_t>(4));
    for (const physics::JoltWheelState& w : wheels) {
        NF_CHECK(w.in_contact);
    }
    NF_CHECK(!env.runtime->vehicle_wheel_states(ecs::kInvalidEntity, wheels));
}

// --- Wheels reach the render world (Step 12) ---------------------------------

NF_TEST(scene_wheels_reach_the_render_world) {
    HeadlessRuntime env("nf_runtime_vehicles_wheels_render");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("WheelsRender");
    const VehicleScene vs = add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/WheelsRender.nfscene");

    // The pose comes from the bind, not from simulation: even the frozen
    // editor viewport (dt = 0) shows the car's wheels.
    env.runtime->update(0.0f);
    const rendering::RenderWorld drawn = env.runtime->render_world_snapshot();
    std::vector<const rendering::RenderObject*> wheel_objects;
    for (const rendering::RenderObject& ro : drawn.objects) {
        if (ro.id == 0) {
            wheel_objects.push_back(&ro);
        }
    }
    // Four wheels — the physics rig's fixed shape (two axles, two sides) —
    // and nothing else: the test scene has no mapped meshes, so every object
    // in this snapshot that is not a wheel would be geometry that leaked in.
    NF_CHECK_EQ(wheel_objects.size(), static_cast<size_t>(4));
    for (size_t i = 0; i < wheel_objects.size(); ++i) {
        const rendering::RenderObject* ro = wheel_objects[i];
        NF_CHECK(ro->mesh_handle.valid());
        // No two wheels at the same spot: the four states are the four
        // distinct corners of the rig, even before the car has moved.
        for (size_t j = i + 1; j < wheel_objects.size(); ++j) {
            const bool same_spot =
                wheel_objects[j]->world.m[3][0] == ro->world.m[3][0] &&
                wheel_objects[j]->world.m[3][1] == ro->world.m[3][1] &&
                wheel_objects[j]->world.m[3][2] == ro->world.m[3][2];
            NF_CHECK(!same_spot);
        }
    }

    // A parked showroom car (enabled = false) draws no wheels: nothing binds,
    // so the wheel builder has no states to pose.
    auto quiet = std::make_unique<Scene>("ShowroomQuiet");
    add_vehicle_scene(*quiet, false);
    env.runtime->adopt_scene(std::move(quiet), "content://Scenes/ShowroomQuiet.nfscene");
    env.runtime->update(0.0f);
    const rendering::RenderWorld after = env.runtime->render_world_snapshot();
    for (const rendering::RenderObject& ro : after.objects) {
        NF_CHECK(ro.id != 0);
    }
}

NF_TEST(wheels_spin_with_drive_and_hold_when_frozen) {
    HeadlessRuntime env("nf_runtime_vehicles_wheels_spin");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Spin");
    const VehicleScene vs = add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Spin.nfscene");

    // Parked, frozen: identity wheel poses, bit-stable across rebuilds.
    env.runtime->update(0.0f);
    const rendering::RenderWorld parked = env.runtime->render_world_snapshot();
    env.runtime->update(0.0f);
    const rendering::RenderWorld parked_again = env.runtime->render_world_snapshot();
    NF_CHECK_EQ(parked.objects.size(), parked_again.objects.size());
    for (size_t i = 0; i < parked.objects.size() && i < parked_again.objects.size(); ++i) {
        NF_CHECK_EQ(parked.objects[i].world.m[3][0], parked_again.objects[i].world.m[3][0]);
        NF_CHECK_EQ(parked.objects[i].world.m[3][1], parked_again.objects[i].world.m[3][1]);
        NF_CHECK_EQ(parked.objects[i].world.m[3][2], parked_again.objects[i].world.m[3][2]);
    }

    // Two seconds at full throttle: the car drives and the wheels roll. A
    // wheel that never rotated while the car moved is integration that never
    // ran — the visible bug the frame-mounted wheels exist to prevent.
    physics::VehicleComponent* vc = car_component(env, vs.car);
    NF_CHECK(vc != nullptr);
    if (vc == nullptr) {
        return;
    }
    vc->throttle = 1.0f;
    for (int i = 0; i < 120; ++i) {
        env.runtime->update(kDt);
    }
    const rendering::RenderWorld drove = env.runtime->render_world_snapshot();
    bool any_wheel_rotated = false;
    for (const rendering::RenderObject& ro : drove.objects) {
        // id 0 is the wheel injection; the test scene maps no meshes otherwise.
        if (ro.id != 0) {
            continue;
        }
        const bool changed =
            std::abs(ro.world.m[0][0] - parked.objects[0].world.m[0][0]) > 1e-4f ||
            std::abs(ro.world.m[1][1] - parked.objects[0].world.m[1][1]) > 1e-4f;
        if (changed) {
            any_wheel_rotated = true;
        }
    }
    NF_CHECK(any_wheel_rotated);
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(1));
}

NF_TEST(unbinding_drops_the_wheels) {
    HeadlessRuntime env("nf_runtime_vehicles_wheels_unbind");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Bind");
    add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Bind.nfscene");
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(1));
    env.runtime->update(0.0f);
    size_t wheel_objects_before = 0;
    const rendering::RenderWorld bound = env.runtime->render_world_snapshot();
    for (const rendering::RenderObject& ro : bound.objects) {
        if (ro.id == 0) {
            ++wheel_objects_before;
        }
    }
    NF_CHECK_EQ(wheel_objects_before, static_cast<size_t>(4));

    auto quiet = std::make_unique<Scene>("Quiet");
    env.runtime->adopt_scene(std::move(quiet), "content://Scenes/Quiet.nfscene");
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(0));
    env.runtime->update(0.0f);
    const rendering::RenderWorld gone = env.runtime->render_world_snapshot();
    for (const rendering::RenderObject& ro : gone.objects) {
        NF_CHECK(ro.id != 0);
    }
}

// --- Moving obstacles as solid mirrors (Step 13) ------------------------------
//
// Scene bodies the engine solver simulates (Dynamic, Kinematic) reach the
// vehicle world as KINEMATIC mirrors: solid to the car, pose-driven from the
// solver, integrating nothing themselves. The bind test counts them; the drive
// test baselines the same car on the same slab without a crate and proves the
// one WITH the crate is held behind it — a test that would fail if the mirror
// were ever silently skipped.

ecs::Entity add_crate(Scene& s, Vec3 position, Vec3 half, physics::BodyType type) {
    ecs::Entity e = s.world().create_entity();
    Transform t{};
    t.local_x = position.x;
    t.local_y = position.y;
    t.local_z = position.z;
    s.world().add<Transform>(e, t);
    physics::RigidBodyComponent rb;
    rb.type = type;
    rb.mass = 2000.0f;
    s.world().add<physics::RigidBodyComponent>(e, rb);
    physics::ColliderComponent col;
    col.shape = physics::Shape::make_box(half);
    s.world().add<physics::ColliderComponent>(e, col);
    return e;
}

NF_TEST(simulated_bodies_mirror_as_solid_obstacles) {
    HeadlessRuntime env("nf_runtime_vehicles_moving_obstacles");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Moving");
    add_vehicle_scene(*s);
    add_crate(*s, Vec3{0.0f, 0.75f, 6.0f}, Vec3{0.75f, 0.75f, 0.75f},
              physics::BodyType::Dynamic);
    // A kinematic platform: already moved by scripts in the engine world, a
    // solid mover here — the same mirror path as dynamics.
    add_crate(*s, Vec3{4.0f, 1.75f, -6.0f}, Vec3{1.5f, 0.25f, 1.5f},
              physics::BodyType::Kinematic);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Moving.nfscene");
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(1));
    NF_CHECK_EQ(env.runtime->vehicle_obstacle_count(), static_cast<size_t>(2));

    // Mirrors belong to the vehicle binding: a scene with no car binds no
    // vehicle world, so it mirrors nothing even where bodies exist.
    auto quiet = std::make_unique<Scene>("QuietMoving");
    add_crate(*quiet, Vec3{0.0f, 0.75f, 6.0f}, Vec3{0.75f, 0.75f, 0.75f},
              physics::BodyType::Dynamic);
    env.runtime->adopt_scene(std::move(quiet), "content://Scenes/QuietMoving.nfscene");
    NF_CHECK_EQ(env.runtime->vehicle_count(), static_cast<size_t>(0));
    NF_CHECK_EQ(env.runtime->vehicle_obstacle_count(), static_cast<size_t>(0));
}

NF_TEST(car_cannot_drive_through_a_dynamic_crate) {
    HeadlessRuntime env("nf_runtime_vehicles_obstacle_stop");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    // Run one: the open slab. How far THIS car gets with nothing ahead.
    auto free_scene = std::make_unique<Scene>("FreeRun");
    const VehicleScene vs = add_vehicle_scene(*free_scene);
    env.runtime->adopt_scene(std::move(free_scene), "content://Scenes/FreeRun.nfscene");
    physics::VehicleComponent* vc = car_component(env, vs.car);
    NF_CHECK(vc != nullptr);
    if (vc == nullptr) {
        return;
    }
    vc->throttle = 1.0f;
    for (int i = 0; i < 360; ++i) {
        env.runtime->update(kDt);
    }
    const Transform* free_t = car_transform(env, vs.car);
    NF_CHECK(free_t != nullptr);
    if (free_t == nullptr) {
        return;
    }
    const f32 free_z = free_t->local_z;

    // Run two: the same drive with a heavy crate parked on the path, its near
    // face planted in front of where the free run would already be.
    const f32 crate_z = free_z - 2.0f;
    std::unique_ptr<Scene> blocked_scene = std::make_unique<Scene>("BlockedRun");
    const VehicleScene blocked_vs = add_vehicle_scene(*blocked_scene);
    const ecs::Entity crate =
        add_crate(*blocked_scene, Vec3{0.0f, 0.75f, crate_z}, Vec3{0.75f, 0.75f, 0.75f},
                  physics::BodyType::Dynamic);
    env.runtime->adopt_scene(std::move(blocked_scene), "content://Scenes/BlockedRun.nfscene");
    NF_CHECK_EQ(env.runtime->vehicle_obstacle_count(), static_cast<size_t>(1));
    physics::VehicleComponent* blocked_vc = car_component(env, blocked_vs.car);
    NF_CHECK(blocked_vc != nullptr);
    if (blocked_vc == nullptr) {
        return;
    }
    blocked_vc->throttle = 1.0f;
    for (int i = 0; i < 360; ++i) {
        env.runtime->update(kDt);
    }
    const Transform* blocked_t = car_transform(env, blocked_vs.car);
    NF_CHECK(blocked_t != nullptr);
    if (blocked_t == nullptr) {
        return;
    }
    // Held behind the crate: the chassis reaches to z + 2.0, so being at or
    // past the crate's near face (crate_z - 0.75) means the mirror failed to
    // block, and lagging the free run by a full car length means it pushed
    // back with its own motion.
    NF_CHECK(blocked_t->local_z < crate_z - 1.8f);
    NF_CHECK(free_z - blocked_t->local_z > 2.0f);
    NF_CHECK(std::abs(blocked_t->local_x) < 2.0f); // straight, like the free run

    // The coupling is one-way BY DESIGN: the car lives in another world, and
    // the crate's own solver body never felt it — still resting where it
    // spawned, never flung or dragged.
    const Transform* crate_t =
        env.runtime->scene()->world().get<Transform>(crate);
    NF_CHECK(crate_t != nullptr);
    if (crate_t != nullptr) {
        NF_CHECK(std::abs(crate_t->local_z - crate_z) < 0.8f);
        NF_CHECK(crate_t->local_y > -0.1f && crate_t->local_y < 1.2f);
    }
    NF_CHECK(rhi::validation_error_count() == 0);
    rhi::reset_validation_error_count();
}

// --- Vehicle network replication (Step 14) ------------------------------------
//
// The wire format, gates and ghost logic ship in NFNetworking; what was
// deferred is the GLUE. These tests wire it end to end the way a session
// would: runtime collector -> net::VehicleSnapshot -> encode (the same 'NFVS'
// bytes DedicatedServer would send) -> decode -> runtime applier, plus the two
// correctness gates (stale ticks, foreign authority on a simulated car).

net::VehicleSnapshot to_wire(u32 tick, const std::vector<Runtime::VehicleNetRecord>& recs) {
    net::VehicleSnapshot snap;
    snap.tick = tick;
    snap.vehicles.reserve(recs.size());
    for (const Runtime::VehicleNetRecord& r : recs) {
        net::VehicleNetState st;
        st.entity = r.net_id;
        st.x = r.state.position.x;
        st.y = r.state.position.y;
        st.z = r.state.position.z;
        st.vx = r.state.linear_velocity.x;
        st.vy = r.state.linear_velocity.y;
        st.vz = r.state.linear_velocity.z;
        st.qx = r.state.rotation.x;
        st.qy = r.state.rotation.y;
        st.qz = r.state.rotation.z;
        st.qw = r.state.rotation.w;
        snap.vehicles.push_back(st);
    }
    return snap;
}

std::vector<Runtime::VehicleNetRecord> from_wire(const net::VehicleSnapshot& snap) {
    std::vector<Runtime::VehicleNetRecord> out;
    out.reserve(snap.vehicles.size());
    for (const net::VehicleNetState& st : snap.vehicles) {
        Runtime::VehicleNetRecord r;
        r.net_id = st.entity;
        r.state.position = Vec3{st.x, st.y, st.z};
        r.state.linear_velocity = Vec3{st.vx, st.vy, st.vz};
        r.state.rotation = Quat{st.qx, st.qy, st.qz, st.qw};
        r.state.active = true;
        out.push_back(r);
    }
    return out;
}

NF_TEST(vehicle_net_states_collect_assigned_cars_only) {
    HeadlessRuntime env("nf_runtime_vehicles_net_collect");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Net");
    const VehicleScene vs = add_vehicle_scene(*s);
    // A second car with no wire identity: unassigned means unreplicated.
    ecs::Entity stray = s->world().create_entity();
    Transform st{};
    st.local_x = 5.0f;
    st.local_y = 2.0f;
    s->world().add<Transform>(stray, st);
    physics::VehicleComponent stray_vc;
    s->world().add<physics::VehicleComponent>(stray, stray_vc);

    env.runtime->adopt_scene(std::move(s), "content://Scenes/Net.nfscene");
    env.runtime->assign_vehicle_net_id(vs.car, 11);
    env.runtime->assign_vehicle_net_id(stray, 0); // zero is unwireable: refused

    std::vector<Runtime::VehicleNetRecord> recs;
    NF_CHECK(env.runtime->collect_vehicle_net_states(recs));
    NF_CHECK_EQ(recs.size(), static_cast<size_t>(1));
    if (recs.size() != 1) {
        return;
    }
    NF_CHECK_EQ(recs[0].net_id, 11u);
    // Collected right at the bind: the chassis is where the car spawned.
    NF_CHECK_NEAR(recs[0].state.position.y, 2.0f, 1e-4f);

    // No bind, nothing authoritative: the collector says so explicitly.
    auto quiet = std::make_unique<Scene>("NetQuiet");
    env.runtime->adopt_scene(std::move(quiet), "content://Scenes/NetQuiet.nfscene");
    NF_CHECK(!env.runtime->collect_vehicle_net_states(recs));
}

NF_TEST(vehicle_net_round_trip_and_ghost_apply) {
    HeadlessRuntime env("nf_runtime_vehicles_net_roundtrip");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    // Server: a car drives two seconds and gets collected.
    auto server = std::make_unique<Scene>("Server");
    const VehicleScene car = add_vehicle_scene(*server);
    env.runtime->adopt_scene(std::move(server), "content://Scenes/Server.nfscene");
    env.runtime->assign_vehicle_net_id(car.car, 5);
    physics::VehicleComponent* vc = car_component(env, car.car);
    NF_CHECK(vc != nullptr);
    if (vc == nullptr) {
        return;
    }
    vc->throttle = 1.0f;
    vc->steer = 0.25f;
    for (int i = 0; i < 120; ++i) {
        env.runtime->update(kDt);
    }
    const Transform* driven = car_transform(env, car.car);
    NF_CHECK(driven != nullptr);
    if (driven == nullptr) {
        return;
    }

    std::vector<Runtime::VehicleNetRecord> recs;
    NF_CHECK(env.runtime->collect_vehicle_net_states(recs));
    NF_CHECK_EQ(recs.size(), static_cast<size_t>(1));

    // The wire hop: exactly the codec the server host uses.
    const std::vector<u8> bytes = net::encode_vehicle_snapshot(to_wire(7, recs));
    net::VehicleSnapshot decoded;
    std::string err;
    NF_CHECK(net::decode_vehicle_snapshot(bytes.data(), bytes.size(), decoded, err));
    const std::vector<Runtime::VehicleNetRecord> remote = from_wire(decoded);
    NF_CHECK_EQ(remote.size(), static_cast<size_t>(1));

    // Client: the ghost car's world simulates nothing; the correction poses it.
    auto client = std::make_unique<Scene>("Ghost");
    const VehicleScene ghost = add_vehicle_scene(*client, false);
    env.runtime->adopt_scene(std::move(client), "content://Scenes/Ghost.nfscene");
    env.runtime->assign_vehicle_net_id(ghost.car, 5);
    NF_CHECK_EQ(env.runtime->apply_vehicle_net_states(remote, 7), 1u);
    const Transform* posed = car_transform(env, ghost.car);
    NF_CHECK(posed != nullptr);
    if (posed == nullptr) {
        return;
    }
    NF_CHECK_NEAR(posed->local_x, driven->local_x, 1e-3f);
    NF_CHECK_NEAR(posed->local_y, driven->local_y, 1e-3f);
    NF_CHECK_NEAR(posed->local_z, driven->local_z, 1e-3f);
    // Heading crossed the wire too (the sim steered, so this is not identity).
    NF_CHECK_NEAR(posed->rot_x, driven->rot_x, 1e-2f);
    NF_CHECK_NEAR(posed->rot_y, driven->rot_y, 1e-2f);
    NF_CHECK_NEAR(posed->rot_z, driven->rot_z, 1e-2f);

    // The freshness gate: the same correction twice is accepted once.
    NF_CHECK_EQ(env.runtime->apply_vehicle_net_states(remote, 7), 0u);
}

NF_TEST(vehicle_net_apply_skips_locally_simulated_car) {
    HeadlessRuntime env("nf_runtime_vehicles_net_guard");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Guard");
    const VehicleScene vs = add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Guard.nfscene");
    env.runtime->assign_vehicle_net_id(vs.car, 3);
    physics::VehicleComponent* vc = car_component(env, vs.car);
    NF_CHECK(vc != nullptr);
    if (vc == nullptr) {
        return;
    }
    vc->throttle = 1.0f;
    for (int i = 0; i < 60; ++i) {
        env.runtime->update(kDt);
    }
    const Transform* t = car_transform(env, vs.car);
    NF_CHECK(t != nullptr);
    if (t == nullptr) {
        return;
    }
    const f32 local_z = t->local_z;

    // A foreign packet pretending authority over a body this world simulates:
    // every record must be refused — the pose would visibly snap otherwise.
    Runtime::VehicleNetRecord intruder;
    intruder.net_id = 3;
    intruder.state.position = Vec3{0, 5.0f, 999.0f};
    intruder.state.rotation = Quat::identity();
    std::vector<Runtime::VehicleNetRecord> recs;
    recs.push_back(intruder);
    recs.push_back(Runtime::VehicleNetRecord{}); // net id 0: unwireable
    NF_CHECK_EQ(env.runtime->apply_vehicle_net_states(recs, 5), 0u);
    NF_CHECK(t->local_z == local_z);
    // Tick 0 from a real record is likewise never a correction.
    recs.clear();
    recs.push_back(intruder);
    NF_CHECK_EQ(env.runtime->apply_vehicle_net_states(recs, 0), 0u);
}

NF_TEST(merge_scrubs_live_inputs_and_the_jolt_view) {
    // A merge that carried throttle (or the JoltVehicle pointer) would drive
    // a fresh car with a stale foot — or through a dangling pointer.
    HeadlessRuntime env("nf_runtime_vehicles_merge");
    if (!env.ok()) {
        NF_SKIP("no Vulkan device");
        return;
    }
    auto s = std::make_unique<Scene>("Drive");
    const VehicleScene vs = add_vehicle_scene(*s);
    env.runtime->adopt_scene(std::move(s), "content://Scenes/Drive.nfscene");
    physics::VehicleComponent* vc = car_component(env, vs.car);
    NF_CHECK(vc != nullptr);
    NF_CHECK(vc->vehicle != nullptr);
    if (vc == nullptr) {
        return;
    }
    vc->throttle = 1.0f;
    vc->steer = -0.5f;

    const ecs::World& src = env.runtime->scene()->world();
    ecs::World dst;
    const ecs::Entity de = dst.create_entity();
    copy_scene_entity(src, vs.car, dst, de);
    const auto* back = dst.get<physics::VehicleComponent>(de);
    NF_CHECK(back != nullptr);
    if (back != nullptr) {
        NF_CHECK_EQ(back->throttle, 0.0f);
        NF_CHECK_EQ(back->steer, 0.0f);
        NF_CHECK_EQ(back->brake, 0.0f);
        NF_CHECK_EQ(back->handbrake, 0.0f);
        NF_CHECK(back->vehicle == nullptr);
        // The tune crosses untouched: scrubbing is about session state, not
        // about flattening the author's car.
        NF_CHECK_NEAR(back->config.chassis_mass, vc->config.chassis_mass, 1e-6f);
    }
}
