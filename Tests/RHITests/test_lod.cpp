// Tests/RHITests/test_lod.cpp — distance LOD selection (Phase 12, W4).
//
// select_lod() is the one function standing between the camera and which
// geometry gets drawn. It is pure (no device, no scene), so its boundaries
// are pinned exactly here; the render integration test next to it proves the
// renderer actually consults it.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <filesystem>
#include <span>
#include <vector>

using namespace nf;
using namespace nf::rendering;
using namespace nf::test;

#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

NF_TEST(select_lod_empty_bands_always_select_zero) {
    // No bands (or one LOD) means no selection: content without LODs renders
    // bit-identical with the policy present or absent.
    NF_CHECK_EQ(select_lod(0.0f, 3, std::span<const float>{}), 0u);
    NF_CHECK_EQ(select_lod(10000.0f, 3, std::span<const float>{}), 0u);
    NF_CHECK_EQ(select_lod(10000.0f, 1, std::span<const float>{{40.0f, 100.0f}}), 0u);
    NF_CHECK_EQ(select_lod(5.0f, 0, std::span<const float>{{40.0f}}), 0u);
}

NF_TEST(select_lod_steps_through_bands_and_clamps) {
    const float bands_arr[] = {40.0f, 100.0f};
    const std::span<const float> bands(bands_arr, 2);
    NF_CHECK_EQ(select_lod(0.0f, 3, bands), 0u);
    NF_CHECK_EQ(select_lod(39.9f, 3, bands), 0u);
    NF_CHECK_EQ(select_lod(40.0f, 3, bands), 0u); // boundary belongs to the nearer band
    NF_CHECK_EQ(select_lod(40.1f, 3, bands), 1u);
    NF_CHECK_EQ(select_lod(100.0f, 3, bands), 1u);
    NF_CHECK_EQ(select_lod(100.1f, 3, bands), 2u);
    NF_CHECK_EQ(select_lod(10000.0f, 3, bands), 2u);
    // Fewer LODs than bands: clamp into range, never out of bounds.
    NF_CHECK_EQ(select_lod(10000.0f, 2, bands), 1u);
    NF_CHECK_EQ(select_lod(-5.0f, 3, bands), 0u);
}

namespace {

// Renders one cube through the real pipeline at the given camera distance
// and counts lit pixels. The mesh carries two LODs: full cube at 0, empty
// at 1 — so LOD selection is visible as light vs dark, not as a subtle
// triangle-count difference no threshold could pin.
u32 render_lod_cube(rhi::IGraphicsDevice& dev, Renderer3D& renderer, MeshLibrary& meshes,
                    StaticMeshHandle mesh, MaterialHandle mat, float camera_z) {
    RenderWorld rw;
    RenderObject ro;
    ro.id = 1;
    ro.visible = true;
    ro.world = Mat4::translate({0.0f, 0.0f, 0.0f});
    ro.mesh_handle = mesh;
    ro.material_handle = mat;
    const StaticMesh* sm = meshes.get(mesh);
    ro.bounds = sm->bounds();
    ro.sphere = sm->bounding_sphere();
    rw.objects.push_back(ro);

    Camera cam{};
    cam.position = {0.0f, 0.0f, camera_z};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.aspect = 1.0f;
    cam.fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
    cam.near_plane = 0.1f;
    cam.far_plane = 1000.0f;
    update_camera(cam);

    rhi::TextureDesc target_desc{};
    target_desc.width = 64;
    target_desc.height = 64;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
    auto target = dev.create_texture(target_desc);
    if (!target) return 0;

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    if (!cmd || !fence) return 0;
    cmd->begin();
    if (!renderer.render(*cmd, rw, cam, *target)) return 0;
    rhi::BufferDesc rb_desc{};
    rb_desc.size = 64u * 64u * 4u;
    rb_desc.usage = rhi::BufferUsage::TransferDst;
    rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = dev.create_buffer(rb_desc);
    if (!rb) return 0;
    cmd->copy_texture_to_buffer(*target, *rb, 0, 0, 64, 64, 0);
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(kGpuTimeoutNs)) return 0;
    dev.wait_idle();

    const auto* px = static_cast<const Pixel*>(rb->map());
    if (px == nullptr) return 0;
    u32 lit = 0;
    for (size_t i = 0; i < 64u * 64u; ++i) {
        if (px[i].r > 10 || px[i].g > 10 || px[i].b > 10) ++lit;
    }
    rb->unmap();
    return lit;
}

} // namespace

NF_TEST(renderer_selects_lod_by_distance) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    std::filesystem::path shader_dir(NF_BASIC3D_SHADER_DIR);
    if (shader_dir.empty() || !std::filesystem::exists(shader_dir)) {
        NF_SKIP("required test asset missing");
    }
    rhi::reset_validation_error_count();

    Renderer3D renderer;
    NF_CHECK(renderer.init(dev, shader_dir, 64, 64));
    renderer.set_lod_max_distances({40.0f});
    MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);

    auto cube = StaticMesh::create_cube(2.0f);
    cube->lods().push_back(MeshLOD{}); // LOD 1: empty — draws nothing
    StaticMeshHandle h = meshes.add(std::move(cube));
    NF_CHECK(meshes.upload_all(dev));

    PBRMaterialParams params{};
    params.base_color[0] = 0.9f;
    params.base_color[1] = 0.1f;
    params.base_color[2] = 0.1f;
    MaterialHandle mat = renderer.materials().create_instance(*renderer.gbuffer_material(), params, "lod");
    NF_CHECK(mat.valid());

    // Near (distance 6 <= 40): full LOD, lit cube.
    NF_CHECK(render_lod_cube(dev, renderer, meshes, h, mat, 6.0f) > 100);
    // Far (distance 60 > 40): empty LOD, nothing drawn — but the frame is
    // still valid (no crash, no validation error, no culled-away confusion:
    // the cube is well inside the frustum and the far plane).
    NF_CHECK_EQ(render_lod_cube(dev, renderer, meshes, h, mat, 60.0f), 0u);

    NF_CHECK(rhi::validation_error_count() == 0);
    dev.wait_idle();
    renderer.shutdown();
    dev.wait_idle();
    rhi::reset_validation_error_count();
}
