// View framing: the orbit pivot, Frame Selection and Frame All.
//
// Before this, the viewport camera orbited the WORLD ORIGIN, hardcoded. That
// made any content away from the origin unreachable — a level authored at
// x = 50 could not be orbited, zoomed to, or framed at all, however the user
// dragged. The pivot is the fix; these tests pin the properties that make it
// usable rather than merely present:
//
//   * framing centres on the SELECTION's real extent, not its origin;
//   * it moves WHERE the camera looks without changing the angle it looks
//     from (framing must not spin the user's view);
//   * it hands back an eye distance that actually puts the object on screen;
//   * the pivot is VIEW state: framing never dirties the scene, never adds an
//     undo step, and never survives into a saved file.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>
#include <vector>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

namespace {

// One 2x2 cube (bounds -1..1 on every axis) placed at (20, 4, -8), plus an
// active camera at a deliberately lopsided angle. The placement is the point:
// (20, 4, -8) is nowhere near the origin, so framing it only works if the
// orbit pivot is real. The camera angle is lopsided so a "reset to a nice
// angle" bug is impossible to miss.
struct FramingScene {
    VirtualFileSystem vfs;
    AssetRegistry reg;
    std::unique_ptr<AssetManager> manager;
    std::unique_ptr<runtime::Runtime> rt;
    std::unique_ptr<editor::ConsoleBuffer> console;
    std::unique_ptr<editor::EditorApp> app;
    std::filesystem::path tmp;

    // The cube, once open().
    ecs::Entity cube = ecs::kInvalidEntity;
    // The active camera, once open().
    ecs::Entity cam = ecs::kInvalidEntity;

    bool build(rhi::IGraphicsDevice& device) {
        tmp = std::filesystem::temp_directory_path() / "nf_ed_framing";
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp / "Content" / "Scenes");
        std::filesystem::create_directories(tmp / "Cache" / "Meshes");
        vfs.mount("content://", tmp / "Content");
        vfs.mount("cache://", tmp / "Cache");

        auto mesh = rendering::StaticMesh::create_cube(2.0f);
        const AssetId mesh_id = AssetId::generate();
        auto asset =
            rendering::make_mesh_asset(*mesh, mesh_id, "content://Meshes/cube.nfmesh");
        std::vector<uint8_t> bytes;
        asset->save_to_bytes(bytes);
        (void)vfs.write_bytes("cache://Meshes/cube.nfmesh", std::span<const uint8_t>(bytes));
        AssetMetadata meta;
        meta.id = mesh_id;
        meta.type = AssetType::Mesh;
        meta.logical_path = "content://Meshes/cube.nfmesh";
        meta.cooked_path = "cache://Meshes/cube.nfmesh";
        std::string err;
        if (!reg.add(meta, err)) {
            return false;
        }
        manager = std::make_unique<AssetManager>(vfs, reg);
        auto handle = manager->load_mesh_sync(mesh_id);
        if (!handle || handle->state != AssetState::Ready) {
            err = "mesh sync load failed";
            return false;
        }
        manager->update();

        scene::Scene scene("Framing");
        auto& w = scene.world();
        // See test_viewport_drag's fixture for why entity id 0 is reserved.
        ecs::Entity placeholder = w.create_entity();
        w.add<scene::NameComponent>(placeholder, scene::NameComponent{"Placeholder"});

        cam = w.create_entity();
        scene::Transform ct;
        ct.local_x = 6.0f;
        ct.local_y = 1.5f;
        ct.local_z = 7.0f;
        w.add<scene::Transform>(cam, ct);
        runtime::CameraComponent cc;
        cc.is_active = true;
        w.add<runtime::CameraComponent>(cam, cc);

        cube = w.create_entity();
        scene::Transform t;
        t.local_x = 20.0f;
        t.local_y = 4.0f;
        t.local_z = -8.0f;
        w.add<scene::Transform>(cube, t);
        runtime::MeshComponent mc;
        mc.mesh_id = mesh_id;
        mc.material = "content://Materials/Default";
        w.add<runtime::MeshComponent>(cube, mc);

        if (!runtime::save_scene_to_vfs(vfs, "content://Scenes/Framing.nfscene", scene, err)) {
            return false;
        }

        rt = std::make_unique<runtime::Runtime>(vfs, reg, *manager, device, nullptr);
        console = std::make_unique<editor::ConsoleBuffer>();
        app = std::make_unique<editor::EditorApp>(vfs, reg, *manager, *console);
        app->attach_runtime(rt.get());
        if (!app->open_scene("content://Scenes/Framing.nfscene", err)) {
            return false;
        }
        scene::propagate_transforms(*app->world());
        // Re-resolve: open_scene rebuilds the world, so the handles above are
        // stale indices into a different world.
        cube = ecs::kInvalidEntity;
        cam = ecs::kInvalidEntity;
        for (ecs::Entity e : app->world()->all_entities()) {
            if (app->world()->has<runtime::MeshComponent>(e)) {
                cube = e;
            }
            const auto* c = app->world()->get<runtime::CameraComponent>(e);
            if (c != nullptr && c->is_active) {
                cam = e;
            }
        }
        return cube.valid() && cam.valid();
    }

    // The eye, relative to a point, in world space.
    void eye_relative_to(const float* p, float& ex, float& ey, float& ez) const {
        const auto* t = app->world()->get<scene::Transform>(cam);
        ex = (t != nullptr ? t->world_x : 0.0f) - p[0];
        ey = (t != nullptr ? t->world_y : 0.0f) - p[1];
        ez = (t != nullptr ? t->world_z : 0.0f) - p[2];
    }

    void teardown(rhi::IGraphicsDevice& device) {
        if (rt) {
            rt->shutdown();
        }
        if (manager) {
            manager->clear();
        }
        device.wait_idle();
        rt.reset();
        app.reset();
        manager.reset();
        console.reset();
        std::filesystem::remove_all(tmp);
    }
};

} // namespace

NF_TEST(fit_distance_puts_a_sphere_of_that_radius_on_screen) {
    // The whole point of the aiming rule: at fit_distance_for(r, fov), a
    // sphere of radius r subtends strictly less than the vertical fov, so the
    // object fits with room to spare at any fov.
    for (const float fov : {30.0f, 60.0f, 90.0f, 120.0f}) {
        for (const float r : {0.05f, 1.0f, 7.5f, 200.0f}) {
            const float d = editor::EditorApp::fit_distance_for(r, fov);
            NF_CHECK(d > r); // closer than the radius and half the object clips
            const float half = fov * 0.5f * 3.14159265359f / 180.0f;
            NF_CHECK(std::asin(std::min(1.0f, r / d)) < half);
        }
    }
    // Monotonic in radius: a bigger object backs the camera off further, so
    // Frame All on a large scene does not crop it.
    NF_CHECK(editor::EditorApp::fit_distance_for(10.0f, 60.0f) >
             editor::EditorApp::fit_distance_for(1.0f, 60.0f));
    // A degenerate fov must not divide by ~zero into an infinity.
    NF_CHECK(std::isfinite(editor::EditorApp::fit_distance_for(1.0f, 0.0f)));
    // A zero-radius target still gets a usable, finite distance — framing a
    // point entity must not put the eye inside it.
    NF_CHECK(editor::EditorApp::fit_distance_for(0.0f, 60.0f) >= 0.0f);
}

NF_TEST(frame_selection_centres_on_the_selection_extent) {
    const GpuFixture& gf = require_gpu();
    auto& device = *gf.device;
    rhi::reset_validation_error_count();

    FramingScene fx;
    NF_CHECK(fx.build(device));

    fx.app->selection().set_single(fx.cube);
    std::string err;
    NF_CHECK(fx.app->frame_selection(err));
    const float* pv = fx.app->view_pivot();
    // The cube is 2x2x2 centred on (20, 4, -8): bounds 19..21, 3..5, -9..-7,
    // so the centre is exactly the transform's position. Landing there is the
    // assertion that the origin-orbiting bug is fixed.
    NF_CHECK_NEAR(pv[0], 20.0f, 1e-3f);
    NF_CHECK_NEAR(pv[1], 4.0f, 1e-3f);
    NF_CHECK_NEAR(pv[2], -8.0f, 1e-3f);

    // The framed radius is the box's circumsphere: half-extent 1 on each axis.
    const float fit = fx.app->consume_view_fit();
    NF_CHECK_NEAR(fit, std::sqrt(3.0f), 1e-3f);
    // And it is a ONE-SHOT: the next frame must not re-fit, or the camera
    // would be pinned at the framed distance and could never be zoomed out.
    NF_CHECK_NEAR(fx.app->consume_view_fit(), 0.0f, 1e-6f);

    fx.teardown(device);
    rhi::reset_validation_error_count();
}

NF_TEST(frame_selection_preserves_the_view_angle) {
    const GpuFixture& gf = require_gpu();
    auto& device = *gf.device;
    rhi::reset_validation_error_count();

    FramingScene fx;
    NF_CHECK(fx.build(device));

    fx.app->selection().set_single(fx.cube);
    float bx, by, bz;
    fx.eye_relative_to(fx.app->view_pivot(), bx, by, bz);
    NF_CHECK(std::sqrt(bx * bx + by * by + bz * bz) > 1e-3f);

    std::string err;
    NF_CHECK(fx.app->frame_selection(err));

    // The eye must have travelled by exactly the pivot delta, so the
    // eye-relative-to-pivot vector is untouched. Framing that also normalized
    // the angle would silently re-aim the user's camera every time they hit
    // the button — the classic "Frame spun my view" bug.
    float ax, ay, az;
    fx.eye_relative_to(fx.app->view_pivot(), ax, ay, az);
    NF_CHECK_NEAR(ax, bx, 1e-3f);
    NF_CHECK_NEAR(ay, by, 1e-3f);
    NF_CHECK_NEAR(az, bz, 1e-3f);

    fx.teardown(device);
    rhi::reset_validation_error_count();
}

NF_TEST(frame_all_covers_everything_and_frame_selection_needs_a_selection) {
    const GpuFixture& gf = require_gpu();
    auto& device = *gf.device;
    rhi::reset_validation_error_count();

    FramingScene fx;
    NF_CHECK(fx.build(device));

    std::string err;
    // Nothing selected: refused, and with a reason (a toolbar button that
    // silently no-ops is indistinguishable from a broken one).
    fx.app->selection().clear();
    NF_CHECK(!fx.app->frame_selection(err));
    NF_CHECK(!err.empty());

    // Frame All works with no selection at all — that is its job: it is the
    // "I have lost the level" escape hatch.
    NF_CHECK(fx.app->frame_all(err));
    const float* pv = fx.app->view_pivot();
    NF_CHECK_NEAR(pv[0], 20.0f, 1e-3f);
    NF_CHECK_NEAR(pv[1], 4.0f, 1e-3f);
    NF_CHECK_NEAR(pv[2], -8.0f, 1e-3f);

    fx.teardown(device);
    rhi::reset_validation_error_count();
}

NF_TEST(framing_is_view_state_and_never_dirties_the_scene) {
    const GpuFixture& gf = require_gpu();
    auto& device = *gf.device;
    rhi::reset_validation_error_count();

    FramingScene fx;
    NF_CHECK(fx.build(device));

    fx.app->selection().set_single(fx.cube);
    // Settle: open_scene may legitimately have dirtied things.
    std::string serr;
    NF_CHECK(fx.app->save(serr));
    const std::size_t undo_before = fx.app->stack().undo_size();
    NF_CHECK(!fx.app->dirty());

    std::string err;
    NF_CHECK(fx.app->frame_selection(err));
    NF_CHECK(fx.app->frame_all(err));
    (void)fx.app->consume_view_fit();

    // Flying the view is not editing: no undo entry, no dirty flag, so the
    // user is never prompted to save a scene they only looked around in.
    NF_CHECK_EQ(fx.app->stack().undo_size(), undo_before);
    NF_CHECK(!fx.app->dirty());

    fx.teardown(device);
    rhi::reset_validation_error_count();
}

NF_TEST(world_bounds_follows_rotation_and_scale) {
    // The bounds are now the mesh's box through the FULL world matrix, because
    // this is the same box the renderer draws and the same one the pick ray
    // tests. Translating only meant a scaled object was clickable in a 1x1 box
    // at its origin, and Frame centred on that phantom cube instead of on the
    // thing filling the screen.
    const GpuFixture& gf = require_gpu();
    auto& device = *gf.device;
    rhi::reset_validation_error_count();

    FramingScene fx;
    NF_CHECK(fx.build(device));

    // Scale the cube 10x on X and turn it 90 degrees on Y. The mesh's -1..1 box
    // stretches to -10..10 along its own X, which the rotation then lays along
    // world Z; the object sits at (20, 4, -8), so world Z spans -18..2. The X
    // extent reverts to the unscaled 1, because the rotation moved the long
    // axis OFF X.
    auto* t = fx.app->world()->get<scene::Transform>(fx.cube);
    NF_CHECK(t != nullptr);
    t->scale_x = 10.0f;
    t->rot_y = 90.0f;
    t->dirty = true;
    scene::propagate_transforms(*fx.app->world());

    editor::AABB b;
    NF_CHECK(fx.app->world_bounds(fx.cube, b));
    NF_CHECK_NEAR(b.min_z, -18.0f, 1e-3f);
    NF_CHECK_NEAR(b.max_z, 2.0f, 1e-3f);
    NF_CHECK_NEAR(b.min_x, 19.0f, 1e-3f);
    NF_CHECK_NEAR(b.max_x, 21.0f, 1e-3f);
    // Y is untouched by a Y rotation and by an X scale.
    NF_CHECK_NEAR(b.min_y, 3.0f, 1e-3f);
    NF_CHECK_NEAR(b.max_y, 5.0f, 1e-3f);

    // And Frame lands on the object's real centre.
    fx.app->selection().set_single(fx.cube);
    std::string err;
    NF_CHECK(fx.app->frame_selection(err));
    NF_CHECK_NEAR(fx.app->view_pivot()[0], 20.0f, 1e-3f);
    NF_CHECK_NEAR(fx.app->view_pivot()[2], -8.0f, 1e-3f);
    // The framed radius is the circumsphere of the SCALED box: half-extents
    // 1, 1, 10.
    const float fit = fx.app->consume_view_fit();
    NF_CHECK_NEAR(fit, std::sqrt(1.0f + 1.0f + 100.0f), 1e-2f);

    fx.teardown(device);
    rhi::reset_validation_error_count();
}

NF_TEST(world_bounds_is_the_box_framing_and_picking_both_use) {
    // world_bounds() is what frame_selection() centres on AND what the pick
    // ray hits. If those two ever disagree, the user frames a box the click
    // test does not use, and "Frame" appears to miss.
    const GpuFixture& gf = require_gpu();
    auto& device = *gf.device;
    rhi::reset_validation_error_count();

    FramingScene fx;
    NF_CHECK(fx.build(device));

    editor::AABB b;
    NF_CHECK(fx.app->world_bounds(fx.cube, b));
    NF_CHECK_NEAR(b.min_x, 19.0f, 1e-3f);
    NF_CHECK_NEAR(b.max_x, 21.0f, 1e-3f);
    NF_CHECK_NEAR(b.min_y, 3.0f, 1e-3f);
    NF_CHECK_NEAR(b.max_y, 5.0f, 1e-3f);
    NF_CHECK_NEAR(b.min_z, -9.0f, 1e-3f);
    NF_CHECK_NEAR(b.max_z, -7.0f, 1e-3f);

    // The scene bounds match, since the cube is the only mesh.
    editor::AABB sb;
    NF_CHECK(fx.app->scene_bounds(sb));
    NF_CHECK_NEAR(sb.min_x, b.min_x, 1e-3f);
    NF_CHECK_NEAR(sb.max_z, b.max_z, 1e-3f);

    // A camera has no mesh: no bounds, and a frame over a camera-only
    // selection falls back to its origin rather than failing.
    NF_CHECK(!fx.app->world_bounds(fx.cam, b));
    fx.app->selection().clear();
    fx.app->selection().add(fx.cam);
    std::string err;
    NF_CHECK(fx.app->frame_selection(err));
    NF_CHECK_NEAR(fx.app->view_pivot()[0], 6.0f, 1e-3f);
    NF_CHECK_NEAR(fx.app->consume_view_fit(), 0.0f, 1e-6f); // nothing to fit

    fx.teardown(device);
    rhi::reset_validation_error_count();
}
