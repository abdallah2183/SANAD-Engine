// Tests/RuntimeTests/test_camera_follow.cpp — the third-person camera.
//
// A camera that always looks at the origin is a camera that can only show a
// scene arranged around the origin, and no third-person game is that. These
// cases pin the three decisions a follow camera makes -- WHERE it looks,
// HOW FAR it sits, and HOW IT MOVES -- and the one that makes it usable in
// a forest: what it does when a tree is in the way.
//
// The maths is tested on its own (no ECS, no GPU, no frame loop) because a
// camera bug is a maths bug, and a maths bug is the easiest kind to pin
// down. The runtime cases then prove the maths is actually WIRED: a scene
// that declares a follow camera has to end up with the camera behind the
// character, not at the origin.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/CameraFollow.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::scene;

namespace {

constexpr float kEps = 1e-4f;

/// A headless runtime with a scene in it.
struct FollowFixture {
    std::filesystem::path tmp;
    VirtualFileSystem vfs;
    AssetRegistry registry;
    AssetManager manager;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<Runtime> runtime;

    FollowFixture() : manager(vfs, registry) {
        tmp = nf::test::temp_root() / "nf_camera_follow";
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

    ~FollowFixture() {
        runtime.reset();
        if (device) {
            device->wait_idle();
            device->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }

    FollowFixture(const FollowFixture&) = delete;
    FollowFixture& operator=(const FollowFixture&) = delete;

    bool ok() const { return static_cast<bool>(runtime); }

    /// Puts a scene into the runtime: a named entity at `position` and a
    /// camera that follows it.
    struct SceneSpec {
        Vec3 player{0.0f, 0.0f, 0.0f};
        std::string follow = "Player";
        float pivot_y = 1.5f;
        float distance = 5.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
        float smoothing = 0.0f;
    };

    ecs::Entity adopt(const SceneSpec& spec) {
        auto scene = std::make_unique<Scene>("CameraFollow");
        ecs::World& world = scene->world();
        ecs::Entity player = world.create_entity();
        Transform t{};
        t.local_x = spec.player.x;
        t.local_y = spec.player.y;
        t.local_z = spec.player.z;
        world.add<Transform>(player, t);
        world.add<NameComponent>(player, NameComponent{"Player"});

        ecs::Entity cam = world.create_entity();
        CameraComponent c;
        c.is_active = true;
        c.follow_target = spec.follow;
        c.pivot_y = spec.pivot_y;
        c.distance = spec.distance;
        c.yaw = spec.yaw;
        c.pitch = spec.pitch;
        c.smoothing = spec.smoothing;
        world.add<CameraComponent>(cam, c);
        world.add<Transform>(cam, Transform{});

        runtime->adopt_scene(std::move(scene), "content://Scenes/CameraFollow.nfscene");
        return player;
    }

    ecs::World& world() { return runtime->edit_scene()->world(); }
};

} // namespace

// --- the maths ---------------------------------------------------------------

NF_TEST(follow_camera_sits_behind_the_target_at_the_requested_distance) {
    const Vec3 pivot{10.0f, 1.6f, -4.0f};
    // yaw 0 = the camera is behind the target on +Z, looking back at it.
    const Vec3 back = follow_camera_position(pivot, 0.0f, 0.0f, 5.0f);
    NF_CHECK_NEAR(back.x, pivot.x, kEps);
    NF_CHECK_NEAR(back.y, pivot.y, kEps);
    NF_CHECK_NEAR(back.z, pivot.z + 5.0f, kEps);
}

NF_TEST(follow_camera_orbits_the_pivot_with_its_yaw) {
    const Vec3 pivot{0.0f, 0.0f, 0.0f};
    // A quarter turn puts the camera on +X.
    const Vec3 side = follow_camera_position(pivot, 90.0f, 0.0f, 4.0f);
    NF_CHECK_NEAR(side.x, 4.0f, kEps);
    NF_CHECK_NEAR(side.z, 0.0f, kEps);
    // Half a turn puts it on -Z.
    const Vec3 front = follow_camera_position(pivot, 180.0f, 0.0f, 4.0f);
    NF_CHECK_NEAR(front.x, 0.0f, kEps);
    NF_CHECK_NEAR(front.z, -4.0f, kEps);
}

NF_TEST(follow_camera_pitch_raises_and_lowers_the_camera) {
    const Vec3 pivot{0.0f, 0.0f, 0.0f};
    // A negative pitch looks DOWN, so the camera sits ABOVE the pivot. This is
    // the convention every engine uses, and the opposite one puts a
    // third-person camera under the character's chin.
    const Vec3 above = follow_camera_position(pivot, 0.0f, -30.0f, 5.0f);
    NF_CHECK(above.y > 1.0f);
    // The distance is the distance, whatever the pitch.
    NF_CHECK_NEAR((above - pivot).length(), 5.0f, kEps);
    // A positive pitch puts it below.
    const Vec3 below = follow_camera_position(pivot, 0.0f, 30.0f, 5.0f);
    NF_CHECK(below.y < -1.0f);
}

NF_TEST(follow_camera_smoothing_covers_the_same_ground_per_second_at_any_frame_rate) {
    // The point of the exponential blend: a camera that covers a fixed
    // fraction of the gap per FRAME is twice as fast on a fast machine, and
    // the scene changes feel with the frame rate.
    const float at30 = follow_camera_blend(8.0f, 1.0f / 30.0f);
    const float at60 = follow_camera_blend(8.0f, 1.0f / 60.0f);
    const float at240 = follow_camera_blend(8.0f, 1.0f / 240.0f);
    NF_CHECK(at30 > at60);
    NF_CHECK(at60 > at240);
    // Sixty frames of 60 Hz and two hundred and forty frames of 240 Hz cover
    // the same fraction of the gap.
    const float one_second_at_60 = 1.0f - std::pow(1.0f - at60, 60.0f);
    const float one_second_at_240 = 1.0f - std::pow(1.0f - at240, 240.0f);
    NF_CHECK_NEAR(one_second_at_60, one_second_at_240, 1e-3f);
}

NF_TEST(follow_camera_without_smoothing_snaps) {
    NF_CHECK_NEAR(follow_camera_blend(0.0f, 1.0f / 60.0f), 1.0f, kEps);
    // No time passed: nothing moves.
    NF_CHECK_NEAR(follow_camera_blend(8.0f, 0.0f), 0.0f, kEps);
}

NF_TEST(follow_camera_pulls_in_front_of_what_is_in_the_way) {
    const Vec3 pivot{0.0f, 0.0f, 0.0f};
    const Vec3 desired{0.0f, 0.0f, 5.0f};
    // A wall two metres away: the camera stops a radius short of it.
    const Vec3 pulled = follow_camera_avoid(pivot, desired, 2.0f, 0.35f, 0.7f);
    NF_CHECK_NEAR(pulled.z, 1.65f, kEps);
    // Nothing in the way: the camera stays where it wanted to be.
    const Vec3 clear = follow_camera_avoid(pivot, desired, -1.0f, 0.35f, 0.7f);
    NF_CHECK_NEAR(clear.z, 5.0f, kEps);
}

NF_TEST(follow_camera_never_ends_up_inside_the_target) {
    const Vec3 pivot{0.0f, 0.0f, 0.0f};
    const Vec3 desired{0.0f, 0.0f, 5.0f};
    // A wall right on top of the pivot: the camera still keeps its distance
    // from the pivot, because a camera inside the character's head is worse
    // than one that grazes a wall.
    const Vec3 pulled = follow_camera_avoid(pivot, desired, 0.1f, 0.35f, 0.7f);
    NF_CHECK(pulled.length() >= 0.7f - kEps);
}

// --- the scene format --------------------------------------------------------

NF_TEST(camera_follow_fields_round_trip_through_scene_text) {
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_camera_round_trip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    vfs.mount("content://", tmp);

    Scene scene("CameraFollowRoundTrip");
    ecs::Entity e = scene.world().create_entity();
    scene.world().add<Transform>(e, Transform{});
    CameraComponent cam;
    cam.is_active = true;
    cam.fov_y = 55.0f;
    cam.follow_target = "Player";
    cam.pivot_x = 0.25f;
    cam.pivot_y = 1.75f;
    cam.pivot_z = -0.5f;
    cam.distance = 7.5f;
    cam.yaw = 42.0f;
    cam.pitch = -22.0f;
    cam.smoothing = 12.0f;
    cam.collision_radius = 0.5f;
    cam.avoid_collision = false;
    scene.world().add<CameraComponent>(e, cam);

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/cam.nfscene", scene, err));
    auto result = load_scene_from_vfs(vfs, "content://Scenes/cam.nfscene");
    NF_CHECK(result.success);
    const auto* loaded =
        result.scene->world().get<CameraComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK(loaded->is_active);
        NF_CHECK_NEAR(loaded->fov_y, 55.0f, kEps);
        NF_CHECK(loaded->follow_target == "Player");
        NF_CHECK_NEAR(loaded->pivot_x, 0.25f, kEps);
        NF_CHECK_NEAR(loaded->pivot_y, 1.75f, kEps);
        NF_CHECK_NEAR(loaded->pivot_z, -0.5f, kEps);
        NF_CHECK_NEAR(loaded->distance, 7.5f, kEps);
        NF_CHECK_NEAR(loaded->yaw, 42.0f, kEps);
        NF_CHECK_NEAR(loaded->pitch, -22.0f, kEps);
        NF_CHECK_NEAR(loaded->smoothing, 12.0f, kEps);
        NF_CHECK_NEAR(loaded->collision_radius, 0.5f, kEps);
        NF_CHECK(!loaded->avoid_collision);
    }
    std::filesystem::remove_all(tmp);
}

NF_TEST(a_scene_without_a_follow_camera_still_loads_as_it_did) {
    // A scene written before the follow camera existed has no follow fields on
    // its Camera line. It must load as a camera that looks at the origin --
    // not as a camera that follows an empty name.
    VirtualFileSystem vfs;
    auto tmp = nf::test::temp_root() / "nf_camera_legacy";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
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
        "  Camera: fov=60 aspect=1.777778 near=0.1 far=1000 active=true\n";
    {
        std::filesystem::create_directories(tmp / "Scenes");
        std::ofstream out(tmp / "Scenes" / "legacy.nfscene", std::ios::binary | std::ios::trunc);
        out << text;
    }
    auto result = load_scene_from_physical((tmp / "Scenes" / "legacy.nfscene").string());
    NF_CHECK(result.success);
    const auto* loaded =
        result.scene->world().get<CameraComponent>(result.scene->world().all_entities()[0]);
    NF_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
        NF_CHECK(loaded->follow_target.empty());
        NF_CHECK(loaded->is_active);
    }
    std::filesystem::remove_all(tmp);
}

// --- the runtime -------------------------------------------------------------

NF_TEST(follow_camera_follows_the_named_entity) {
    FollowFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    FollowFixture::SceneSpec spec;
    spec.player = Vec3{3.0f, 0.0f, 7.0f};
    spec.distance = 4.0f;
    f.adopt(spec);

    f.runtime->update(1.0f / 60.0f);
    NF_CHECK(f.runtime->follow_camera_active());

    rendering::Camera out{};
    NF_CHECK(f.runtime->extract_camera(1280, 720, out));
    // Behind the player, at the requested distance, at the pivot's height.
    NF_CHECK_NEAR(out.position.x, 3.0f, 0.05f);
    NF_CHECK_NEAR(out.position.y, 1.5f, 0.05f);
    NF_CHECK_NEAR(out.position.z, 11.0f, 0.05f);
    // And it LOOKS at the pivot, not at the origin.
    NF_CHECK_NEAR(out.target.x, 3.0f, 0.05f);
    NF_CHECK_NEAR(out.target.y, 1.5f, 0.05f);
    NF_CHECK_NEAR(out.target.z, 7.0f, 0.05f);
}

NF_TEST(follow_camera_lags_behind_a_moving_target_and_catches_up) {
    FollowFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    FollowFixture::SceneSpec spec;
    spec.smoothing = 4.0f;
    const ecs::Entity player = f.adopt(spec);
    ecs::World& world = f.world();

    // The first frame places the camera exactly: a camera that eases in from
    // the origin spends the first second of every game flying across the map.
    f.runtime->update(1.0f / 60.0f);
    rendering::Camera first{};
    f.runtime->extract_camera(1280, 720, first);
    NF_CHECK_NEAR(first.position.z, 5.0f, 0.05f);

    // The player moves. The camera follows, but not instantly.
    auto* t = world.get<Transform>(player);
    t->local_z = 10.0f;
    t->dirty = true;
    f.runtime->update(1.0f / 60.0f);
    rendering::Camera second{};
    f.runtime->extract_camera(1280, 720, second);
    NF_CHECK(second.position.z > 5.0f);
    NF_CHECK(second.position.z < 15.0f);

    // Left alone, it converges on the target.
    for (int i = 0; i < 120; ++i) {
        f.runtime->update(1.0f / 60.0f);
    }
    rendering::Camera settled{};
    f.runtime->extract_camera(1280, 720, settled);
    NF_CHECK_NEAR(settled.position.z, 15.0f, 0.2f);
}

NF_TEST(a_camera_that_follows_nothing_still_looks_at_the_origin) {
    // The old behaviour, unchanged for a scene that does not ask for a follow
    // camera. A camera that snapped somewhere new would break every scene
    // that never heard of the follow camera.
    FollowFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    // A camera with no follow target: the old behaviour, unchanged.
    auto scene = std::make_unique<Scene>("PlainCamera");
    ecs::World& world = scene->world();
    ecs::Entity cam = world.create_entity();
    CameraComponent c;
    c.is_active = true;
    world.add<CameraComponent>(cam, c);
    Transform t{};
    t.local_x = 5.0f;
    t.local_y = 3.0f;
    t.local_z = 9.0f;
    world.add<Transform>(cam, t);
    f.runtime->adopt_scene(std::move(scene), "content://Scenes/PlainCamera.nfscene");

    f.runtime->update(1.0f / 60.0f);
    NF_CHECK(!f.runtime->follow_camera_active());
    rendering::Camera out{};
    NF_CHECK(f.runtime->extract_camera(1280, 720, out));
    NF_CHECK_NEAR(out.target.x, 0.0f, kEps);
    NF_CHECK_NEAR(out.target.y, 0.0f, kEps);
    NF_CHECK_NEAR(out.target.z, 0.0f, kEps);
    NF_CHECK_NEAR(out.position.x, 5.0f, 0.05f);
}

NF_TEST(a_follow_camera_whose_target_is_missing_keeps_its_place) {
    // A name that matches nothing must not teleport the camera to the
    // origin: the player would suddenly be looking at empty space.
    FollowFixture f;
    if (!f.ok()) {
        NF_SKIP("no Vulkan device");
    }
    FollowFixture::SceneSpec spec;
    spec.follow = "Nobody";
    f.adopt(spec);

    f.runtime->update(1.0f / 60.0f);
    NF_CHECK(!f.runtime->follow_camera_active());
    rendering::Camera out{};
    NF_CHECK(f.runtime->extract_camera(1280, 720, out));
    NF_CHECK_NEAR(out.target.z, 0.0f, kEps);
}
