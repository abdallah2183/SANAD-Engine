// Tests/RHITests/test_3d_renderer.cpp — Basic 3D Renderer milestone tests
//
// Test philosophy: nothing passes because "it compiled" or "the draw call
// succeeded". Every renderer test proves its claim from the GPU's output —
// CPU readback + pixel verification, or the strongest available invariant
// (validation-clean runs, alive-object counters) where pixels don't apply.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshAsset.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/ResourceState.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <array>
#include <cmath>
#include <filesystem>

namespace {

using nf::u32;
using nf::u64;
using nf::u8;
using nf::usize;
using namespace nf::test;
using namespace nf::rendering;
using namespace nf::ecs;
// Qualified-name aliases: using-directives import members but not the
// namespace names themselves, and render_full_chain spells ecs::/scene:: out.
namespace ecs = nf::ecs;
namespace scene = nf::scene;
namespace rhi = nf::rhi;
namespace fs = std::filesystem;

#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

fs::path basic3d_shader_dir() {
    fs::path p(NF_BASIC3D_SHADER_DIR);
    if (!p.empty() && fs::exists(p)) return p;
    return {};
}

// A camera looking at the origin from +Z.
Camera make_camera(float z = 3.0f, float aspect = 1.0f) {
    Camera cam{};
    cam.position = {0, 0, z};
    cam.target = {0, 0, 0};
    cam.up = {0, 1, 0};
    cam.aspect = aspect;
    cam.fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
    cam.near_plane = 0.1f;
    cam.far_plane = 100.0f;
    update_camera(cam);
    return cam;
}

/// Canonical one-frame render through the full pipeline:
///   ECS world → extract → cull → Depth → GBuffer → Lighting → Tonemap → readback.
/// `setup` builds the scene (entities, meshes, materials, lights) before the
/// frame is extracted. Returns false when the GPU or shaders are unavailable.
bool render_full_chain(rhi::IGraphicsDevice& dev, u32 width, u32 height,
                       const std::function<void(ecs::World&, MeshLibrary&, Renderer3D&)>& setup,
                       std::vector<Pixel>& out_pixels, Renderer3D::Stats* out_stats = nullptr,
                       const Camera* camera_override = nullptr) {
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    if (!renderer.init(dev, basic3d_shader_dir(), width, height)) return false;
    MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);

    ecs::World world;
    const Camera fallback_cam = make_camera(3.0f, float(width) / float(height));
    const Camera& camera = camera_override ? *camera_override : fallback_cam;
    setup(world, meshes, renderer);

    scene::propagate_transforms(world);
    RenderWorld render_world;
    nf::runtime::extract_render_objects(world, meshes, render_world);

    rhi::TextureDesc target_desc{};
    target_desc.width = width;
    target_desc.height = height;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    auto target = dev.create_texture(target_desc);
    if (!target) return false;

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    if (!cmd || !fence) return false;

    cmd->begin();
    if (!renderer.render(*cmd, render_world, camera, *target)) return false;
    rhi::BufferDesc rb_desc{};
    rb_desc.size = usize(width) * height * 4;
    rb_desc.usage = rhi::BufferUsage::TransferDst;
    rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = dev.create_buffer(rb_desc);
    if (!rb) return false;
    cmd->copy_texture_to_buffer(*target, *rb, 0, 0, width, height, 0);
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(kGpuTimeoutNs)) return false;
    dev.wait_idle();

    auto* px = static_cast<Pixel*>(rb->map());
    if (!px) return false;
    out_pixels.assign(px, px + usize(width) * height);
    rb->unmap();

    if (out_stats) *out_stats = renderer.last_stats();
    renderer.shutdown();
    return true;
}

u32 count_pixels_above(const std::vector<Pixel>& px, u8 threshold) {
    u32 n = 0;
    for (const Pixel& p : px) {
        if (p.r > threshold || p.g > threshold || p.b > threshold) ++n;
    }
    return n;
}

/// Number of pixels that differ by more than `tolerance` in any channel.
u32 count_different_pixels(const std::vector<Pixel>& a, const std::vector<Pixel>& b, u8 tolerance = 4) {
    NF_CHECK_EQ(a.size(), b.size());
    u32 n = 0;
    for (usize i = 0; i < a.size(); ++i) {
        int dr = std::abs(int(a[i].r) - int(b[i].r));
        int dg = std::abs(int(a[i].g) - int(b[i].g));
        int db = std::abs(int(a[i].b) - int(b[i].b));
        if (dr > tolerance || dg > tolerance || db > tolerance) ++n;
    }
    return n;
}

void set_rgb(PBRMaterialParams& p, float r, float g, float b) {
    p.base_color[0] = r;
    p.base_color[1] = g;
    p.base_color[2] = b;
}

u64 pixel_sum(const std::vector<Pixel>& px) {
    u64 s = 0;
    for (const Pixel& p : px) s += u64(p.r) + p.g + p.b;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Static mesh foundation
// ---------------------------------------------------------------------------

NF_TEST(mesh_asset_creation) {
    auto cube = StaticMesh::create_cube(1.0f);
    NF_CHECK(cube != nullptr);
    NF_CHECK(cube->lods().size() >= 1);
    NF_CHECK(cube->lods()[0].vertices.size() == 24);
    NF_CHECK(cube->lods()[0].indices.size() == 36);
    NF_CHECK(cube->lods()[0].material_slots.size() >= 1);

    const AABB& b = cube->bounds();
    NF_CHECK_NEAR(b.min_x, -0.5f, 1e-5f);
    NF_CHECK_NEAR(b.max_x, 0.5f, 1e-5f);
    NF_CHECK_NEAR(b.min_z, -0.5f, 1e-5f);
    NF_CHECK_NEAR(cube->bounding_sphere().radius, 0.8660254f, 1e-4f);

    // Vertex format carries Position / Normal / Tangent / UV0 (+UV1 slot)
    const Vertex& v = cube->lods()[0].vertices[0];
    NF_CHECK(v.normal[0] != 0.0f || v.normal[1] != 0.0f || v.normal[2] != 0.0f);
    NF_CHECK(v.tangent[0] != 0.0f);
}

NF_TEST(mesh_vertex_upload) {
    const GpuFixture& f = require_gpu();
    auto quad = StaticMesh::create_quad(1.0f);
    NF_CHECK(quad->upload(*f.device));
    NF_CHECK(quad->is_uploaded());
    NF_CHECK(quad->vertex_buffer(0) != nullptr);
    NF_CHECK(quad->vertex_buffer(0)->size() == quad->lods()[0].vertices.size() * sizeof(Vertex));
}

NF_TEST(mesh_index_upload) {
    const GpuFixture& f = require_gpu();
    auto cube = StaticMesh::create_cube(1.0f);
    NF_CHECK(cube->upload(*f.device));
    NF_CHECK(cube->index_buffer(0) != nullptr);
    NF_CHECK(cube->index_buffer(0)->size() == cube->lods()[0].indices.size() * sizeof(u32));
}

NF_TEST(mesh_submesh_layout) {
    auto cube = StaticMesh::create_cube(1.0f);
    auto sphere = StaticMesh::create_sphere(0.5f, 8);
    NF_CHECK(cube->lods()[0].submeshes.size() >= 1);
    const SubMesh& sm = cube->lods()[0].submeshes[0];
    NF_CHECK_EQ(sm.index_offset, 0u);
    NF_CHECK_EQ(sm.index_count, 36u);
    NF_CHECK_EQ(sm.vertex_count, 24u);
    NF_CHECK_EQ(sm.material_slot, 0u);
    NF_CHECK(sm.bounds.min_x < sm.bounds.max_x);
    // Sphere exercises a mesh with more than one face's worth of submesh data
    NF_CHECK(sphere->lods()[0].indices.size() > 36);
}

NF_TEST(mesh_asset_cook_import) {
    // Mesh Asset → Cook (save) → Import (load) → Runtime Mesh — byte-exact round trip
    auto cube = StaticMesh::create_cube(1.0f);
    cube->set_name("cooked_cube");

    const fs::path tmp = fs::temp_directory_path() / "nfmesh_roundtrip.nfmesh";
    NF_CHECK(mesh_asset::save_mesh_asset(*cube, tmp));

    auto loaded = mesh_asset::load_mesh_asset(tmp);
    NF_CHECK(loaded != nullptr);
    NF_CHECK_EQ(loaded->name(), std::string("cooked_cube"));
    NF_CHECK_EQ(loaded->lods().size(), cube->lods().size());
    NF_CHECK_EQ(loaded->lods()[0].vertices.size(), cube->lods()[0].vertices.size());
    NF_CHECK_EQ(loaded->lods()[0].indices.size(), cube->lods()[0].indices.size());
    NF_CHECK_EQ(loaded->lods()[0].submeshes.size(), cube->lods()[0].submeshes.size());
    for (usize i = 0; i < cube->lods()[0].vertices.size(); ++i) {
        const Vertex& a = cube->lods()[0].vertices[i];
        const Vertex& b = loaded->lods()[0].vertices[i];
        NF_CHECK(std::memcmp(a.position, b.position, sizeof(a.position)) == 0);
        NF_CHECK(std::memcmp(a.normal, b.normal, sizeof(a.normal)) == 0);
        NF_CHECK(std::memcmp(a.uv0, b.uv0, sizeof(a.uv0)) == 0);
    }
    const AABB loaded_bounds = loaded->bounds();
    const AABB cube_bounds = cube->bounds();
    NF_CHECK(std::memcmp(&loaded_bounds, &cube_bounds, sizeof(AABB)) == 0);
    // The GPU half of this round trip lives in
    // mesh_asset_cook_import_gpu_upload below, so it can be reported SKIPPED
    // on a GPU-less runner instead of being silently skipped inside this test.
    std::error_code ec;
    fs::remove(tmp, ec);
}

NF_TEST(mesh_asset_cook_import_gpu_upload) {
    // Split from mesh_asset_cook_import so the GPU upload is a first-class
    // result: a bare `if (f.available) { ... }` around these assertions made
    // them no-op while the test still reported PASS.
    const GpuFixture& f = require_gpu();

    auto cube = StaticMesh::create_cube(1.0f);
    cube->set_name("cooked_cube_gpu");

    const fs::path tmp = fs::temp_directory_path() / "nfmesh_roundtrip_gpu.nfmesh";
    NF_CHECK(mesh_asset::save_mesh_asset(*cube, tmp));

    auto loaded = mesh_asset::load_mesh_asset(tmp);
    NF_CHECK(loaded != nullptr);
    NF_CHECK(loaded->upload(*f.device));
    NF_CHECK(loaded->vertex_buffer(0) != nullptr);

    std::error_code ec;
    fs::remove(tmp, ec);
}

// ---------------------------------------------------------------------------
// Camera + frustum
// ---------------------------------------------------------------------------

NF_TEST(camera_view_projection) {
    Camera cam = make_camera(5.0f, 16.0f / 9.0f);
    // A point straight ahead of the camera must project near NDC center
    auto project = [&](float x, float y, float z) {
        const Mat4& vp = cam.view_projection;
        float clip[4] = {
            vp.m[0]*x + vp.m[4]*y + vp.m[8]*z + vp.m[12],
            vp.m[1]*x + vp.m[5]*y + vp.m[9]*z + vp.m[13],
            vp.m[2]*x + vp.m[6]*y + vp.m[10]*z + vp.m[14],
            vp.m[3]*x + vp.m[7]*y + vp.m[11]*z + vp.m[15],
        };
        return Vec3{clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
    };
    const Vec3 ndc = project(0, 0, 0); // origin: dead ahead of (0,0,5)
    NF_CHECK_NEAR(ndc.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(ndc.y, 0.0f, 1e-4f);
    const Vec3 ndc2 = project(0, 0, 4); // one unit closer to the camera
    NF_CHECK_NEAR(ndc2.x, 0.0f, 1e-4f);
    NF_CHECK(ndc.z > ndc2.z); // origin sits farther along NDC depth
}

NF_TEST(camera_frustum) {
    // Perspective: frustum planes reject behind-camera geometry
    Camera persp = make_camera(5.0f);
    AABB in_front{-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
    AABB behind_origin{-1.5f, -0.5f, 5.5f, -0.5f, 0.5f, 6.5f}; // entirely behind the eye at z=5
    NF_CHECK(is_visible(in_front, persp.frustum));
    NF_CHECK(!is_visible(behind_origin, persp.frustum));

    // Orthographic: same geometry, parallel projection
    Camera ortho{};
    ortho.type = Camera::ProjectionType::Orthographic;
    ortho.ortho_width = 10.0f;
    ortho.ortho_height = 10.0f;
    ortho.position = {0, 0, 5};
    ortho.target = {0, 0, 0};
    ortho.near_plane = 0.1f;
    ortho.far_plane = 100.0f;
    update_camera(ortho);
    NF_CHECK(is_visible(in_front, ortho.frustum));
    AABB ortho_far_away{50, 50, 50, 51, 51, 51};
    NF_CHECK(!is_visible(ortho_far_away, ortho.frustum));

    // Mat4 inverse round-trips (foundation for depth reconstruction)
    const Mat4 inv = persp.view_projection.inverse();
    const Mat4 round = persp.view_projection * inv;
    for (int i = 0; i < 16; ++i) {
        const float expect = (i % 5 == 0) ? 1.0f : 0.0f;
        NF_CHECK_NEAR(round.m[i], expect, 1e-3f);
    }
}

NF_TEST(frustum_contains_object) {
    Camera cam = make_camera(5.0f);
    AABB inside{-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
    AABB outside{100, 100, 100, 101, 101, 101};
    NF_CHECK(is_visible(inside, cam.frustum));
    NF_CHECK(!is_visible(outside, cam.frustum));
    BoundingSphere s_inside{0, 0, 0, 0.5f};
    BoundingSphere s_outside{100, 100, 100, 0.5f};
    NF_CHECK(is_visible(s_inside, cam.frustum));
    NF_CHECK(!is_visible(s_outside, cam.frustum));
}

NF_TEST(frustum_culls_object) {
    World world;
    Camera cam = make_camera(5.0f);
    auto cube = StaticMesh::create_cube(1.0f);
    {
        Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        world.add<MeshComponent>(e, MeshComponent{StaticMeshHandle{0}, kInvalidMaterialHandle, true});
        AABB aabb = transform_aabb(cube->bounds(), 0, 0, 0);
        NF_CHECK(is_visible(aabb, cam.frustum));
    }
    {
        Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        auto* t = world.get<scene::Transform>(e);
        t->world_x = 100;
        AABB aabb = transform_aabb(cube->bounds(), 100, 0, 0);
        NF_CHECK(!is_visible(aabb, cam.frustum));
    }
}

// ---------------------------------------------------------------------------
// Extraction: the hard boundary between game world and render world
// ---------------------------------------------------------------------------

NF_TEST(static_mesh_extraction) {
    World world;
    MeshLibrary meshes;
    auto cube = StaticMesh::create_cube(1.0f);
    auto sphere = StaticMesh::create_sphere(0.5f, 8);
    StaticMeshHandle h_cube = meshes.add(std::move(cube));
    StaticMeshHandle h_sphere = meshes.add(std::move(sphere));

    Entity e1 = world.create_entity();
    world.add<scene::Transform>(e1, scene::Transform{});
    world.get<scene::Transform>(e1)->local_x = 1;
    world.add<MeshComponent>(e1, MeshComponent{h_cube, kInvalidMaterialHandle, true});

    Entity e2 = world.create_entity();
    world.add<scene::Transform>(e2, scene::Transform{});
    world.add<MeshComponent>(e2, MeshComponent{h_sphere, kInvalidMaterialHandle, true});

    // Invisible: must not be extracted
    Entity e3 = world.create_entity();
    world.add<scene::Transform>(e3, scene::Transform{});
    world.add<MeshComponent>(e3, MeshComponent{h_cube, kInvalidMaterialHandle, false});

    // Gameplay-only: no mesh component, must not be extracted
    Entity e4 = world.create_entity();
    world.add<scene::Transform>(e4, scene::Transform{});
    world.add<int>(e4, 42);

    scene::propagate_transforms(world);
    RenderWorld rw;
    nf::runtime::extract_render_objects(world, meshes, rw);
    NF_CHECK_EQ(rw.size(), 2u);
    NF_CHECK(rw.objects[0].mesh_handle == h_cube);
    NF_CHECK(rw.objects[1].mesh_handle == h_sphere);
    NF_CHECK_NEAR(rw.objects[0].transform.x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(rw.objects[0].world.m[12], 1.0f, 1e-6f);
    // Bounds are world-space: cube at x=1 spans [0.5, 1.5]
    NF_CHECK_NEAR(rw.objects[0].bounds.min_x, 0.5f, 1e-5f);
    NF_CHECK_NEAR(rw.objects[0].bounds.max_x, 1.5f, 1e-5f);
    NF_CHECK_NEAR(rw.objects[0].sphere.cx, 1.0f, 1e-5f);
    // Gameplay components never cross: RenderObject has no such fields
}

NF_TEST(renderobject_culling_pipeline) {
    // RenderWorld → Frustum Culling → Visible Objects: inside submitted, outside culled
    World world;
    MeshLibrary meshes;
    auto cube = StaticMesh::create_cube(1.0f);
    StaticMeshHandle h = meshes.add(std::move(cube));

    for (int i = 0; i < 4; ++i) {
        Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        auto* t = world.get<scene::Transform>(e);
        if (i == 0) { t->local_x = 0; }          // inside
        if (i == 1) { t->local_x = 100; }        // outside
        if (i == 2) { t->local_z = -2; }         // inside
        if (i == 3) { t->local_y = -100; }       // outside
        world.add<MeshComponent>(e, MeshComponent{h, kInvalidMaterialHandle, true});
    }
    scene::propagate_transforms(world);
    RenderWorld rw;
    nf::runtime::extract_render_objects(world, meshes, rw);
    NF_CHECK_EQ(rw.size(), 4u);

    Camera cam = make_camera(5.0f);
    std::vector<u32> visible;
    cull_render_world(rw, cam, visible);
    NF_CHECK_EQ(visible.size(), 2u);
    for (u32 idx : visible) {
        NF_CHECK(std::abs(rw.objects[idx].transform.x) < 10.0f);
        NF_CHECK(std::abs(rw.objects[idx].transform.y) < 10.0f);
        NF_CHECK(std::abs(rw.objects[idx].transform.z) < 10.0f);
    }
}

// ---------------------------------------------------------------------------
// Generic RenderGraph resource states + barriers (backend-neutral)
// ---------------------------------------------------------------------------

NF_TEST(generic_resource_state_transition) {
    // Every declared state maps to a sensible backend-neutral usage
    NF_CHECK(to_image_usage(RGResourceState::Undefined) == rhi::ImageUsage::None);
    NF_CHECK(to_image_usage(RGResourceState::TransferSrc) == rhi::ImageUsage::TransferSrc);
    NF_CHECK(to_image_usage(RGResourceState::TransferDst) == rhi::ImageUsage::TransferDst);
    NF_CHECK(to_image_usage(RGResourceState::ColorAttachment) == rhi::ImageUsage::ColorAtt);
    NF_CHECK(to_image_usage(RGResourceState::DepthAttachment) == rhi::ImageUsage::DepthAtt);
    NF_CHECK(to_image_usage(RGResourceState::ShaderRead) == rhi::ImageUsage::Sampled);
    NF_CHECK(to_image_usage(RGResourceState::ShaderWrite) == rhi::ImageUsage::Storage);
    NF_CHECK(to_image_usage(RGResourceState::Present) == rhi::ImageUsage::ColorAtt);

    // Round-trips
    NF_CHECK(from_image_usage(to_image_usage(RGResourceState::ShaderWrite)) == RGResourceState::ShaderWrite);
    NF_CHECK(from_image_usage(to_image_usage(RGResourceState::Present)) == RGResourceState::ColorAttachment);

    // The pipeline's actual transitions infer the right usage pairs
    auto t = [](RGResourceState from, RGResourceState to) {
        return get_transition(from, to);
    };
    UsageTransition a = t(RGResourceState::Undefined, RGResourceState::ColorAttachment);
    NF_CHECK(a.before == rhi::ImageUsage::None && a.after == rhi::ImageUsage::ColorAtt);
    UsageTransition b = t(RGResourceState::ColorAttachment, RGResourceState::ShaderRead);
    NF_CHECK(b.before == rhi::ImageUsage::ColorAtt && b.after == rhi::ImageUsage::Sampled);
    UsageTransition c = t(RGResourceState::DepthAttachment, RGResourceState::ShaderRead);
    NF_CHECK(c.before == rhi::ImageUsage::DepthAtt && c.after == rhi::ImageUsage::Sampled);
    UsageTransition d = t(RGResourceState::ShaderRead, RGResourceState::DepthAttachment);
    NF_CHECK(d.before == rhi::ImageUsage::Sampled && d.after == rhi::ImageUsage::DepthAtt);
    UsageTransition e = t(RGResourceState::ShaderRead, RGResourceState::ColorAttachment);
    NF_CHECK(e.before == rhi::ImageUsage::Sampled && e.after == rhi::ImageUsage::ColorAtt);
    UsageTransition g = t(RGResourceState::Undefined, RGResourceState::ShaderWrite);
    NF_CHECK(g.after == rhi::ImageUsage::Storage);
    UsageTransition h = t(RGResourceState::ShaderWrite, RGResourceState::ShaderRead);
    NF_CHECK(h.before == rhi::ImageUsage::Storage && h.after == rhi::ImageUsage::Sampled);
}

NF_TEST(generic_rendergraph_barrier) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;

    rhi::TextureDesc td{};
    td.width = 16;
    td.height = 16;
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    auto texA = dev.create_texture(td);
    auto texB = dev.create_texture(td);
    NF_CHECK(texA && texB);

    RenderGraph graph(dev);
    auto hA = graph.import_texture("A", texA.get());
    auto hB = graph.import_texture("B", texB.get());

    rhi::ColorAttachment ca{};
    ca.format = td.format;
    const std::array<rhi::ColorAttachment, 1> atts{ca};
    rhi::RenderPassDesc rpd{};
    rpd.color_attachments = std::span<const rhi::ColorAttachment>(atts);
    rpd.present_source = false;
    auto rp = dev.create_render_pass(rpd);
    NF_CHECK(rp != nullptr);

    const std::array<rhi::Texture*, 1> colsA{texA.get()};
    auto fbA = dev.create_framebuffer(*rp, std::span<rhi::Texture* const>(colsA), nullptr);
    NF_CHECK(fbA != nullptr);

    const std::array<rhi::Texture*, 1> colsB{texB.get()};
    auto fbB = dev.create_framebuffer(*rp, std::span<rhi::Texture* const>(colsB), nullptr);
    NF_CHECK(fbB != nullptr);

    bool pass1_executed = false, pass2_executed = false;
    RGPassDesc p1{};
    p1.name = "WriteA";
    p1.color_attachments = {hA};
    p1.execute = [&](rhi::CommandBuffer& cmd) {
        pass1_executed = true;
        const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{1, 0, 0, 1}};
        cmd.begin_render_pass(*rp, *fbA, std::span<const rhi::ClearValue>(clears));
        cmd.end_render_pass();
    };
    RGPassDesc p2{};
    p2.name = "ReadA_WriteB";
    p2.reads = {hA};
    p2.color_attachments = {hB};
    p2.execute = [&](rhi::CommandBuffer& cmd) {
        pass2_executed = true;
        const std::array<rhi::ClearValue, 1> clears{rhi::ClearValue{0, 1, 0, 1}};
        cmd.begin_render_pass(*rp, *fbB, std::span<const rhi::ClearValue>(clears));
        cmd.end_render_pass();
    };
    graph.add_pass(p1);
    graph.add_pass(p2);
    NF_CHECK(graph.compile());
    // p2 must run after p1 (read-after-write dependency inferred)
    NF_CHECK_EQ(graph.execution_order().size(), 2u);
    NF_CHECK_EQ(graph.execution_order()[0], 0u);
    NF_CHECK_EQ(graph.execution_order()[1], 1u);

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    cmd->begin();
    graph.execute(*cmd); // inserts ColorAttachment→ShaderRead barrier for A
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    dev.wait_idle();
    NF_CHECK(pass1_executed && pass2_executed);

    // Depth-role declaration: a depth pass followed by a reader must also
    // schedule in order and issue DepthAttachment→ShaderRead via the role,
    // not via format sniffing.
    rhi::TextureDesc depth_td{};
    depth_td.width = 16;
    depth_td.height = 16;
    depth_td.format = rhi::Format::D32_SFloat;
    depth_td.usage = rhi::ImageUsage::DepthAtt | rhi::ImageUsage::Sampled;
    auto depth = dev.create_texture(depth_td);
    RenderGraph g2(dev);
    auto hD = g2.import_texture("Depth", depth.get());
    bool wrote_depth = false, read_depth = false;
    RGPassDesc pd{};
    pd.name = "DepthWrite";
    pd.depth_attachment = hD;
    pd.execute = [&](rhi::CommandBuffer&) { wrote_depth = true; };
    RGPassDesc pr{};
    pr.name = "DepthRead";
    pr.reads = {hD};
    pr.execute = [&](rhi::CommandBuffer&) { read_depth = true; };
    g2.add_pass(pd);
    g2.add_pass(pr);
    NF_CHECK(g2.compile());
    auto cmd2 = dev.create_command_buffer();
    auto fence2 = dev.create_fence(false);
    cmd2->begin();
    g2.execute(*cmd2);
    cmd2->end();
    dev.submit(*cmd2, rhi::SubmitInfo{.signal_fence = fence2.get()});
    NF_CHECK(fence2->wait(kGpuTimeoutNs));
    dev.wait_idle();
    NF_CHECK(wrote_depth && read_depth);
}

// ---------------------------------------------------------------------------
// Passes: depth prepass + gbuffer (verified from GPU output)
// ---------------------------------------------------------------------------

NF_TEST(depth_pass) {
    const GpuFixture& f = require_gpu();
    auto sdir = basic3d_shader_dir();
    if (sdir.empty()) NF_SKIP("required test asset missing");
    auto& dev = *f.device;

    auto vs = load_spirv(sdir / "depth_vert.spv");
    auto fs = load_spirv(sdir / "depth_frag.spv");
    NF_CHECK(!vs.empty() && !fs.empty());
    auto vs_mod = dev.create_shader_module({vs, rhi::ShaderStage::Vertex});
    auto fs_mod = dev.create_shader_module({fs, rhi::ShaderStage::Fragment});
    NF_CHECK(vs_mod && fs_mod);

    auto cube = StaticMesh::create_cube(1.5f);
    NF_CHECK(cube->upload(dev));

    constexpr u32 W = 64, H = 64;
    // TransferSrc is required for the depth readback below.
    rhi::TextureDesc depth_desc{};
    depth_desc.width = W;
    depth_desc.height = H;
    depth_desc.format = rhi::Format::D32_SFloat;
    depth_desc.usage = rhi::ImageUsage::DepthAtt | rhi::ImageUsage::TransferSrc;
    auto depth_tex = dev.create_texture(depth_desc);
    NF_CHECK(depth_tex);
    rhi::RenderPassDesc rp_desc{};
    rp_desc.has_depth = true;
    rp_desc.depth_format = rhi::Format::D32_SFloat;
    rp_desc.present_source = false;
    auto rp = dev.create_render_pass(rp_desc);
    NF_CHECK(rp);
    const std::array<rhi::Texture*, 0> no_colors{};
    auto fb = dev.create_framebuffer(*rp, std::span<rhi::Texture* const>(no_colors), depth_tex.get());
    NF_CHECK(fb);

    PipelineCache pcache(dev);
    rhi::VertexLayout vl{};
    vl.binding = 0;
    vl.stride = sizeof(Vertex);
    const std::array<rhi::VertexAttrib, 1> attribs{{
        {0, offsetof(Vertex, position), rhi::Format::R32G32B32_SFloat},
    }};
    vl.attributes = std::span<const rhi::VertexAttrib>(attribs);
    rhi::PipelineDesc pd{};
    pd.vs = vs_mod.get();
    pd.fs = fs_mod.get();
    pd.render_pass = rp.get();
    pd.vertex_layout = vl;
    pd.depth.test_enabled = true;
    pd.depth.write_enabled = true;
    pd.push_constant_size = 128;
    pd.push_constant_stages = rhi::ShaderStage::Vertex;
    auto* pipe = pcache.get_or_create(pd);
    NF_CHECK(pipe);

    Camera cam = make_camera(3.0f);
    struct Push { float view_proj[16]; float model[16]; };
    Push push{};
    std::memcpy(push.view_proj, cam.view_projection.m, sizeof(push.view_proj));
    push.model[0] = push.model[5] = push.model[10] = push.model[15] = 1.0f;

    rhi::BufferDesc rb_desc{};
    rb_desc.size = usize(W) * H * 4;
    rb_desc.usage = rhi::BufferUsage::TransferDst;
    rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = dev.create_buffer(rb_desc);
    NF_CHECK(rb);
    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    cmd->begin();
    const std::array<rhi::ClearValue, 0> no_clears{};
    cmd->begin_render_pass(*rp, *fb, std::span<const rhi::ClearValue>(no_clears), 1.0f, 0);
    cmd->bind_pipeline(*pipe);
    cmd->push_constants(rhi::ShaderStage::Vertex, 0, sizeof(push), &push);
    const std::array<const rhi::Buffer*, 1> vbs{cube->vertex_buffer(0)};
    cmd->bind_vertex_buffers(std::span<const rhi::Buffer* const>(vbs));
    cmd->bind_index_buffer(*cube->index_buffer(0), 0);
    cmd->set_viewport(0, 0, W, H);
    cmd->set_scissor(0, 0, W, H);
    cmd->draw_indexed(36);
    cmd->end_render_pass();
    cmd->copy_texture_to_buffer(*depth_tex, *rb, 0, 0, W, H, 0);
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    dev.wait_idle();

    // Pixel verification on the depth aspect: clear state is 1.0 everywhere
    // the cube did not rasterize; geometry sits strictly in front of it.
    auto* depths = static_cast<const float*>(rb->map());
    NF_CHECK(depths);
    u32 written = 0, cleared = 0;
    float min_depth = 2.0f;
    for (usize i = 0; i < usize(W) * H; ++i) {
        const float d = depths[i];
        if (d >= 0.999999f) { ++cleared; }
        else { ++written; min_depth = std::min(min_depth, d); }
    }
    rb->unmap();
    NF_CHECK(written > 300);                 // cube actually rasterized
    NF_CHECK(cleared > 100);                 // background stayed at clear
    NF_CHECK(min_depth > 0.0f && min_depth < 0.99f);
}

NF_TEST(gbuffer_pass) {
    // GBuffer through the real pipeline: BaseColor / Normal / Surface targets
    // are RenderGraph-owned and verified by readback.
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    NF_CHECK(renderer.init(dev, basic3d_shader_dir(), 64, 64));
    MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);
    auto cube = StaticMesh::create_cube(1.0f);
    StaticMeshHandle h = meshes.add(std::move(cube));
    NF_CHECK(meshes.upload_all(dev));

    PBRMaterialParams params{};
    params.base_color[0] = 0.8f; params.base_color[1] = 0.2f; params.base_color[2] = 0.2f;
    params.roughness = 0.5f;
    MaterialHandle mat = renderer.materials().create_instance(*renderer.gbuffer_material(), params, "red");

    World world;
    Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    world.add<MeshComponent>(e, MeshComponent{h, mat, true});
    scene::propagate_transforms(world);
    RenderWorld rw;
    nf::runtime::extract_render_objects(world, meshes, rw);

    Camera cam = make_camera(3.0f);
    rhi::TextureDesc target_desc{};
    target_desc.width = 64;
    target_desc.height = 64;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    auto target = dev.create_texture(target_desc);

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    cmd->begin();
    NF_CHECK(renderer.render(*cmd, rw, cam, *target));
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    dev.wait_idle();

    // Readback each gbuffer attachment
    auto readback = [&](rhi::Texture* tex, std::vector<Pixel>& out) {
        rhi::BufferDesc rb_desc{};
        rb_desc.size = 64ull * 64 * 4;
        rb_desc.usage = rhi::BufferUsage::TransferDst;
        rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
        auto rb = dev.create_buffer(rb_desc);
        NF_CHECK(rb && tex);
        auto c2 = dev.create_command_buffer();
        auto f2 = dev.create_fence(false);
        c2->begin();
        c2->copy_texture_to_buffer(*tex, *rb, 0, 0, 64, 64, 0);
        c2->end();
        dev.submit(*c2, rhi::SubmitInfo{.signal_fence = f2.get()});
        NF_CHECK(f2->wait(kGpuTimeoutNs));
        dev.wait_idle();
        auto* px = static_cast<const Pixel*>(rb->map());
        NF_CHECK(px);
        out.assign(px, px + 64 * 64);
        rb->unmap();
    };

    std::vector<Pixel> base, normal, surface;
    readback(renderer.gbuffer_target(0), base);
    readback(renderer.gbuffer_target(1), normal);
    readback(renderer.gbuffer_target(2), surface);
    renderer.shutdown();

    const usize center = usize(32) * 64 + 32;
    const usize corner = usize(2) * 64 + 2;
    // BaseColor: red cube face in the middle, clear (black) in the corner
    NF_CHECK(base[center].r > 120 && base[center].g < 90 && base[center].b < 90);
    NF_CHECK(base[corner].r == 0 && base[corner].g == 0 && base[corner].b == 0);
    // Normal: encoded *0.5+0.5 — front face points at +Z → blue-dominant
    NF_CHECK(normal[center].b > 200);
    // Surface: metallic=0, roughness=0.5 → g ≈ 128
    NF_CHECK(surface[center].g > 100 && surface[center].g < 160);
    NF_CHECK(surface[center].r < 20); // metallic 0
    NF_CHECK(surface[corner].r == 0 && surface[corner].g == 0);
}

// ---------------------------------------------------------------------------
// PBR materials: parameters without pipelines
// ---------------------------------------------------------------------------

NF_TEST(pbr_material) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    NF_CHECK(renderer.init(dev, basic3d_shader_dir(), 32, 32));
    Material* material = renderer.gbuffer_material();
    NF_CHECK(material != nullptr && material->valid());

    // Two instances of one material: same pipeline, different parameters
    PBRMaterialParams metal{};
    metal.metallic = 1.0f;
    PBRMaterialParams matte{};
    matte.roughness = 1.0f;
    MaterialHandle h1 = renderer.materials().create_instance(*material, metal, "metal");
    MaterialHandle h2 = renderer.materials().create_instance(*material, matte, "matte");
    NF_CHECK(h1.valid() && h2.valid());
    NF_CHECK_EQ(renderer.materials().get(h1)->material, renderer.materials().get(h2)->material);

    // Parameter values round-trip
    const PBRMaterialParams* p1 = renderer.materials().params(h1);
    const PBRMaterialParams* p2 = renderer.materials().params(h2);
    NF_CHECK(p1 && p2);
    NF_CHECK_NEAR(p1->metallic, 1.0f, 1e-6f);
    NF_CHECK_NEAR(p2->roughness, 1.0f, 1e-6f);

    // The params UBO actually holds the packed block
    float packed[12];
    p1->pack(packed);
    auto* mapped = static_cast<const float*>(renderer.materials().get(h1)->params_ubo->map());
    NF_CHECK(mapped);
    NF_CHECK_NEAR(mapped[4], packed[4], 1e-6f); // metallic
    NF_CHECK_NEAR(mapped[5], packed[5], 1e-6f); // roughness
    renderer.materials().get(h1)->params_ubo->unmap();
    renderer.shutdown();
}

NF_TEST(material_instance_parameter_update) {
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    NF_CHECK(renderer.init(dev, basic3d_shader_dir(), 32, 32));
    Material* material = renderer.gbuffer_material();

    PBRMaterialParams params{};
    params.base_color[0] = 1.0f;
    MaterialHandle h = renderer.materials().create_instance(*material, params, "mutable");
    MaterialEntry* entry = renderer.materials().get(h);
    NF_CHECK(entry);

    rhi::Pipeline& pipeline_before = material->pipeline();
    rhi::Pipeline& entry_pipeline_before = entry->material->pipeline();

    // Mutate parameters — must be a pure buffer write: no new pipeline, no
    // cache growth, no descriptor churn.
    PBRMaterialParams updated{};
    updated.base_color[0] = 0.1f;
    updated.base_color[1] = 0.9f;
    updated.metallic = 1.0f;
    updated.roughness = 0.25f;
    renderer.materials().set_params(h, updated);

    NF_CHECK(&material->pipeline() == &pipeline_before);
    NF_CHECK(&entry->material->pipeline() == &entry_pipeline_before);

    auto* mapped = static_cast<const float*>(entry->params_ubo->map());
    NF_CHECK(mapped);
    NF_CHECK_NEAR(mapped[0], 0.1f, 1e-6f);
    NF_CHECK_NEAR(mapped[1], 0.9f, 1e-6f);
    NF_CHECK_NEAR(mapped[4], 1.0f, 1e-6f); // metallic
    NF_CHECK_NEAR(mapped[5], 0.25f, 1e-6f);
    entry->params_ubo->unmap();
    renderer.shutdown();
}

// ---------------------------------------------------------------------------
// Lighting: response tests — change the input, see the pixels move
// ---------------------------------------------------------------------------

namespace {

// Scene used by the light/material response tests: one cube dead center with
// a scalar material, configured per test through `setup`.
struct SceneSpec {
    PBRMaterialParams material;
    DirectionalLight directional;
    std::vector<PointLight> points;
    std::vector<SpotLight> spots;
    float camera_z = 3.0f;
    bool light_enabled = true;
};

bool render_spec(rhi::IGraphicsDevice& dev, u32 W, u32 H, const SceneSpec& spec,
                 std::vector<Pixel>& out) {
    const Camera cam = make_camera(spec.camera_z, float(W) / float(H));
    return render_full_chain(dev, W, H,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
            auto cube = StaticMesh::create_cube(1.5f);
            StaticMeshHandle h = meshes.add(std::move(cube));
            meshes.upload_all(dev);
            MaterialHandle m = renderer.materials().create_instance(
                *renderer.gbuffer_material(), spec.material, "spec");
            Entity e = world.create_entity();
            world.add<scene::Transform>(e, scene::Transform{});
            world.add<MeshComponent>(e, MeshComponent{h, m, true});

            DirectionalLight d = spec.directional;
            d.enabled = spec.light_enabled;
            renderer.set_directional_light(d);
            for (const PointLight& l : spec.points) renderer.add_point_light(l);
            for (const SpotLight& l : spec.spots) renderer.add_spot_light(l);
        },
        out, nullptr, &cam);
}

SceneSpec base_spec() {
    SceneSpec s{};
    s.material.base_color[0] = 0.8f;
    s.material.base_color[1] = 0.8f;
    s.material.base_color[2] = 0.8f;
    s.material.roughness = 0.4f;
    s.directional.direction = {0.0f, -1.0f, -0.6f};
    s.directional.color = {1, 1, 1};
    s.directional.intensity = 2.0f;
    return s;
}

} // namespace

NF_TEST(lighting_pass) {
    const GpuFixture& f = require_gpu();
    std::vector<Pixel> lit;
    SceneSpec s = base_spec();
    NF_CHECK(render_spec(*f.device, 64, 64, s, lit));
    NF_LOG_WARN(nf::LogCategory::Core, "lighting debug: center=({}, {}, {}) c1=({}, {}, {}) litcount={}",
                lit[32 * 64 + 32].r, lit[32 * 64 + 32].g, lit[32 * 64 + 32].b,
                lit[30 * 64 + 32].r, lit[30 * 64 + 32].g, lit[30 * 64 + 32].b,
                count_pixels_above(lit, 30));
    // Lit geometry against a black background — the lighting pass ran
    NF_CHECK(count_pixels_above(lit, 30) > 300);
    u32 dark = 0;
    for (usize i = 0; i < lit.size(); ++i) {
        const u32 x = static_cast<u32>(i % 64);
        const u32 y = static_cast<u32>(i / 64);
        if ((x < 6 || x > 57) && (y < 6 || y > 57)) {
            if (lit[i].r < 8 && lit[i].g < 8 && lit[i].b < 8) ++dark;
        }
    }
    NF_CHECK(dark > 100); // corners stayed unlit
}

NF_TEST(directional_light) {
    const GpuFixture& f = require_gpu();
    SceneSpec s = base_spec();
    std::vector<Pixel> on, off;
    NF_CHECK(render_spec(*f.device, 64, 64, s, on));
    s.light_enabled = false;
    NF_CHECK(render_spec(*f.device, 64, 64, s, off));
    // Toggling the directional light changes the image; off ≈ ambient only
    NF_CHECK(count_different_pixels(on, off) > 300);
    NF_CHECK(pixel_sum(on) > pixel_sum(off) * 2);
}

NF_TEST(point_light) {
    const GpuFixture& f = require_gpu();
    SceneSpec s = base_spec();
    s.light_enabled = false; // isolate the point light

    SceneSpec near_light = s;
    near_light.points.push_back(PointLight{Vec3{0, 0.5f, 1.5f}, Vec3{1, 1, 1}, 8.0f, 10.0f});
    SceneSpec far_light = s;
    far_light.points.push_back(PointLight{Vec3{0, 0.5f, 60.0f}, Vec3{1, 1, 1}, 8.0f, 10.0f});

    std::vector<Pixel> near_img, far_img, none;
    NF_CHECK(render_spec(*f.device, 64, 64, near_light, near_img));
    NF_CHECK(render_spec(*f.device, 64, 64, far_light, far_img));
    NF_CHECK(render_spec(*f.device, 64, 64, s, none));
    // Moving the light changes the output, and proximity adds energy
    NF_CHECK(count_different_pixels(near_img, none) > 100);
    NF_CHECK(count_different_pixels(near_img, far_img) > 100);
    NF_CHECK(pixel_sum(near_img) > pixel_sum(far_img));
}

NF_TEST(spot_light) {
    const GpuFixture& f = require_gpu();
    SceneSpec s = base_spec();
    s.light_enabled = false; // isolate the spot light

    SceneSpec aimed = s;
    aimed.spots.push_back(SpotLight{Vec3{0, 0, 3}, Vec3{0, 0, -1}, Vec3{1, 1, 1}, 30.0f, 0.5f, 0.9f});
    SceneSpec away = s;
    away.spots.push_back(SpotLight{Vec3{0, 0, 3}, Vec3{0, 1, 0}, Vec3{1, 1, 1}, 30.0f, 0.5f, 0.9f});

    std::vector<Pixel> hit, missed, none;
    NF_CHECK(render_spec(*f.device, 64, 64, aimed, hit));
    NF_CHECK(render_spec(*f.device, 64, 64, away, missed));
    NF_CHECK(render_spec(*f.device, 64, 64, s, none));
    // Cone aimed at the cube lights it; cone aimed at the sky does not
    NF_CHECK(count_different_pixels(hit, none) > 100);
    NF_CHECK(count_different_pixels(hit, missed) > 100);
    NF_CHECK(pixel_sum(hit) > pixel_sum(missed) * 2);
    NF_CHECK(pixel_sum(missed) < pixel_sum(none) * 2); // away ≈ unlit
}

// ---------------------------------------------------------------------------
// PBR response: material parameters must move pixels
// ---------------------------------------------------------------------------

NF_TEST(pbr_metallic_response) {
    const GpuFixture& f = require_gpu();
    SceneSpec s = base_spec();
    SceneSpec dielectric = s;
    dielectric.material.metallic = 0.0f;
    SceneSpec metal = s;
    metal.material.metallic = 1.0f;
    metal.material.roughness = 0.3f;
    std::vector<Pixel> a, b;
    NF_CHECK(render_spec(*f.device, 64, 64, dielectric, a));
    NF_CHECK(render_spec(*f.device, 64, 64, metal, b));
    NF_CHECK(count_different_pixels(a, b) > 150);
}

NF_TEST(pbr_roughness_response) {
    const GpuFixture& f = require_gpu();
    SceneSpec s = base_spec();
    // Metallic + light nearly behind the camera puts the GGX specular spike
    // on the visible face; with a dielectric at this geometry the roughness
    // response is too dim to verify pixel-wise.
    s.material.metallic = 1.0f;
    s.material.base_color[0] = 0.9f;
    s.material.base_color[1] = 0.9f;
    s.material.base_color[2] = 0.9f;
    s.directional.direction = Vec3{0.0f, 0.0f, -1.0f};
    SceneSpec glossy = s;
    glossy.material.roughness = 0.05f;
    SceneSpec rough = s;
    rough.material.roughness = 1.0f;
    std::vector<Pixel> a, b;
    NF_CHECK(render_spec(*f.device, 64, 64, glossy, a));
    NF_CHECK(render_spec(*f.device, 64, 64, rough, b));
    NF_CHECK(count_different_pixels(a, b) > 150);
}

NF_TEST(pbr_basecolor_response) {
    const GpuFixture& f = require_gpu();
    SceneSpec s = base_spec();
    SceneSpec red = s;
    set_rgb(red.material, 0.9f, 0.1f, 0.1f);
    SceneSpec green = s;
    set_rgb(green.material, 0.1f, 0.9f, 0.1f);
    std::vector<Pixel> a, b;
    NF_CHECK(render_spec(*f.device, 64, 64, red, a));
    NF_CHECK(render_spec(*f.device, 64, 64, green, b));
    NF_CHECK(count_different_pixels(a, b) > 300);
    // The channel identity survives: red image is redder, green is greener
    u64 red_dom = 0, green_dom = 0;
    for (usize i = 0; i < a.size(); ++i) {
        if (a[i].r > a[i].g + 20) ++red_dom;
        if (b[i].g > b[i].r + 20) ++green_dom;
    }
    NF_CHECK(red_dom > 150);
    NF_CHECK(green_dom > 150);
}

NF_TEST(light_intensity_response) {
    const GpuFixture& f = require_gpu();
    SceneSpec s = base_spec();
    SceneSpec dim = s;
    dim.directional.intensity = 0.25f;
    SceneSpec bright = s;
    bright.directional.intensity = 6.0f;
    std::vector<Pixel> a, b;
    NF_CHECK(render_spec(*f.device, 64, 64, dim, a));
    NF_CHECK(render_spec(*f.device, 64, 64, bright, b));
    NF_CHECK(count_different_pixels(a, b) > 300);
    NF_CHECK(pixel_sum(b) > pixel_sum(a));
}

// (helper declared late on purpose? No — see fix below.)

NF_TEST(camera_transform_response) {
    const GpuFixture& f = require_gpu();
    SceneSpec s = base_spec();

    auto build = [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
        auto cube = StaticMesh::create_cube(1.5f);
        auto h = meshes.add(std::move(cube));
        meshes.upload_all(*f.device);
        auto m = renderer.materials().create_instance(*renderer.gbuffer_material(), s.material, "m");
        Entity e = world.create_entity();
        world.add<scene::Transform>(e, scene::Transform{});
        world.add<MeshComponent>(e, MeshComponent{h, m, true});
        renderer.set_directional_light(s.directional);
    };

    // Camera straight on vs orbited around the cube: different faces face the
    // camera, so the tonemapped image must change.
    Camera front_cam = make_camera(3.0f);
    Camera orbit_cam{};
    orbit_cam.position = {2.12f, 0.9f, 2.12f};
    orbit_cam.target = {0, 0, 0};
    orbit_cam.aspect = 1.0f;
    orbit_cam.fov_y_rad = front_cam.fov_y_rad;
    orbit_cam.near_plane = 0.1f;
    orbit_cam.far_plane = 100.0f;
    update_camera(orbit_cam);

    std::vector<Pixel> front, side;
    NF_CHECK(render_full_chain(*f.device, 64, 64, build, front, nullptr, &front_cam));
    NF_CHECK(render_full_chain(*f.device, 64, 64, build, side, nullptr, &orbit_cam));
    NF_CHECK(count_different_pixels(front, side) > 150);
}

NF_TEST(tonemapping) {
    const GpuFixture& f = require_gpu();
    // Exposure is part of the tonemap pass; the helper doesn't expose it, so
    // run the pipeline twice with different exposure values directly.
    auto render_with_exposure = [&](float exposure, std::vector<Pixel>& out) {
        return render_full_chain(*f.device, 64, 64,
            [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
                auto cube = StaticMesh::create_cube(1.5f);
                auto h = meshes.add(std::move(cube));
                meshes.upload_all(*f.device);
                PBRMaterialParams p{};
                set_rgb(p, 0.8f, 0.8f, 0.8f);
                auto m = renderer.materials().create_instance(*renderer.gbuffer_material(), p, "m");
                Entity e = world.create_entity();
                world.add<scene::Transform>(e, scene::Transform{});
                world.add<MeshComponent>(e, MeshComponent{h, m, true});
                renderer.set_directional_light(DirectionalLight{Vec3{0, -1, -0.6f}, Vec3{1, 1, 1}, 2.0f, true});
                renderer.set_exposure(exposure);
            }, out);
    };
    std::vector<Pixel> dim, bright;
    NF_CHECK(render_with_exposure(0.25f, dim));
    NF_CHECK(render_with_exposure(4.0f, bright));
    NF_CHECK(count_different_pixels(dim, bright) > 200);
    NF_CHECK(pixel_sum(bright) > pixel_sum(dim)); // monotonic exposure response
}

NF_TEST(basic3d_pixel_verification) {
    const GpuFixture& f = require_gpu();
    // The full requested chain with two meshes, distinct materials and lights:
    //   ECS → Extraction → Culling → Depth → GBuffer → Lighting → Tonemap → GPU → Readback
    Renderer3D::Stats stats{};
    std::vector<Pixel> px_stats_dummy;
    bool ok = render_full_chain(*f.device, 64, 64,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
            auto cube = StaticMesh::create_cube(1.0f);
            auto sphere = StaticMesh::create_sphere(0.5f, 12);
            StaticMeshHandle h_cube = meshes.add(std::move(cube));
            StaticMeshHandle h_sphere = meshes.add(std::move(sphere));
            meshes.upload_all(*f.device);

            PBRMaterialParams red{};
            set_rgb(red, 0.9f, 0.15f, 0.1f);
            red.roughness = 0.35f;
            PBRMaterialParams gold{};
            set_rgb(gold, 0.95f, 0.8f, 0.3f);
            gold.metallic = 1.0f;
            gold.roughness = 0.25f;
            MaterialHandle m_red = renderer.materials().create_instance(*renderer.gbuffer_material(), red, "red");
            MaterialHandle m_gold = renderer.materials().create_instance(*renderer.gbuffer_material(), gold, "gold");

            Entity a = world.create_entity();
            world.add<scene::Transform>(a, scene::Transform{});
            world.get<scene::Transform>(a)->local_x = -0.9f;
            world.add<MeshComponent>(a, MeshComponent{h_cube, m_red, true});

            Entity b = world.create_entity();
            world.add<scene::Transform>(b, scene::Transform{});
            world.get<scene::Transform>(b)->local_x = 0.9f;
            world.add<MeshComponent>(b, MeshComponent{h_sphere, m_gold, true});

            renderer.set_directional_light(DirectionalLight{Vec3{-0.4f, -1.0f, -0.5f}, Vec3{1, 1, 1}, 2.5f, true});
            renderer.add_point_light(PointLight{Vec3{0, 1.5f, 1.0f}, Vec3{0.4f, 0.5f, 1.0f}, 6.0f, 12.0f});
            renderer.add_spot_light(SpotLight{Vec3{0, 4, 0}, Vec3{0, -1, 0}, Vec3{1, 0.9f, 0.8f}, 10.0f, 0.4f, 0.8f});
        },
        px_stats_dummy, &stats);
    NF_CHECK(ok);
    NF_LOG_WARN(nf::LogCategory::Core, "basic3d debug: center=({}, {}, {}) litcount={}",
                px_stats_dummy[32 * 64 + 32].r, px_stats_dummy[32 * 64 + 32].g,
                px_stats_dummy[32 * 64 + 32].b, count_pixels_above(px_stats_dummy, 30));
    NF_CHECK(count_pixels_above(px_stats_dummy, 30) > 400);
    NF_CHECK_EQ(stats.visible, 2u);       // both meshes survived culling
    NF_CHECK(stats.draw_calls >= 2u);     // both were submitted
}

// ---------------------------------------------------------------------------
// Resource lifetime + validation
// ---------------------------------------------------------------------------

NF_TEST(resource_lifetime_zero_leaks) {
    auto dev = rhi::create_device();
    NF_CHECK(dev);
    rhi::DeviceDesc ddesc{};
    ddesc.enable_validation = false;
    NF_CHECK(dev->init(ddesc));

    {
        rhi::BufferDesc buf_desc{};
        buf_desc.size = 256;
        buf_desc.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
        buf_desc.memory = rhi::MemoryUsage::GPUOnly;
        auto buffer = dev->create_buffer(buf_desc);
        NF_CHECK(buffer);

        rhi::TextureDesc tex_desc{};
        tex_desc.width = 8;
        tex_desc.height = 8;
        tex_desc.format = rhi::Format::R8G8B8A8_UNorm;
        tex_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
        auto tex = dev->create_texture(tex_desc);
        auto view = tex ? dev->create_texture_view(*tex) : nullptr;
        NF_CHECK(tex && view);

        auto sampler = dev->create_sampler(rhi::SamplerDesc{});
        NF_CHECK(sampler);

        rhi::ColorAttachment ca{};
        ca.format = tex_desc.format;
        const std::array<rhi::ColorAttachment, 1> atts{ca};
        rhi::RenderPassDesc rpd{};
        rpd.color_attachments = std::span<const rhi::ColorAttachment>(atts);
        rpd.present_source = false;
        auto rp = dev->create_render_pass(rpd);
        NF_CHECK(rp);
        if (tex) {
            const std::array<rhi::Texture*, 1> colors{tex.get()};
            auto fb = dev->create_framebuffer(*rp, std::span<rhi::Texture* const>(colors), nullptr);
            NF_CHECK(fb);
        }

        const std::array<rhi::DescriptorBinding, 1> binds{{
            {0, rhi::DescriptorType::SampledImage, rhi::ShaderStage::Fragment, 1},
        }};
        rhi::DescriptorSetLayoutDesc ld{};
        ld.bindings = std::span<const rhi::DescriptorBinding>(binds);
        auto layout = dev->create_descriptor_set_layout(ld);
        NF_CHECK(layout);
        auto set = layout ? dev->create_descriptor_set(*layout) : nullptr;
        NF_CHECK(set);

        auto allocator = dev->create_descriptor_allocator(4);
        NF_CHECK(allocator);
        auto pooled = layout ? allocator->allocate(*layout) : nullptr;
        NF_CHECK(pooled);

        auto cmd = dev->create_command_buffer();
        auto semaphore = dev->create_semaphore();
        auto fence = dev->create_fence(false);
        NF_CHECK(cmd && semaphore && fence);
    }
    // Everything above was scoped — the device must be holding zero objects.
    NF_CHECK_EQ(dev->alive_objects(), 0u);
    dev->wait_idle();
    dev->shutdown();
}

NF_TEST(validation_clean_full_pipeline) {
    // NF_SKIP rather than `return`: a bare return is recorded as PASS, so a
    // machine without validation layers would report this test green while
    // nothing was verified.
    auto dev = rhi::create_device();
    if (!dev) NF_SKIP("no Vulkan device available");
    rhi::DeviceDesc ddesc{};
    ddesc.enable_validation = true;
    if (!dev->init(ddesc)) {
        NF_SKIP("validation test: device init failed");
    }
    if (!dev->validation_enabled()) {
        dev->shutdown();
        NF_SKIP("validation test: validation layers unavailable");
    }

    rhi::reset_validation_error_count();
    std::vector<Pixel> px;
    Renderer3D::Stats stats{};
    bool ok = render_full_chain(*dev, 64, 64,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
            auto cube = StaticMesh::create_cube(1.5f);
            auto h = meshes.add(std::move(cube));
            meshes.upload_all(*dev);
            PBRMaterialParams p{};
            set_rgb(p, 0.8f, 0.2f, 0.2f);
            auto m = renderer.materials().create_instance(*renderer.gbuffer_material(), p, "m");
            Entity e = world.create_entity();
            world.add<scene::Transform>(e, scene::Transform{});
            world.add<MeshComponent>(e, MeshComponent{h, m, true});
            renderer.set_directional_light(DirectionalLight{Vec3{-0.5f, -1, -0.3f}, Vec3{1, 1, 1}, 2.0f, true});
        }, px);
    NF_CHECK(ok);
    NF_CHECK(count_pixels_above(px, 30) > 150); // scene sanity; errors==0 is the real assert
    const u32 errors = rhi::validation_error_count();
    NF_CHECK_EQ(errors, 0u);
    dev->shutdown();
}
