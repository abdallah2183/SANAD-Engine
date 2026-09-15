// GPU picking — the id pass must report what the user actually sees.
//
// The CPU ray/AABB path cannot express occlusion or per-pixel coverage: it
// picks against world-space bounds, so it selects the wrong object when one
// surface hides another. These tests exist to pin down that difference.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Rendering/GpuPicker.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::rendering;

namespace {

constexpr u32 kW = 64;
constexpr u32 kH = 64;

std::filesystem::path pick_shader_dir() {
#ifdef NF_BASIC3D_SHADER_DIR
    return std::filesystem::path(NF_BASIC3D_SHADER_DIR);
#else
    return {};
#endif
}

Camera make_pick_camera(float z) {
    Camera cam{};
    cam.position = {0.0f, 0.0f, z};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.aspect = static_cast<float>(kW) / static_cast<float>(kH);
    cam.fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
    cam.near_plane = 0.1f;
    cam.far_plane = 100.0f;
    update_camera(cam);
    return cam;
}

void set_translation(RenderObject& ro, float x, float y, float z) {
    ro.world = scene::compose_trs_mat4(x, y, z, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
}

} // namespace

// The id encoding is the one part of picking that needs no GPU, and an
// off-by-one here would silently select a neighbouring entity while every
// pixel-level test still passed.
NF_TEST(pick_id_encoding_round_trips) {
    float rgb[3]{};
    const u32 ids[] = {0u, 1u, 2u, 254u, 255u, 256u, 65535u, 65536u, 0x00FFFFFEu};
    for (u32 id : ids) {
        pack_pick_id(id, rgb);
        const auto r = static_cast<u8>(std::lround(rgb[0] * 255.0f));
        const auto g = static_cast<u8>(std::lround(rgb[1] * 255.0f));
        const auto b = static_cast<u8>(std::lround(rgb[2] * 255.0f));
        NF_CHECK_EQ(unpack_pick_id(r, g, b, 255), id);
    }

    // A cleared pixel means "nothing here" — never entity 0.
    NF_CHECK_EQ(unpack_pick_id(0, 0, 0, 0), 0u);

    // Out-of-range ids saturate instead of wrapping, so they cannot alias a
    // real entity.
    pack_pick_id(0xFFFFFFFFu, rgb);
    NF_CHECK(rgb[0] > 0.0f || rgb[1] > 0.0f || rgb[2] > 0.0f);
    const auto r = static_cast<u8>(std::lround(rgb[0] * 255.0f));
    const auto g = static_cast<u8>(std::lround(rgb[1] * 255.0f));
    const auto b = static_cast<u8>(std::lround(rgb[2] * 255.0f));
    NF_CHECK_EQ(unpack_pick_id(r, g, b, 255), 0x00FFFFFEu);
}

NF_TEST(gpu_pick_hits_the_object_under_the_cursor) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    GpuPicker picker;
    NF_CHECK(picker.init(device, pick_shader_dir(), kW, kH));
    NF_CHECK(picker.ready());

    MeshLibrary meshes;
    auto cube = StaticMesh::create_cube(2.0f);
    StaticMeshHandle h = meshes.add(std::move(cube));
    meshes.upload_all(device);

    RenderWorld world{};
    RenderObject ro{};
    ro.id = 77;
    ro.visible = true;
    ro.mesh_handle = h;
    ro.lod = 0;
    set_translation(ro, 0.0f, 0.0f, 0.0f);
    world.objects.push_back(ro);

    const Camera cam = make_pick_camera(5.0f);

    // Centre of the screen: the cube is there.
    auto cmd = device.create_command_buffer();
    NF_CHECK(cmd != nullptr);
    const PickHit centre = picker.pick(*cmd, world.objects, cam, meshes, kW / 2, kH / 2);
    NF_CHECK(centre.hit);
    NF_CHECK_EQ(centre.object_id, 77u);

    // Far corner: background, so no hit. A picker that reported a hit here
    // would be ignoring coverage entirely.
    auto cmd2 = device.create_command_buffer();
    const PickHit corner = picker.pick(*cmd2, world.objects, cam, meshes, 1, 1);
    NF_CHECK(!corner.hit);

    // Out-of-range coordinates are rejected, not read out of bounds.
    auto cmd3 = device.create_command_buffer();
    NF_CHECK(!picker.pick(*cmd3, world.objects, cam, meshes, kW, kH).hit);

    device.wait_idle();
    picker.shutdown();
    device.wait_idle();
    NF_CHECK(rhi::validation_error_count() == 0);
    rhi::reset_validation_error_count();
}

// The reason GPU picking exists: two objects project onto the same pixel, and
// the one in front must win. The CPU AABB path returns whichever its ray hits
// first along its own ordering and cannot distinguish this reliably.
NF_TEST(gpu_pick_prefers_the_nearer_surface) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    GpuPicker picker;
    NF_CHECK(picker.init(device, pick_shader_dir(), kW, kH));

    MeshLibrary meshes;
    auto cube = StaticMesh::create_cube(2.0f);
    StaticMeshHandle h = meshes.add(std::move(cube));
    meshes.upload_all(device);

    RenderWorld world{};
    // Far object listed FIRST: if the picker ignored depth, draw order would
    // make the far cube win and this test would catch it.
    RenderObject far_obj{};
    far_obj.id = 202;
    far_obj.visible = true;
    far_obj.mesh_handle = h;
    far_obj.lod = 0;
    set_translation(far_obj, 0.0f, 0.0f, -1.5f);
    world.objects.push_back(far_obj);

    RenderObject near_obj{};
    near_obj.id = 101;
    near_obj.visible = true;
    near_obj.mesh_handle = h;
    near_obj.lod = 0;
    set_translation(near_obj, 0.0f, 0.0f, 1.5f);
    world.objects.push_back(near_obj);

    const Camera cam = make_pick_camera(5.0f);
    auto cmd = device.create_command_buffer();
    const PickHit hit = picker.pick(*cmd, world.objects, cam, meshes, kW / 2, kH / 2);
    NF_CHECK(hit.hit);
    NF_CHECK_EQ(hit.object_id, 101u); // the near one, not the first-listed one

    device.wait_idle();
    picker.shutdown();
    device.wait_idle();
    NF_CHECK(rhi::validation_error_count() == 0);
    rhi::reset_validation_error_count();
}

// Invisible objects must not be pickable: the id pass draws what the frame
// draws, and a culled object is not on screen to click.
NF_TEST(gpu_pick_ignores_invisible_objects) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    GpuPicker picker;
    NF_CHECK(picker.init(device, pick_shader_dir(), kW, kH));

    MeshLibrary meshes;
    auto cube = StaticMesh::create_cube(2.0f);
    StaticMeshHandle h = meshes.add(std::move(cube));
    meshes.upload_all(device);

    RenderWorld world{};
    RenderObject ro{};
    ro.id = 55;
    ro.visible = false; // present in the world, but not drawn
    ro.mesh_handle = h;
    ro.lod = 0;
    set_translation(ro, 0.0f, 0.0f, 0.0f);
    world.objects.push_back(ro);

    const Camera cam = make_pick_camera(5.0f);
    auto cmd = device.create_command_buffer();
    NF_CHECK(!picker.pick(*cmd, world.objects, cam, meshes, kW / 2, kH / 2).hit);

    device.wait_idle();
    picker.shutdown();
    device.wait_idle();
    NF_CHECK(rhi::validation_error_count() == 0);
    rhi::reset_validation_error_count();
}
