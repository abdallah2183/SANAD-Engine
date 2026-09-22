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
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Rendering/RenderGraph.hpp>
#include <NF/Rendering/ResourceState.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/Sky.hpp>
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
namespace rendering = nf::rendering;
namespace assets = nf::assets;
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

// Sum over a centered square region (half-side `side` around the frame
// center): geometry response isolated from the procedural sky, which is
// identical across the compared frames and would otherwise compress
// on/off ratios toward 1.
u64 pixel_sum_center(const std::vector<Pixel>& px, u32 w, u32 h, u32 side) {
    u64 s = 0;
    const u32 cx = w / 2, cy = h / 2;
    for (u32 y = (cy > side) ? cy - side : 0; y < cy + side && y < h; ++y) {
        for (u32 x = (cx > side) ? cx - side : 0; x < cx + side && x < w; ++x) {
            const Pixel& p = px[usize(y) * w + x];
            s += u64(p.r) + p.g + p.b;
        }
    }
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
    // Runtime mesh -> MeshAsset -> NFME bytes -> MeshAsset -> Runtime mesh.
    //
    // This used to round-trip through rendering::mesh_asset, which wrote a
    // second, incompatible format ("NFM1") that the cooker rejects. There is one
    // format now (assets::MeshAsset, "NFME"), so the test that proves the round
    // trip is byte-exact is also the test that proves the format the cooker
    // writes is the one the runtime reads.
    auto cube = StaticMesh::create_cube(1.0f);

    const fs::path tmp = fs::temp_directory_path() / "nfme_roundtrip.nfmesh";
    auto asset = rendering::make_mesh_asset(*cube, assets::AssetId::generate(),
                                            "content://Meshes/rt.nfmesh");
    NF_CHECK(asset != nullptr);
    std::string err;
    NF_CHECK(asset->save_to_file(tmp.string(), err));

    auto reloaded = assets::MeshAsset::load_from_file(tmp.string(), err);
    NF_CHECK(reloaded != nullptr);
    if (reloaded == nullptr) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return;
    }
    NF_CHECK_EQ(reloaded->vertices.size(), cube->lods()[0].vertices.size());
    NF_CHECK_EQ(reloaded->indices.size(), cube->lods()[0].indices.size());
    NF_CHECK_EQ(reloaded->submeshes.size(), cube->lods()[0].submeshes.size());

    auto loaded = rendering::make_static_mesh(*reloaded, "cooked_cube");
    NF_CHECK(loaded != nullptr);
    if (loaded == nullptr) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return;
    }
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

    const fs::path tmp = fs::temp_directory_path() / "nfme_roundtrip_gpu.nfmesh";
    auto asset = rendering::make_mesh_asset(*cube, assets::AssetId::generate(),
                                            "content://Meshes/rt_gpu.nfmesh");
    std::string err;
    NF_CHECK(asset->save_to_file(tmp.string(), err));

    auto reloaded = assets::MeshAsset::load_from_file(tmp.string(), err);
    NF_CHECK(reloaded != nullptr);
    auto loaded = rendering::make_static_mesh(*reloaded, "cooked_cube_gpu");
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
    // A point straight ahead of the camera must project near NDC center.
    // Row-vector application (v' = v * M) with perspective divide.
    auto project = [&](float x, float y, float z) {
        const Mat4& vp = cam.view_projection;
        return vp.transform_point(Vec3{x, y, z});
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
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float expect = (r == c) ? 1.0f : 0.0f;
            NF_CHECK_NEAR(round.m[r][c], expect, 1e-3f);
        }
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
    NF_CHECK_NEAR(rw.objects[0].world.m[3][0], 1.0f, 1e-6f);
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
// Terrain layer splat: `uv1 = (slot, blend)` picks colours out of a palette
// bound at material binding 2. Slot 0 is the material itself, so a surface the
// mesh never classified still renders as the material.
// ---------------------------------------------------------------------------

namespace {
// A flat quad in the XY plane at the origin, sized to cover most of a 64x64
// frame shot from make_camera(3.0f) (half-extent of the view at z=0 is ~1.73).
// `uv1` is authored per vertex; position/normal are the same every time, so a
// caller varying `slot`/`blend` is varying only the splat the shader sees.
// blend is affine over a flat surface, so the column a pixel sits in is a
// direct readout of how far into the next layer it is.
std::unique_ptr<StaticMesh> make_splat_quad(float slot, float blend_left, float blend_right) {
    auto mesh = std::make_unique<StaticMesh>("splatquad");
    // The StaticMesh ctor already pushed the default LOD 0; write into it
    // rather than appending an empty LOD 1.
    MeshLOD& lod = mesh->lod(0);
    lod.vertices.resize(4);
    const float half = 1.7f;
    auto put = [&](u32 i, float x, float y, float blend) {
        Vertex& v = lod.vertices[i];
        v.position[0] = x;
        v.position[1] = y;
        v.position[2] = 0.0f;
        v.normal[0] = 0.0f;
        v.normal[1] = 0.0f;
        v.normal[2] = 1.0f;
        v.uv1[0] = slot;
        v.uv1[1] = blend;
    };
    put(0, -half, -half, blend_left);
    put(1, half, -half, blend_right);
    put(2, half, half, blend_right);
    put(3, -half, half, blend_left);
    lod.indices = {1u, 0u, 3u, 3u, 2u, 1u}; // create_quad's winding — the
    // natural (0,1,2) order is back-face culled from a +Z camera
    StaticMesh::compute_lod_bounds(lod);
    return mesh;
}

/// Renders `setup`'s scene and reads back gbuffer attachment 0 (base colour).
/// Pre-lighting and pre-tonemap, so a palette colour comes back unmixed with
/// shading and the assertion is about the splat alone.
bool readback_base_color(rhi::IGraphicsDevice& dev, Renderer3D& renderer, u32 w, u32 h,
                         const std::function<void(ecs::World&, MeshLibrary&, Renderer3D&)>& setup,
                         std::vector<Pixel>& out) {
    MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);
    ecs::World world;
    setup(world, meshes, renderer);
    scene::propagate_transforms(world);
    RenderWorld rw;
    nf::runtime::extract_render_objects(world, meshes, rw);

    rhi::TextureDesc target_desc{};
    target_desc.width = w;
    target_desc.height = h;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    auto target = dev.create_texture(target_desc);
    if (!target) return false;

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    if (!cmd || !fence) return false;
    cmd->begin();
    if (!renderer.render(*cmd, rw, make_camera(3.0f), *target)) return false;
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(kGpuTimeoutNs)) return false;
    dev.wait_idle();

    rhi::Texture* base_target = renderer.gbuffer_target(0);
    if (!base_target) return false;
    rhi::BufferDesc rb_desc{};
    rb_desc.size = usize(w) * h * 4;
    rb_desc.usage = rhi::BufferUsage::TransferDst;
    rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = dev.create_buffer(rb_desc);
    if (!rb) return false;
    auto c2 = dev.create_command_buffer();
    auto f2 = dev.create_fence(false);
    if (!c2 || !f2) return false;
    c2->begin();
    c2->copy_texture_to_buffer(*base_target, *rb, 0, 0, w, h, 0);
    c2->end();
    dev.submit(*c2, rhi::SubmitInfo{.signal_fence = f2.get()});
    if (!f2->wait(kGpuTimeoutNs)) return false;
    dev.wait_idle();
    auto* px = static_cast<const Pixel*>(rb->map());
    if (!px) return false;
    out.assign(px, px + usize(w) * h);
    rb->unmap();
    return true;
}

/// Mean RGB over a vertical band of rows at column `x` (u32 out params so the
/// caller's thresholds read as integers).
void column_mean(const std::vector<Pixel>& px, u32 w, u32 x, u32 row_half,
                 u32& out_r, u32& out_g, u32& out_b) {
    const u32 h = u32(px.size()) / w;
    u64 r = 0, g = 0, b = 0;
    u32 n = 0;
    const u32 cy = h / 2;
    const u32 y0 = (cy > row_half) ? cy - row_half : 0;
    for (u32 y = y0; y < cy + row_half && y < h; ++y) {
        const Pixel& p = px[usize(y) * w + x];
        r += p.r;
        g += p.g;
        b += p.b;
        ++n;
    }
    out_r = u32(r / n);
    out_g = u32(g / n);
    out_b = u32(b / n);
}
} // namespace

NF_TEST(terrain_splat_blends_two_layers_across_a_surface) {
    // The contract: a surface carrying `uv1 = (1, blend)` samples the palette
    // at slot 1 and slot 2 and mixes them by blend. blend sweeps 0 to 1 across
    // this quad, so the left of the frame must be red, the right green, and
    // the middle neither — a mix only the splat can produce.
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    NF_CHECK(renderer.init(dev, basic3d_shader_dir(), 64, 64));

    renderer.set_splat_layer_color(1, Vec3{1.0f, 0.0f, 0.0f}); // red
    renderer.set_splat_layer_color(2, Vec3{0.0f, 1.0f, 0.0f}); // green
    // The CPU mirror is what the shader will see; a stale mirror would make
    // the pixel assertions below uninterpretable.
    NF_CHECK_NEAR(renderer.splat_layer_color(1).x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(renderer.splat_layer_color(2).y, 1.0f, 1e-6f);

    std::vector<Pixel> px;
    NF_CHECK(readback_base_color(dev, renderer, 64, 64,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& r) {
            const StaticMeshHandle h = meshes.add(make_splat_quad(1.0f, 0.0f, 1.0f));
            NF_CHECK(meshes.upload_all(dev));
            PBRMaterialParams params{};
            set_rgb(params, 0.1f, 0.1f, 0.9f); // blue — nothing like red or green
            params.roughness = 0.5f;
            const MaterialHandle mat = r.materials().create_instance(
                *r.gbuffer_material(), params, "splat");
            const Entity e = world.create_entity();
            world.add<scene::Transform>(e, scene::Transform{});
            world.add<MeshComponent>(e, MeshComponent{h, mat, true});
        }, px));
    renderer.shutdown();

    // View half-extent at z=0 is tan(30)*3 ≈ 1.732 and the quad is ±1.7, so
    // columns 8/32/56 sit at blend ≈ 0.13 / 0.51 / 0.89.
    u32 lr = 0, lg = 0, lb = 0, cr = 0, cg = 0, cb = 0, rr = 0, rg = 0, rb = 0;
    column_mean(px, 64, 8, 8, lr, lg, lb);
    column_mean(px, 64, 32, 8, cr, cg, cb);
    column_mean(px, 64, 56, 8, rr, rg, rb);

    // Left: layer 1, red. Right: layer 2, green. Centre: a genuine mix — both
    // channels present and neither dominant.
    NF_CHECK(lr > 150 && lg < 70);
    NF_CHECK(rg > 150 && rr < 70);
    NF_CHECK(cr > 90 && cg > 90);
    NF_CHECK(u32(std::abs(int(cr) - int(cg))) < 40);
    // The material is still underneath it all; blue must not leak in where the
    // splat has fully taken over.
    NF_CHECK(lb < 70 && rb < 70);
}

NF_TEST(terrain_splat_leaves_an_unclassified_surface_as_the_material) {
    // uv1 = (0, 0) is the "no layers" default — and the gate is the value
    // itself, not a per-object flag, so this is also the exact regression test
    // for every mesh authored before the splat existed.
    const GpuFixture& f = require_gpu();
    auto& dev = *f.device;
    if (basic3d_shader_dir().empty()) NF_SKIP("required test asset missing");

    Renderer3D renderer;
    NF_CHECK(renderer.init(dev, basic3d_shader_dir(), 64, 64));
    // Palette layer 1 is set to red, so this only passes if the shader never
    // samples it for a surface that carries (0, 0).
    renderer.set_splat_layer_color(1, Vec3{1.0f, 0.0f, 0.0f});

    std::vector<Pixel> px;
    NF_CHECK(readback_base_color(dev, renderer, 64, 64,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& r) {
            const StaticMeshHandle h = meshes.add(make_splat_quad(0.0f, 0.0f, 0.0f));
            NF_CHECK(meshes.upload_all(dev));
            PBRMaterialParams params{};
            set_rgb(params, 0.1f, 0.1f, 0.9f);
            params.roughness = 0.5f;
            const MaterialHandle mat = r.materials().create_instance(
                *r.gbuffer_material(), params, "plain");
            const Entity e = world.create_entity();
            world.add<scene::Transform>(e, scene::Transform{});
            world.add<MeshComponent>(e, MeshComponent{h, mat, true});
        }, px));
    renderer.shutdown();

    u32 r0 = 0, g0 = 0, b0 = 0;
    column_mean(px, 64, 32, 8, r0, g0, b0);
    NF_CHECK(b0 > 150 && r0 < 70 && g0 < 70);
}

namespace {
/// One full-frame lit-and-tonemapped frame of a single quad `dist` units down
/// the camera axis. The quad is sized to still fill the frame at the farthest
/// distance the fog test uses, so every pixel of the readback is surface rather
/// than sky — the assertion is about the haze alone, and a sky pixel that moved
/// would be a bug, not evidence. `fog` is null for the reference frame.
///
/// The quad is parked on the -Z axis, so distance is camera_z - local_z, and
/// sliding `local_z` is the ONLY thing that changes between renders: same mesh,
/// same material, same light, same camera. The directional light has no
/// distance falloff and there are no point/spot lights, so the UNFOGGED colour
/// is identical at every depth — any pixel change is the fog term alone.
bool render_fog_frame(rhi::IGraphicsDevice& dev, float camera_z, float local_z, float quad_size,
                      const Renderer3D::FogParams* fog, const Vec3& albedo,
                      std::vector<Pixel>& out) {
    const Camera cam = make_camera(camera_z, 1.0f);
    return render_full_chain(dev, 64, 64,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
            const StaticMeshHandle h = meshes.add(StaticMesh::create_quad(quad_size));
            NF_CHECK(meshes.upload_all(dev));
            PBRMaterialParams params{};
            set_rgb(params, albedo.x, albedo.y, albedo.z);
            params.roughness = 0.5f;
            const MaterialHandle mat = renderer.materials().create_instance(
                *renderer.gbuffer_material(), params, "fogquad");
            const Entity e = world.create_entity();
            scene::Transform t{};
            t.local_z = local_z;
            world.add<scene::Transform>(e, t);
            world.add<MeshComponent>(e, MeshComponent{h, mat, true});
            renderer.set_ambient(0.25f);
            // Built inline rather than via base_spec(): SceneSpec and its helper
            // are declared in a later anonymous-namespace block below, so they are
            // not in scope here. The light itself is the simple case the spec
            // builds — one directional, no falloff, no points or spots.
            DirectionalLight d{};
            d.direction = {0.0f, -1.0f, -0.6f};
            d.color = {1.0f, 1.0f, 1.0f};
            d.intensity = 2.0f;
            renderer.set_directional_light(d);
            if (fog) renderer.set_fog(*fog);
        }, out, nullptr, &cam);
}

/// Channel means over the central square of the frame: the quad fills the frame
/// at every distance tested, so the centre is always surface. u32 out params so
/// the caller's thresholds read as integers.
void center_mean(const std::vector<Pixel>& px, u32& r, u32& g, u32& b) {
    u64 sr = 0, sg = 0, sb = 0;
    for (u32 y = 20; y < 44; ++y) {
        for (u32 x = 20; x < 44; ++x) {
            const Pixel& p = px[usize(y) * 64 + x];
            sr += p.r; sg += p.g; sb += p.b;
        }
    }
    r = u32(sr / 576); g = u32(sg / 576); b = u32(sb / 576);
}
} // namespace

NF_TEST(fog_fades_a_distant_surface_to_the_haze_colour) {
    // The contract: a surface past `end` becomes the haze colour, a surface
    // before `start` is untouched, and between the two it is a genuine mix that
    // moves with distance. The band is placed at 10..20 world units and the
    // quad is slid along the view axis, so the three frames differ ONLY in how
    // much air is between the camera and the surface.
    //
    // The haze colours are channel-opposite to the material: a warm-red lit
    // surface turned fully cyan, or fully orange, can only be the fog term — no
    // lighting or BRDF change produces it. That is also why the far comparison
    // is differential across two haze colours rather than against a predicted
    // byte: reproducing the tonemap + gamma here would add a third copy of an
    // operator that already has to match itself in two places.
    //
    // The mid assertions are ORDERINGS, not magnitudes, and that is deliberate:
    // fog mixes in linear light and the tonemap is per-channel monotone, so a
    // frame at f = 0.5 is strictly between the f = 0 and f = 1 frames in every
    // channel by construction. The gaps are ~30 u8, so quantisation cannot
    // collapse them, and the ordering is exactly what breaks if the fog term is
    // never applied (mid == near), saturates (mid == far), or is applied to the
    // wrong quantity (mid lands outside the range).
    const GpuFixture& f = require_gpu();

    const Vec3 albedo{0.90f, 0.20f, 0.10f};          // warm red — nothing like either haze
    Renderer3D::FogParams fog{};
    fog.enabled = true;
    fog.start = 10.0f;
    fog.end = 20.0f;

    // d = camera_z - local_z. near sits before the band, mid at its centre
    // (f = 0.5), far past `end` (f = 1). Quad half-size 17 covers the frame at
    // d = 25 (view half-extent tan(30)*25 ~= 14.4) with margin for the PCF-free
    // edge, so the centre sample is surface at every depth.
    const float kCameraZ = 4.0f;
    const float kSize = 34.0f;

    std::vector<Pixel> near_px, mid_px, far_cyan, far_orange;
    fog.color = {0.05f, 0.55f, 0.95f}; // cyan haze
    NF_CHECK(render_fog_frame(*f.device, kCameraZ, 0.0f, kSize, &fog, albedo, near_px));
    NF_CHECK(render_fog_frame(*f.device, kCameraZ, -11.0f, kSize, &fog, albedo, mid_px));
    NF_CHECK(render_fog_frame(*f.device, kCameraZ, -21.0f, kSize, &fog, albedo, far_cyan));
    fog.color = {0.95f, 0.45f, 0.05f}; // orange haze
    NF_CHECK(render_fog_frame(*f.device, kCameraZ, -21.0f, kSize, &fog, albedo, far_orange));

    u32 nr = 0, ng = 0, nb = 0, mr = 0, mg = 0, mb = 0;
    u32 cr = 0, cg = 0, cb = 0, orr = 0, org = 0, orb = 0;
    center_mean(near_px, nr, ng, nb);
    center_mean(mid_px, mr, mg, mb);
    center_mean(far_cyan, cr, cg, cb);
    center_mean(far_orange, orr, org, orb);

    NF_LOG_WARN(nf::LogCategory::Core,
        "fog ramp: near=({},{},{}) mid=({},{},{}) far_cyan=({},{},{}) far_orange=({},{},{})",
        nr, ng, nb, mr, mg, mb, cr, cg, cb, orr, org, orb);

    // Near: the lit material, warm and red-dominant. This is also the
    // before-start no-op — a surface the band has not reached must still read
    // as its own lit colour, not a pre-darkened version of it.
    NF_CHECK(nr > 120);
    NF_CHECK(nr > nb + 40);

    // Far with the cyan haze: blue-dominant. The material contributed almost no
    // blue, so this channel is the haze's alone.
    NF_CHECK(cb > 100);
    NF_CHECK(cb > cr + 40);

    // Far with the ORANGE haze: the SAME geometry, light and distance, only the
    // haze tint swapped — the surface must follow it and be red-dominant
    // instead. This is the pixel signature of "the surface became the fog
    // colour"; a fade that merely darkened toward a constant would leave both
    // far frames identical.
    NF_CHECK(orr > 100);
    NF_CHECK(orr > orb + 40);
    NF_CHECK(orr > cr + 40);
    NF_CHECK(cb > orb + 40);

    // Mid (f = 0.5) is a genuine mix: strictly between the unfogged surface and
    // the fully-hazed one in each channel that moves, and equal to neither.
    NF_CHECK(mr < nr - 10 && mr > cr + 10);
    NF_CHECK(mb > nb + 10 && mb < cb - 10);
    // Neither colour dominates at the centre of the band.
    NF_CHECK(u32(std::abs(int(mr) - int(mb))) < 60);
}

NF_TEST(fog_leaves_the_sky_and_a_surface_before_the_band_bit_identical) {
    // Two invariants in one comparison, because they share a failure mode: if
    // the fog term ever ran on a sky pixel, the sky would saturate to the haze
    // (a sky pixel reconstructs to the far plane, so any finite `end` gives
    // f = 1) and the whole frame would move. Tolerance 0 is the claim — the
    // disabled path and the before-start path must both be exact no-ops.
    //
    // The quad is deliberately SMALLER than the frame so sky is visible around
    // it: the sky exemption is structural (lighting.frag returns for sky pixels
    // before the fog call can run), not a flag, and this is the guard against
    // anyone moving the call or restructuring that early return.
    const GpuFixture& f = require_gpu();

    const Vec3 albedo{0.85f, 0.25f, 0.15f};
    // The band sits past the far plane entirely: surface at d = 4, band 50..100.
    // Every sky pixel reconstructs to d ~= 100, so a naive implementation
    // saturates the whole sky to haze here and the comparison fails loudly.
    Renderer3D::FogParams fog{};
    fog.enabled = true;
    fog.color = {0.15f, 0.55f, 0.85f};
    fog.start = 50.0f;
    fog.end = 100.0f;

    std::vector<Pixel> off, on;
    NF_CHECK(render_fog_frame(*f.device, 4.0f, 0.0f, 2.8f, nullptr, albedo, off));
    NF_CHECK(render_fog_frame(*f.device, 4.0f, 0.0f, 2.8f, &fog, albedo, on));
    NF_CHECK_EQ(off.size(), on.size());

    u32 moved = 0, surface_pixels = 0;
    for (usize i = 0; i < off.size(); ++i) {
        const Pixel& a = off[i];
        const Pixel& b = on[i];
        if (a.r != b.r || a.g != b.g || a.b != b.b) ++moved;
        // The lit quad is warm and red-dominant; the procedural sky behind it is
        // blue-dominant. This partition is what makes "nothing moved" meaningful
        // — a culled or unlit quad would leave the frame all sky and the test
        // would pass vacuously.
        if (a.r > a.b + 10) ++surface_pixels;
    }
    NF_LOG_WARN(nf::LogCategory::Core, "fog no-op: moved={} (of {}) surface_pixels={}",
                moved, off.size(), surface_pixels);

    NF_CHECK(surface_pixels > 800);
    NF_CHECK_EQ(moved, 0u);
}

NF_TEST(fog_round_trips_through_the_public_accessor) {
    // The CPU mirror the shader reads is written from `m_fog` on every frame;
    // an accessor that does not round-trip the 9 significant digits the engine
    // requires would silently re-author the band on upload.
    Renderer3D::FogParams expected{};
    expected.enabled = true;
    expected.color = {0.37f, 0.51f, 0.68f};
    expected.start = 13.5f;
    expected.end = 42.25f;

    Renderer3D renderer;
    renderer.set_fog(expected);
    const Renderer3D::FogParams& got = renderer.fog();
    NF_CHECK(got.enabled == expected.enabled);
    NF_CHECK_NEAR(got.color.x, expected.color.x, 1e-6f);
    NF_CHECK_NEAR(got.color.y, expected.color.y, 1e-6f);
    NF_CHECK_NEAR(got.color.z, expected.color.z, 1e-6f);
    NF_CHECK_NEAR(got.start, expected.start, 1e-6f);
    NF_CHECK_NEAR(got.end, expected.end, 1e-6f);
}

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
    bool shadows = true;
    u32 cascades = kMaxShadowCascades;
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
            d.shadows_enabled = spec.shadows;
            d.shadow_cascades = spec.cascades;
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
    NF_LOG_WARN(nf::LogCategory::Core, "lighting debug: center=({}, {}, {}) c1=({}, {}, {}) cTL=({}, {}, {}) cBR=({}, {}, {}) litcount={}",
                lit[32 * 64 + 32].r, lit[32 * 64 + 32].g, lit[32 * 64 + 32].b,
                lit[30 * 64 + 32].r, lit[30 * 64 + 32].g, lit[30 * 64 + 32].b,
                lit[0].r, lit[0].g, lit[0].b,
                lit[63 * 64 + 63].r, lit[63 * 64 + 63].g, lit[63 * 64 + 63].b,
                count_pixels_above(lit, 30));
    // Lit geometry in the middle; procedural sky (Phase 13) around it —
    // blue-dominant and clearly non-black, so sky and geometry stay
    // distinguishable the way black background used to be.
    // Orientation note: the readback buffer stores framebuffer row order (row 0
    // is the top of the image). Mat4::perspective carries a deliberate Y-flip
    // (m[1][1] negated) so that world-up ends up at the top of the framebuffer,
    // which puts world-up on readback TOP rows and the below-horizon haze on the
    // bottom ones. This is the reverse of the pre-Y-flip engine, where every
    // frame was drawn upside down and world-up really did land on the bottom
    // rows — the two counters below were written for that engine and sampled the
    // opposite corners of the frame.
    NF_CHECK(count_pixels_above(lit, 30) > 300);
    u32 sky = 0;
    u32 haze = 0;
    for (usize i = 0; i < lit.size(); ++i) {
        const u32 x = static_cast<u32>(i % 64);
        const u32 y = static_cast<u32>(i / 64);
        if (x < 6 || x > 57) {
            if (y < 6) {
                if (lit[i].b > lit[i].r + 10 && lit[i].b > 30) ++sky;
            } else if (y > 57) {
                if (lit[i].r > 20 && lit[i].g > 20 && lit[i].b > 20) ++haze;
            }
        }
    }
    NF_CHECK(sky > 60);  // world-up (readback top) shows blue sky
    NF_CHECK(haze > 60); // world-down (readback bottom) shows lit haze, not black
}

NF_TEST(directional_shadow_does_not_darken_an_unoccluded_face) {
    // The cube's front face points at the camera, and the light arrives from
    // above-and-in-front of it, so nothing stands between the two: enabling
    // shadows must leave that face untouched. Any darkening here is acne.
    //
    // This is the failure mode cascading makes easy to introduce, and it is
    // invisible in a shadow-coverage test — acne is *extra* shadow, and the
    // coverage tests only count that shadow exists. The shader compares a bias
    // against the shadow map in NDC, and a cascade's NDC depth span is its own
    // light-space depth range, so the WORLD offset a fixed bias buys is that
    // range. Fitting a cascade to the near slice makes the range far shorter
    // than the single fixed box it replaced, the world offset shrinks with it,
    // and a bias that used to clear the depth slope across a texel no longer
    // does. Every texel on the face then self-shadows and the face dims evenly.
    //
    // Both cascade counts are checked because they size the bias very
    // differently: one cascade fits the entire near-to-far range, so its texels
    // and its depth span are both far larger, and the derived bias has to come
    // out right in both regimes rather than only in the four-cascade default.
    const GpuFixture& f = require_gpu();

    for (const u32 cascades : {1u, kMaxShadowCascades}) {
        SceneSpec s = base_spec();
        s.cascades = cascades;

        std::vector<Pixel> shadowed, unshadowed;
        NF_CHECK(render_spec(*f.device, 64, 64, s, shadowed));
        s.shadows = false;
        NF_CHECK(render_spec(*f.device, 64, 64, s, unshadowed));

        const u64 with_shadows = pixel_sum_center(shadowed, 64, 64, 16);
        const u64 without = pixel_sum_center(unshadowed, 64, 64, 16);
        NF_LOG_WARN(nf::LogCategory::Core,
                    "unoccluded face, {} cascade(s): with_shadows={} without={}",
                    cascades, with_shadows, without);
        // Exact: with no occluder the shader's `dir_lo *= 1 - strength * 0` is
        // the identity, so the two frames must agree bit for bit on this face.
        NF_CHECK_EQ(with_shadows, without);
        NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    }
}

NF_TEST(directional_light) {
    const GpuFixture& f = require_gpu();
    SceneSpec s = base_spec();
    std::vector<Pixel> on, off;
    NF_CHECK(render_spec(*f.device, 64, 64, s, on));
    s.light_enabled = false;
    NF_CHECK(render_spec(*f.device, 64, 64, s, off));
    // Toggling the directional light changes the image; off ≈ ambient only.
    // Compared over the frame center (the cube): the sky is identical in
    // both frames, so a whole-frame ratio would be compressed toward 1.
    NF_CHECK(count_different_pixels(on, off) > 300);
    const u64 on_c = pixel_sum_center(on, 64, 64, 16);
    const u64 off_c = pixel_sum_center(off, 64, 64, 16);
    NF_LOG_WARN(nf::LogCategory::Core, "dirlight debug: on_c={} off_c={}", on_c, off_c);
    NF_CHECK(on_c > off_c * 2);
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
    // Cone aimed at the cube lights it; cone aimed at the sky does not.
    // Center-region sums: the sky is identical across frames, so whole-frame
    // ratios would be compressed toward 1.
    NF_CHECK(count_different_pixels(hit, none) > 100);
    NF_CHECK(count_different_pixels(hit, missed) > 100);
    const u64 hit_c = pixel_sum_center(hit, 64, 64, 16);
    const u64 missed_c = pixel_sum_center(missed, 64, 64, 16);
    const u64 none_c = pixel_sum_center(none, 64, 64, 16);
    NF_LOG_WARN(nf::LogCategory::Core, "spot debug: hit_c={} missed_c={} none_c={}", hit_c,
                missed_c, none_c);
    NF_CHECK(hit_c > missed_c * 2);
    NF_CHECK(missed_c < none_c * 2); // away ≈ unlit
}

// ---------------------------------------------------------------------------
// Local light shadows (Phase 21): point and spot lights must actually cast
// ---------------------------------------------------------------------------

/// The local-shadow scene: a wide receiver whose +Z face fills most of the
/// frame, plus a small occluder parked between that face and the light. The
/// camera looks down -Z from z=6, so the receiver's lit face is the centered
/// region `pixel_sum_center` measures.
///
/// `shadows` flips only the local light's `shadows_enabled` — the occluder is
/// present in EVERY frame, so any pixel difference between the two renders is
/// the shadow itself and not a change of geometry. That is what makes the
/// darkening assertion meaningful: comparing against an occluder-free frame
/// would mix "the occluder hid part of the wall" into the difference.
bool render_local_shadow_frame(rhi::IGraphicsDevice& dev, u32 W, u32 H,
                               const PointLight& point, const SpotLight& spot,
                               bool use_spot, bool shadows, bool occluder,
                               std::vector<Pixel>& out) {
    const Camera cam = make_camera(6.0f, float(W) / float(H));
    return render_full_chain(dev, W, H,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
            const StaticMeshHandle receiver = meshes.add(StaticMesh::create_cube(2.6f));
            const StaticMeshHandle blocker = meshes.add(StaticMesh::create_cube(0.5f));
            meshes.upload_all(dev);
            const SceneSpec s = base_spec();
            const MaterialHandle m = renderer.materials().create_instance(
                *renderer.gbuffer_material(), s.material, "lm");

            Entity a = world.create_entity();
            world.add<scene::Transform>(a, scene::Transform{});
            world.add<MeshComponent>(a, MeshComponent{receiver, m, true});
            if (occluder) {
                // On the segment between the light and the receiver's face, so
                // its umbra lands ON that face and is wider than the occluder's
                // own silhouette (it is nearer the light than the face is).
                Entity b = world.create_entity();
                scene::Transform bt{};
                bt.local_z = 2.6f;
                world.add<scene::Transform>(b, bt);
                world.add<MeshComponent>(b, MeshComponent{blocker, m, true});
            }

            // The directional light stays on in both frames and is shadowed
            // identically in both; it lights the face so the local shadow has a
            // bright surface to darken.
            renderer.set_directional_light(s.directional);
            if (use_spot) {
                SpotLight l = spot;
                l.shadows_enabled = shadows;
                renderer.add_spot_light(l);
            } else {
                PointLight l = point;
                l.shadows_enabled = shadows;
                renderer.add_point_light(l);
            }
        },
        out, nullptr, &cam);
}

NF_TEST(point_light_shadow_darkens_the_lit_face) {
    // The occluder is between the point light and the receiver's face, so a
    // working point-light shadow must darken that face. The six-tile atlas, the
    // per-tile projector, the shader's face selection and the clamped PCF all
    // have to agree for this pixel difference to appear at all; a wrong face
    // index or a mirrored tile returns a plausible depth from the wrong place
    // and the shadow lands somewhere else (or nowhere).
    const GpuFixture& f = require_gpu();
    const PointLight light{Vec3{0.0f, 0.0f, 4.0f}, Vec3{1.0f, 1.0f, 1.0f}, 8.0f, 12.0f};
    const SpotLight unused{};

    std::vector<Pixel> shadowed, lit;
    NF_CHECK(render_local_shadow_frame(*f.device, 64, 64, light, unused, false, true, true,
                                       shadowed));
    NF_CHECK(render_local_shadow_frame(*f.device, 64, 64, light, unused, false, false, true,
                                       lit));
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    NF_LOG_WARN(nf::LogCategory::Core, "point shadow: lit={} shadowed={}",
                pixel_sum_center(lit, 64, 64, 16), pixel_sum_center(shadowed, 64, 64, 16));
    NF_CHECK(pixel_sum_center(shadowed, 64, 64, 16) < pixel_sum_center(lit, 64, 64, 16));
    // The umbra is ~2x the occluder's silhouette on the face, and the frame
    // also picks up the penumbra ring around it, so the differing pixels
    // outnumber the occluder's ~8px screen footprint several times over.
    NF_CHECK(count_different_pixels(shadowed, lit) > 40);
}

NF_TEST(spot_light_shadow_darkens_the_lit_face) {
    // Same geometry as the point-light test, one tile instead of six: the spot
    // projector's fov is fitted to the outer cone, so a fragment inside the
    // cone has to project into the tile and find the occluder's depth there.
    const GpuFixture& f = require_gpu();
    const SpotLight light{Vec3{0.0f, 0.0f, 4.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f},
                          30.0f, 0.5f, 0.9f};
    const PointLight unused{};

    std::vector<Pixel> shadowed, lit;
    NF_CHECK(render_local_shadow_frame(*f.device, 64, 64, unused, light, true, true, true,
                                       shadowed));
    NF_CHECK(render_local_shadow_frame(*f.device, 64, 64, unused, light, true, false, true,
                                       lit));
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    NF_LOG_WARN(nf::LogCategory::Core, "spot shadow: lit={} shadowed={}",
                pixel_sum_center(lit, 64, 64, 16), pixel_sum_center(shadowed, 64, 64, 16));
    NF_CHECK(pixel_sum_center(shadowed, 64, 64, 16) < pixel_sum_center(lit, 64, 64, 16));
    NF_CHECK(count_different_pixels(shadowed, lit) > 40);
}

NF_TEST(spot_light_range_caps_its_reach) {
    // SpotLight::range replaced a literal 25.0 that lighting.frag and
    // kLocalShadowSpotDefaultFar each held a private copy of. The two frames
    // differ in exactly one field, so a pixel difference is the reach and
    // nothing else: a spot 12 units from the receiver, once with a reach that
    // stops short of it and once with one that covers it.
    //
    // Shadows are OFF in both — this is the light's falloff, not its shadow.
    // The projector fallback is covered by the same value reaching
    // lighting.frag and fit_spot, which is the point of moving it into one
    // field; asserting the projector separately would only re-test the
    // shadow path the three tests above already pin.
    const GpuFixture& f = require_gpu();
    const PointLight unused{};

    SpotLight light;
    light.position = {0.0f, 0.0f, 12.0f};
    light.direction = {0.0f, 0.0f, -1.0f};
    light.color = {1.0f, 1.0f, 1.0f};
    light.intensity = 30.0f;

    std::vector<Pixel> near, far;
    {
        SpotLight l = light;
        l.range = 3.0f;  // window = 0 at 12 units: the receiver is outside the pool
        NF_CHECK(render_local_shadow_frame(*f.device, 64, 64, unused, l, true, false, false, near));
    }
    {
        SpotLight l = light;
        l.range = 20.0f; // window ~0.87 at 12 units: the receiver is inside it
        NF_CHECK(render_local_shadow_frame(*f.device, 64, 64, unused, l, true, false, false, far));
    }
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    NF_LOG_WARN(nf::LogCategory::Core, "spot range: near={} far={}",
                pixel_sum_center(near, 64, 64, 16), pixel_sum_center(far, 64, 64, 16));
    NF_CHECK(pixel_sum_center(far, 64, 64, 16) > pixel_sum_center(near, 64, 64, 16));
    NF_CHECK(count_different_pixels(far, near) > 40);
}

NF_TEST(downward_spot_shadow_darkens_the_floor) {
    // `fit_spot` has to fall back to +X as up when its aim is parallel to world
    // +Y — i.e. a spot pointing straight down, which is exactly how the Basic3D
    // sample rigs its overhead light. A degenerate basis there NaNs the
    // projector and blanks the tile, and a wrong handedness mirrors the shadow
    // off to the far side of the cone; both still leave a lit cone on the
    // floor, so the shadow's PRESENCE is the only thing that separates them.
    //
    // A light this close to overhead throws every shadow nearly straight down
    // under its caster, so the occluder is lifted clear of the floor and the
    // camera looks at the floor under it: the puddle is the patch visible
    // between the cube's own base and the floor. A cube sitting ON the floor
    // hides its own puddle from any oblique viewpoint and the test would pass
    // for the wrong reason.
    const GpuFixture& f = require_gpu();

    const auto render = [&](bool shadows, std::vector<Pixel>& out) -> bool {
        Camera cam{};
        cam.position = {0.0f, 2.5f, 3.5f};
        cam.target = {0.0f, -0.6f, 0.0f}; // the floor patch the puddle lands on
        cam.up = {0.0f, 1.0f, 0.0f};
        cam.aspect = 1.0f;
        cam.fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
        cam.near_plane = 0.1f;
        cam.far_plane = 100.0f;
        nf::rendering::update_camera(cam);
        return render_full_chain(*f.device, 64, 64,
            [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
                const StaticMeshHandle floor = meshes.add(StaticMesh::create_cube(4.0f));
                const StaticMeshHandle cube = meshes.add(StaticMesh::create_cube(1.4f));
                meshes.upload_all(*f.device);
                const SceneSpec s = base_spec();
                const MaterialHandle m = renderer.materials().create_instance(
                    *renderer.gbuffer_material(), s.material, "floor");

                Entity fl = world.create_entity();
                scene::Transform ft{};
                ft.local_y = -2.6f; // top at -0.6
                world.add<scene::Transform>(fl, ft);
                world.add<MeshComponent>(fl, MeshComponent{floor, m, true});
                // Floating at the cone's centre: straight under the light, so
                // its puddle lands on the floor patch the camera is aimed at.
                // Sized to read as more than a handful of pixels at this range.
                Entity e = world.create_entity();
                scene::Transform t{};
                t.local_y = 1.2f;
                world.add<scene::Transform>(e, t);
                world.add<MeshComponent>(e, MeshComponent{cube, m, true});

                renderer.set_directional_light(s.directional);
                SpotLight light;
                light.position = {0.0f, 5.0f, 0.0f};
                light.direction = {0.0f, -1.0f, 0.0f}; // |dir.y| = 1: the degenerate-up case
                light.color = {1.0f, 1.0f, 1.0f};
                light.intensity = 40.0f;
                light.inner_angle_rad = 0.35f;
                light.outer_angle_rad = 0.6f;
                light.shadows_enabled = shadows;
                renderer.add_spot_light(light);
            },
            out, nullptr, &cam);
    };

    std::vector<Pixel> shadowed, lit;
    NF_CHECK(render(true, shadowed));
    NF_CHECK(render(false, lit));
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    NF_LOG_WARN(nf::LogCategory::Core, "downward spot: lit={} shadowed={}",
                pixel_sum_center(lit, 64, 64, 20), pixel_sum_center(shadowed, 64, 64, 20));
    NF_CHECK(pixel_sum_center(shadowed, 64, 64, 20) < pixel_sum_center(lit, 64, 64, 20));
    NF_CHECK(count_different_pixels(shadowed, lit) > 40);
}

NF_TEST(local_shadow_leaves_an_unshadowed_scene_bit_identical) {
    // No occluder: every fragment's shadow factor is exactly 0, so the
    // shader's `point_lo *= 1 - strength * 0` is the identity and the
    // shadowed frame must come back bit for bit identical to the unshadowed
    // one.
    //
    // This is the acne test for the whole local path — the atlas pass, the 28
    // per-tile projectors, the bias the CPU derived per tile — because a
    // self-shadowing face dims evenly and the equality breaks loudly, while a
    // coverage test above would count the acne as "the shadow works". It is
    // also the only test that would catch the derived bias being far too
    // SMALL: the point-light path has no cascade analogue to hide behind, and
    // a near face lit at a grazing angle is exactly where the texel-slope
    // reasoning has to hold.
    const GpuFixture& f = require_gpu();
    const PointLight light{Vec3{0.0f, 0.0f, 4.0f}, Vec3{1.0f, 1.0f, 1.0f}, 8.0f, 12.0f};
    const SpotLight unused{};

    std::vector<Pixel> on, off;
    NF_CHECK(render_local_shadow_frame(*f.device, 64, 64, light, unused, false, true, false, on));
    NF_CHECK(render_local_shadow_frame(*f.device, 64, 64, light, unused, false, false, false,
                                       off));
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    // Tolerance 0: the claim is bit-exact, matching the directional path's
    // unoccluded-face test.
    NF_CHECK_EQ(count_different_pixels(on, off, 0), 0u);
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

// ---------------------------------------------------------------------------
// Directional shadows (Phase 13)
// ---------------------------------------------------------------------------

namespace {

// Cube floating above a wide flat floor, lit at an angle: the cube throws a
// shadow onto the floor beside it. Shadows on/off differ only in the shadow
// factor, so lit pixels must be identical and shadowed ones strictly darker.
struct ShadowScene {
    bool shadows_on = true;
    /// World offset applied to the camera and to every entity, so the identical
    /// scene can be rendered at the origin or far away from it. Nothing about
    /// the shadow depends on where in the world the scene sits — that is the
    /// property the cascades exist to provide, and this is what measures it.
    float offset = 0.0f;
    /// Cascade count handed to the light. 1 is the single-fitted-map case.
    u32 cascades = kMaxShadowCascades;
    /// Whether the caster is present. Dropping it isolates self-shadowing: the
    /// floor alone must render identically with shadows on and off, which is the
    /// only way to tell floor acne from the cube's shadow, since both show up as
    /// "pixels that got darker" in the on/off comparison.
    bool cube = true;
    /// Shadow reach handed to the light. 0 is the "cast to the camera's far
    /// plane" sentinel, which is also the default, so leaving this alone keeps
    /// every other case in this file unchanged.
    float distance = 0.0f;
};

bool render_shadow_scene(rhi::IGraphicsDevice& dev, const ShadowScene& spec, std::vector<Pixel>& out) {
    const u32 W = 128, H = 128;
    Camera cam = make_camera(6.0f, float(W) / float(H));
    cam.position = {spec.offset, 0.0f, spec.offset + 6.0f};
    cam.target = {spec.offset, 0.0f, spec.offset};
    update_camera(cam);
    return render_full_chain(dev, W, H,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
            auto cube = StaticMesh::create_cube(1.5f);
            StaticMeshHandle h_cube = meshes.add(std::move(cube));
            auto slab = StaticMesh::create_cube(1.0f);
            StaticMeshHandle h_floor = meshes.add(std::move(slab));
            meshes.upload_all(dev);

            PBRMaterialParams gray{};
            set_rgb(gray, 0.8f, 0.8f, 0.8f);
            gray.roughness = 0.6f;
            MaterialHandle m = renderer.materials().create_instance(*renderer.gbuffer_material(),
                                                                    gray, "gray");

            Entity floor_e = world.create_entity();
            world.add<scene::Transform>(floor_e, scene::Transform{});
            world.get<scene::Transform>(floor_e)->local_x = spec.offset;
            world.get<scene::Transform>(floor_e)->local_y = -1.6f;
            world.get<scene::Transform>(floor_e)->local_z = spec.offset;
            world.get<scene::Transform>(floor_e)->scale_x = 14.0f;
            world.get<scene::Transform>(floor_e)->scale_y = 0.2f;
            world.get<scene::Transform>(floor_e)->scale_z = 14.0f;
            world.add<MeshComponent>(floor_e, MeshComponent{h_floor, m, true});

            Entity cube_e = world.create_entity();
            world.add<scene::Transform>(cube_e, scene::Transform{});
            world.get<scene::Transform>(cube_e)->local_x = spec.offset;
            world.get<scene::Transform>(cube_e)->local_y = 0.4f;
            world.get<scene::Transform>(cube_e)->local_z = spec.offset;
            if (spec.cube) {
                world.add<MeshComponent>(cube_e, MeshComponent{h_cube, m, true});
            }

            DirectionalLight d{Vec3{-0.55f, -1.0f, -0.35f}, Vec3{1, 1, 1}, 2.5f, true};
            d.shadows_enabled = spec.shadows_on;
            d.shadow_cascades = spec.cascades;
            d.shadow_distance = spec.distance;
            renderer.set_directional_light(d);
        },
        out, nullptr, &cam);
}

/// Pixels the shadow darkened, and pixels it wrongly brightened.
void count_shadow_delta(const std::vector<Pixel>& on, const std::vector<Pixel>& off,
                        u32& darkened, u32& brightened) {
    darkened = 0;
    brightened = 0;
    for (usize i = 0; i < on.size(); ++i) {
        const int dr = int(off[i].r) - int(on[i].r);
        const int dg = int(off[i].g) - int(on[i].g);
        const int db = int(off[i].b) - int(on[i].b);
        if (dr > 10 || dg > 10 || db > 10) ++darkened;
        if (dr < -10 || dg < -10 || db < -10) ++brightened;
    }
}

/// Total occlusion the shadow removed, summed over every pixel.
///
/// This is the blur-invariant way to ask "how much shadow is there". The count
/// above is not: a coarser cascade has a wider 3x3 PCF footprint, so its
/// penumbra drags a band of partially-lit pixels over any fixed threshold and
/// inflates the count without occluding anything extra. Summing instead of
/// counting is indifferent to how the same occlusion is spread across pixels,
/// so it moves only when shadow is genuinely gained or lost.
u64 shadow_energy(const std::vector<Pixel>& on, const std::vector<Pixel>& off) {
    u64 sum = 0;
    for (usize i = 0; i < on.size(); ++i) {
        const int d = int(off[i].g) - int(on[i].g);
        if (d > 0) sum += static_cast<u64>(d);
    }
    return sum;
}

} // namespace

NF_TEST(directional_shadow_floor_alone_does_not_self_shadow) {
    // With the caster removed, nothing in the scene can legally cast onto the
    // floor, so shadows on and off must agree everywhere. Whatever darkens is
    // the floor self-shadowing against its own depth — acne — and this is the
    // case a cascade makes dangerous, because the bias needed to clear it is a
    // WORLD distance derived from the cascade's texel size, and cascading
    // changes that texel size out from under any single tuned constant.
    //
    // This also disambiguates the coverage test: floor pixels that darken there
    // can be either the cube's shadow or floor acne, and only this scene tells
    // the two apart.
    const GpuFixture& f = require_gpu();
    for (const u32 cascades : {1u, kMaxShadowCascades}) {
        std::vector<Pixel> on, off;
        ShadowScene spec{};
        spec.shadows_on = true;
        spec.cube = false;
        spec.cascades = cascades;
        NF_CHECK(render_shadow_scene(*f.device, spec, on));
        spec.shadows_on = false;
        NF_CHECK(render_shadow_scene(*f.device, spec, off));

        u32 darkened = 0;
        u32 brightened = 0;
        count_shadow_delta(on, off, darkened, brightened);
        NF_LOG_WARN(nf::LogCategory::Core,
                    "floor acne probe, {} cascade(s): darkened={} brightened={}",
                    cascades, darkened, brightened);
        NF_CHECK_EQ(darkened, 0u);
        NF_CHECK_EQ(brightened, 0u);
        NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    }
}

NF_TEST(directional_shadow_darkens_floor) {
    const GpuFixture& f = require_gpu();
    std::vector<Pixel> on, off;
    NF_CHECK(render_shadow_scene(*f.device, ShadowScene{true}, on));
    NF_CHECK(render_shadow_scene(*f.device, ShadowScene{false}, off));
    NF_CHECK_EQ(on.size(), off.size());

    // Shadows only ever darken: no pixel may get brighter with them on.
    u32 darkened = 0;
    u32 brightened = 0;
    count_shadow_delta(on, off, darkened, brightened);
    // Not just "some pixels moved": floor acne would inflate this count with
    // shadow that is not the cube's. `directional_shadow_floor_alone_...` pins
    // that the floor contributes none of it, so this number is the cube's own
    // shadow and nothing else.
    NF_LOG_WARN(nf::LogCategory::Core, "shadow debug: darkened={} brightened={}", darkened,
                brightened);
    NF_CHECK(darkened > 30);   // the cube's shadow lands on the floor
    NF_CHECK_EQ(brightened, 0u); // ...and nothing else moved
    NF_CHECK(shadow_energy(on, off) > 0u);
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
}

NF_TEST(directional_shadow_survives_being_split_into_more_cascades) {
    // Cascading must redistribute shadow ACROSS the atlas without ever dropping
    // any of it: each fragment is tested against exactly one cascade's map, and
    // a fragment whose cascade happened to be fitted without its caster stops
    // being shadowed entirely. That failure is silent — the frame still renders,
    // it just quietly loses the occlusion — so it needs a test.
    //
    // The measured quantity is energy, not a darkened-pixel count. The count is
    // NOT comparable across cascade counts and looks like a bug if you compare
    // it: one cascade fits a box around the whole frustum, so its texel is ~15x
    // coarser, its PCF penumbra proportionally wider, and ~2.6x as many pixels
    // cross any fixed darkness threshold without a single extra photon being
    // blocked. Summing the darkening is invariant to that re-filtering, so it
    // changes only if occlusion is really gained or lost.
    const GpuFixture& f = require_gpu();

    std::vector<Pixel> off;
    ShadowScene off_spec{};
    off_spec.shadows_on = false;
    NF_CHECK(render_shadow_scene(*f.device, off_spec, off));

    const u64 single = [&] {
        std::vector<Pixel> on;
        ShadowScene spec{};
        spec.cascades = 1; // ShadowScene's default is the full cascade count
        NF_CHECK(render_shadow_scene(*f.device, spec, on));
        NF_CHECK_EQ(on.size(), off.size());
        // Sanity: the reference case has to be a real shadow, or every
        // comparison below is comparing nothing against nothing.
        NF_CHECK(shadow_energy(on, off) > 0u);
        return shadow_energy(on, off);
    }();

    for (const u32 cascades : {2u, 3u, kMaxShadowCascades}) {
        ShadowScene spec{};
        spec.cascades = cascades;
        std::vector<Pixel> on;
        NF_CHECK(render_shadow_scene(*f.device, spec, on));
        NF_CHECK_EQ(on.size(), off.size());

        u32 darkened = 0;
        u32 brightened = 0;
        count_shadow_delta(on, off, darkened, brightened);
        const u64 energy = shadow_energy(on, off);
        NF_LOG_WARN(nf::LogCategory::Core,
                    "cascade energy: cascades={} darkened={} energy={} (single={})", cascades,
                    darkened, energy, single);

        // Splitting can only ever resolve the same occlusion better — a sharper
        // map puts genuinely-dark pixels further down, never further up. So the
        // energy may rise modestly but must never fall, and a real caster loss
        // would show up as a collapse toward zero.
        NF_CHECK(energy >= single);
        NF_CHECK(energy <= single * 2);
        NF_CHECK_EQ(brightened, 0u);
        NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    }
}

NF_TEST(directional_shadow_distance_caps_the_shadow_reach) {
    // `shadow_distance` is the open-world knob: it stops the cascades at a chosen
    // range so the atlas is spent where a player can actually read it instead of
    // stretched to the camera's far plane. The contracts worth pinning are that
    // it BITES (a cap below the caster's distance removes the shadow) and that 0
    // means "no cap" rather than "no shadows" — 0 is a plausible-looking value
    // that a mangled default or a bad scene parse would produce.
    const GpuFixture& f = require_gpu();

    std::vector<Pixel> off;
    ShadowScene off_spec{};
    off_spec.shadows_on = false;
    NF_CHECK(render_shadow_scene(*f.device, off_spec, off));

    // The caster stands ~6.4 units from the camera, so a 0.5 reach cannot see it.
    ShadowScene capped{};
    capped.distance = 0.5f;
    std::vector<Pixel> capped_on;
    NF_CHECK(render_shadow_scene(*f.device, capped, capped_on));
    NF_CHECK_EQ(capped_on.size(), off.size());
    NF_CHECK_EQ(shadow_energy(capped_on, off), 0u);

    // The sentinel: 0 leaves the full camera far plane in play.
    ShadowScene uncapped{};
    uncapped.distance = 0.0f;
    std::vector<Pixel> uncapped_on;
    NF_CHECK(render_shadow_scene(*f.device, uncapped, uncapped_on));
    NF_CHECK_EQ(uncapped_on.size(), off.size());
    const u64 full = shadow_energy(uncapped_on, off);
    NF_CHECK(full > 0u);

    // A cap ABOVE the caster's distance must leave the shadow present. It is not
    // exactly equal to the uncapped case and cannot be: the cap also shortens the
    // far plane the cascades are split over, which reshuffles every slice and so
    // re-filters the shadow. What matters is that it survives — without this case
    // the two above would also pass if any non-zero distance simply disabled
    // shadows, which is not a cap at all, and is the likelier bug since it is
    // what a mistaken `> 0` treated as a boolean would do.
    ShadowScene generous{};
    generous.distance = 8.0f;
    std::vector<Pixel> generous_on;
    NF_CHECK(render_shadow_scene(*f.device, generous, generous_on));
    NF_CHECK_EQ(generous_on.size(), off.size());
    const u64 generous_energy = shadow_energy(generous_on, off);
    NF_LOG_WARN(nf::LogCategory::Core, "shadow reach: uncapped={} capped8={} capped0.5=0",
                full, generous_energy);
    NF_CHECK(generous_energy > 0u);
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
}

NF_TEST(directional_shadow_follows_the_camera_far_from_the_origin) {
    // This is the defect the cascades exist to fix, measured from GPU output
    // rather than inferred from the code. The shadow transform used to be a
    // single ortho box pinned to the WORLD ORIGIN (a +-12 box with the light
    // parked at dir * -20), so translating this exact scene away from (0,0,0)
    // did not dim the shadow — it deleted it: zero darkened pixels, in a scene
    // that shades well over a thousand at the origin.
    const GpuFixture& f = require_gpu();
    std::vector<Pixel> near_on, far_on, far_off;
    NF_CHECK(render_shadow_scene(*f.device, ShadowScene{true, 0.0f}, near_on));
    NF_CHECK(render_shadow_scene(*f.device, ShadowScene{true, 300.0f}, far_on));
    NF_CHECK(render_shadow_scene(*f.device, ShadowScene{false, 300.0f}, far_off));

    u32 near_dark = 0, near_bright = 0, far_dark = 0, far_bright = 0;
    count_shadow_delta(near_on, far_off, near_dark, near_bright);
    count_shadow_delta(far_on, far_off, far_dark, far_bright);
    NF_LOG_WARN(nf::LogCategory::Core,
                "far shadow debug: near_darkened={} far_darkened={} far_brightened={}",
                near_dark, far_dark, far_bright);

    NF_CHECK(far_dark > 30);
    NF_CHECK_EQ(far_bright, 0u);

    // ...and it must be the SAME shadow, not merely a shadow. The offset is a
    // rigid translation of camera, floor and caster together, so the cascades
    // see an identical frustum and the shaded pixel count is preserved. A
    // cascade that drifted or lost its far range would shade a different area.
    NF_CHECK(far_dark > near_dark * 8 / 10);
    NF_CHECK(far_dark < near_dark * 12 / 10);
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
}

NF_TEST(directional_shadow_single_cascade_still_shadows) {
    // Non-regression for the count knob: 1 cascade is the degenerate
    // single-fitted-map case, and it must still work — including far from the
    // origin, which is where the old single map failed.
    const GpuFixture& f = require_gpu();
    std::vector<Pixel> on, off;
    NF_CHECK(render_shadow_scene(*f.device, ShadowScene{true, 300.0f, 1u}, on));
    NF_CHECK(render_shadow_scene(*f.device, ShadowScene{false, 300.0f, 1u}, off));

    u32 darkened = 0, brightened = 0;
    count_shadow_delta(on, off, darkened, brightened);
    NF_LOG_WARN(nf::LogCategory::Core, "single-cascade debug: darkened={} brightened={}",
                darkened, brightened);
    NF_CHECK(darkened > 30);
    NF_CHECK_EQ(brightened, 0u);
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
}

// ---------------------------------------------------------------------------
// Procedural sky (Phase 13)
// ---------------------------------------------------------------------------

NF_TEST(procedural_sky_covers_background) {
    const GpuFixture& f = require_gpu();
    const u32 W = 128, H = 128;
    const Camera cam = make_camera(3.0f, float(W) / float(H));
    std::vector<Pixel> px;
    // Empty scene: every pixel takes the sky branch (no geometry at all).
    NF_CHECK(render_full_chain(*f.device, W, H,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
            (void)world;
            (void)meshes;
            renderer.set_directional_light(
                DirectionalLight{Vec3{-0.5f, -1.0f, -0.3f}, Vec3{1, 1, 1}, 2.0f, true});
        },
        px, nullptr, &cam));

    // Nothing black anywhere: sky replaces the old near-black clear.
    NF_CHECK_EQ(px.size(), usize(W) * H);
    NF_CHECK_EQ(count_pixels_above(px, 15), u32(W) * H);
    // Majority blue-dominant (zenith gradient + horizon), ground haze below.
    u32 blue = 0;
    for (const Pixel& p : px) {
        if (p.b > p.r + 8) ++blue;
    }
    NF_LOG_WARN(nf::LogCategory::Core, "sky debug: blue={} total={}", blue, px.size());
    NF_CHECK(blue > px.size() * 35 / 100);
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
}

// ---------------------------------------------------------------------------
// Transparency (Phase 22): alpha < 1 routes a surface to the forward pass,
// which is the only place a transparent surface can be drawn at all — the
// gbuffer holds one surface per pixel, so a pane would otherwise average two
// surfaces into one and destroy both.
// ---------------------------------------------------------------------------

namespace {

/// The transparency scene: a wide opaque wall straight ahead of the camera,
/// and a pane floating between the wall and the eye. The wall's +Z face fills
/// the centered region `pixel_sum_center` reads, so every comparison below is
/// "the same wall with and without something in front of it".
///
/// `pane_alpha` is the whole routing rule: 1.0 draws the pane through the
/// deferred path like any other surface, < 1.0 draws it through the forward
/// pass over the lit image, and NaN would draw it nowhere. The wall is present
/// in every variant so nothing about the comparison can be a change of
/// geometry behind the pane.
bool render_transparency_frame(rhi::IGraphicsDevice& dev, u32 W, u32 H,
                               float pane_alpha, bool pane_present,
                               std::vector<Pixel>& out,
                               Renderer3D::Stats* out_stats = nullptr) {
    const Camera cam = make_camera(5.0f, float(W) / float(H));
    return render_full_chain(dev, W, H,
        [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
            const StaticMeshHandle wall_h = meshes.add(StaticMesh::create_cube(3.0f));
            const StaticMeshHandle pane_h = meshes.add(StaticMesh::create_quad(2.0f));
            NF_CHECK(meshes.upload_all(dev));

            const SceneSpec s = base_spec();
            renderer.set_ambient(0.25f);
            const MaterialHandle wall_mat = renderer.materials().create_instance(
                *renderer.gbuffer_material(), s.material, "wall");

            // The pane is its own material instance: alpha is a per-instance
            // parameter, so the wall keeps its opaque instance untouched.
            // Albedo is chosen >= the wall's in every channel: a blended pixel
            // is a per-channel convex combination of the two surfaces, so a
            // pane brighter everywhere is the case where "between the other
            // two" is provable. A darker pane blends to a darker image and the
            // ordering test would be testing nothing.
            PBRMaterialParams pane_params = s.material;
            pane_params.base_color[0] = 0.90f;
            pane_params.base_color[1] = 0.92f;
            pane_params.base_color[2] = 0.95f;
            pane_params.base_color[3] = pane_alpha;
            pane_params.roughness = 0.2f;
            const MaterialHandle pane_mat = renderer.materials().create_instance(
                *renderer.gbuffer_material(), pane_params, "pane");

            Entity wall = world.create_entity();
            scene::Transform wt{};
            wt.local_z = -1.0f;
            world.add<scene::Transform>(wall, wt);
            world.add<MeshComponent>(wall, MeshComponent{wall_h, wall_mat, true});

            if (pane_present) {
                Entity pane = world.create_entity();
                scene::Transform pt{};
                pt.local_z = 0.5f; // between the wall and the camera
                world.add<scene::Transform>(pane, pt);
                // Not a shadow caster: a pane that threw a shadow would make the
                // comparisons below about the shadow instead of the blend.
                world.add<MeshComponent>(pane, MeshComponent{pane_h, pane_mat, true});
            }

            renderer.set_directional_light(s.directional);
        },
        out, out_stats, &cam);
}

} // namespace

NF_TEST(transparent_pane_blends_over_opaque_geometry) {
    // Three frames of the same wall: bare, with a translucent pane in front,
    // and with the same pane drawn opaque. Alpha blending is the only thing
    // that can put the middle frame strictly between the other two — an
    // unblended pane replaces the wall (equal to the third), and a pane the
    // forward pass never drew leaves the wall alone (equal to the first).
    const GpuFixture& f = require_gpu();
    std::vector<Pixel> bare, blended, opaque_pane;
    Renderer3D::Stats stats{};
    NF_CHECK(render_transparency_frame(*f.device, 64, 64, 1.0f, false, bare));
    NF_CHECK(render_transparency_frame(*f.device, 64, 64, 0.45f, true, blended, &stats));
    NF_CHECK(render_transparency_frame(*f.device, 64, 64, 1.0f, true, opaque_pane));

    const u64 bare_s = pixel_sum_center(bare, 64, 64, 16);
    const u64 blend_s = pixel_sum_center(blended, 64, 64, 16);
    const u64 opaque_s = pixel_sum_center(opaque_pane, 64, 64, 16);
    NF_LOG_WARN(nf::LogCategory::Core, "transparency: bare={} blended={} opaque_pane={}",
                bare_s, blend_s, opaque_s);

    NF_CHECK(blend_s > bare_s);
    NF_CHECK(blend_s < opaque_s);
    // The routed count is what proves the pane took the forward path rather
    // than landing in the gbuffer: a surface written into the gbuffer would be
    // opaque by construction and read 0 here.
    NF_CHECK_EQ(stats.transparent_objects, 1u);
    NF_CHECK(stats.draw_calls >= 2u);
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
}

NF_TEST(forward_pass_renders_an_opaque_surface_like_the_deferred_path) {
    // One cube, two rendering paths: alpha 0.9999 routes it through the forward
    // pass, and the same cube at 1.0 goes through the deferred path. Both
    // evaluate the same direct_lighting() body out of brdf.glsl against the
    // same shadow atlases, so the two frames have to agree. What separates them
    // is only what each path had to quantize: the gbuffer packs albedo, normal
    // and surface into 8-bit channels and reconstructs the position from depth,
    // while the forward path keeps everything at full precision. That is a
    // fraction of a percent per channel, so a large disagreement anywhere is a
    // divergent BRDF — one path reading a different light, a different shadow
    // tile, or a different view — not a precision artifact.
    const GpuFixture& f = require_gpu();
    const Camera cam = make_camera(3.0f, 1.0f);

    auto render = [&](float alpha, std::vector<Pixel>& out) {
        return render_full_chain(*f.device, 64, 64,
            [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
                const StaticMeshHandle h = meshes.add(StaticMesh::create_cube(1.5f));
                NF_CHECK(meshes.upload_all(*f.device));
                const SceneSpec s = base_spec();
                renderer.set_ambient(0.25f);
                PBRMaterialParams p = s.material;
                p.base_color[3] = alpha;
                const MaterialHandle m = renderer.materials().create_instance(
                    *renderer.gbuffer_material(), p, "cube");
                Entity e = world.create_entity();
                world.add<scene::Transform>(e, scene::Transform{});
                world.add<MeshComponent>(e, MeshComponent{h, m, true});
                renderer.set_directional_light(s.directional);
            },
            out, nullptr, &cam);
    };

    std::vector<Pixel> deferred, forward;
    NF_CHECK(render(1.0f, deferred));     // opaque: the deferred path
    NF_CHECK(render(0.9999f, forward));   // routed, but one part in 10^4 of the
                                          // background under a fully covering
                                          // surface — below 8-bit resolution

    const u64 deferred_s = pixel_sum_center(deferred, 64, 64, 16);
    const u64 forward_s = pixel_sum_center(forward, 64, 64, 16);
    const double diff_pct = deferred_s
        ? 100.0 * double(std::abs(int64_t(forward_s) - int64_t(deferred_s))) / double(deferred_s)
        : 0.0;
    NF_LOG_WARN(nf::LogCategory::Core, "forward vs deferred: d={} f={} diff_pct={:.3f}",
                deferred_s, forward_s, diff_pct);
    // The surface still draws — a no-op forward pass would leave the sky.
    NF_CHECK(count_different_pixels(deferred, forward, 4) < 64 * 64 / 8);
    // ...and it draws the same surface the deferred path did.
    NF_CHECK(diff_pct < 5.0);
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
}

NF_TEST(transparent_pane_leaves_the_rest_of_the_frame_bit_identical) {
    // The pass LOADs the lit HDR image and draws over it, so a pane must touch
    // only its own pixels. Everything outside the pane's silhouette is a
    // barrier-and-nothing-else and has to come back bit for bit identical to a
    // frame that has no pane in it at all — tolerance 0 is the claim.
    //
    // The pane is parked to the right of the cube so the two never overlap;
    // the sampled region is on the cube's side, where the pane has no pixels.
    const GpuFixture& f = require_gpu();
    const Camera cam = make_camera(4.0f, 1.0f);

    auto render = [&](bool pane, std::vector<Pixel>& out) {
        return render_full_chain(*f.device, 64, 64,
            [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
                const StaticMeshHandle cube_h = meshes.add(StaticMesh::create_cube(1.5f));
                const StaticMeshHandle pane_h = meshes.add(StaticMesh::create_quad(1.2f));
                NF_CHECK(meshes.upload_all(*f.device));
                const SceneSpec s = base_spec();
                renderer.set_ambient(0.25f);
                const MaterialHandle cube_mat = renderer.materials().create_instance(
                    *renderer.gbuffer_material(), s.material, "cube");

                Entity cube = world.create_entity();
                scene::Transform ct{};
                ct.local_x = -0.9f;
                world.add<scene::Transform>(cube, ct);
                world.add<MeshComponent>(cube, MeshComponent{cube_h, cube_mat, true});

                if (pane) {
                    // Warm albedo against the blue sky behind it, so the pane's
                    // footprint is findable as a channel shift rather than a
                    // brightness shift.
                    PBRMaterialParams pane_params = s.material;
                    pane_params.base_color[0] = 0.95f;
                    pane_params.base_color[1] = 0.35f;
                    pane_params.base_color[2] = 0.25f;
                    pane_params.base_color[3] = 0.5f;
                    const MaterialHandle pane_mat = renderer.materials().create_instance(
                        *renderer.gbuffer_material(), pane_params, "pane");
                    Entity p = world.create_entity();
                    scene::Transform pt{};
                    pt.local_x = 1.6f; // right of the cube, no overlap
                    pt.local_z = -0.5f;
                    world.add<scene::Transform>(p, pt);
                    world.add<MeshComponent>(p, MeshComponent{pane_h, pane_mat, true});
                }

                renderer.set_directional_light(s.directional);
            },
            out, nullptr, &cam);
    };

    std::vector<Pixel> without, with_pane;
    NF_CHECK(render(false, without));
    NF_CHECK(render(true, with_pane));

    // The left half of the frame: cube and the sky behind it. The pane is on
    // the right half, so not one pixel here may move.
    u32 moved = 0;
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 32; ++x) {
            const Pixel& a = without[usize(y) * 64 + x];
            const Pixel& b = with_pane[usize(y) * 64 + x];
            if (a.r != b.r || a.g != b.g || a.b != b.b) ++moved;
        }
    }
    NF_LOG_WARN(nf::LogCategory::Core, "transparency spillover: moved={} (of 2048)", moved);
    NF_CHECK_EQ(moved, 0u);
    // ...and the pane did draw, somewhere on the right half. Any channel
    // moving counts: the pane's hue shifts red up and blue down against the
    // sky, which is a bigger signal than its brightness.
    u32 pane_pixels = 0;
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 32; x < 64; ++x) {
            const Pixel& a = without[usize(y) * 64 + x];
            const Pixel& b = with_pane[usize(y) * 64 + x];
            if (std::abs(int(a.r) - int(b.r)) > 8 || std::abs(int(a.g) - int(b.g)) > 8 ||
                std::abs(int(a.b) - int(b.b)) > 8) {
                ++pane_pixels;
            }
        }
    }
    NF_CHECK(pane_pixels > 50);
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
}

NF_TEST(two_transparent_panes_blend_in_back_to_front_order) {
    // Two overlapping alpha panes. The forward pass sorts far-to-near and
    // disables depth writes, so the nearer pane blends OVER the farther one and
    // the overlap carries both colours. Which pane is nearer is the only thing
    // that flips between the two renders, so the overlap's dominant channel
    // must flip with it — that is the pixel-level signature of the sort. A
    // reversed order, or depth writes left on (the nearer pane then fails its
    // own depth test and never draws), leaves the overlap showing only the
    // farther pane in BOTH frames.
    const GpuFixture& f = require_gpu();
    const Camera cam = make_camera(5.0f, 1.0f);

    // Uniform dark backdrop instead of the procedural sky: the sky gradient is
    // bright enough to wash out a 0.5-alpha pane lit at this intensity, and a
    // constant background lets a pixel be classified as "a pane covers this"
    // without an empty-scene reference render.
    SkyParams dark{};
    dark.enabled = false;

    // Fixed geometry: one pane at x=-0.7 (farther, 6.0 from the camera) and one
    // at x=+0.7 (nearer, 5.0), overlapping in the middle. Only the colours move
    // between renders — draw_far/draw_near exist so each pane's lone footprint
    // can be measured and the overlap located as their intersection.
    auto render = [&](bool far_is_red, bool draw_far, bool draw_near,
                      std::vector<Pixel>& out, Renderer3D::Stats* stats) {
        return render_full_chain(*f.device, 64, 64,
            [&](ecs::World& world, MeshLibrary& meshes, Renderer3D& renderer) {
                const StaticMeshHandle pane_h = meshes.add(StaticMesh::create_quad(2.4f));
                NF_CHECK(meshes.upload_all(*f.device));
                const SceneSpec s = base_spec();
                renderer.set_ambient(0.25f);
                renderer.set_sky(dark);
                // Shadows would add a factor both panes share; falloff is
                // irrelevant at this range, so the light is shadowless.
                DirectionalLight d = s.directional;
                d.shadows_enabled = false;
                renderer.set_directional_light(d);

                PBRMaterialParams red{};
                set_rgb(red, 0.9f, 0.1f, 0.1f);
                red.base_color[3] = 0.5f;
                red.roughness = 0.5f;
                PBRMaterialParams green{};
                set_rgb(green, 0.1f, 0.9f, 0.1f);
                green.base_color[3] = 0.5f;
                green.roughness = 0.5f;
                const MaterialHandle red_mat = renderer.materials().create_instance(
                    *renderer.gbuffer_material(), red, "red");
                const MaterialHandle green_mat = renderer.materials().create_instance(
                    *renderer.gbuffer_material(), green, "green");

                auto place = [&](float x, float z, MaterialHandle m) {
                    Entity e = world.create_entity();
                    scene::Transform t{};
                    t.local_x = x;
                    t.local_z = z;
                    world.add<scene::Transform>(e, t);
                    world.add<MeshComponent>(e, MeshComponent{pane_h, m, true});
                };
                if (draw_far)  place(-0.7f, -1.0f, far_is_red ? red_mat : green_mat);
                if (draw_near) place(0.7f, 0.0f, far_is_red ? green_mat : red_mat);
            },
            out, stats, &cam);
    };

    std::vector<Pixel> only_far, only_near, green_front, red_front;
    Renderer3D::Stats stats_far{}, stats_near{}, stats_green{}, stats_red{};
    NF_CHECK(render(true, true, false, only_far, &stats_far));
    NF_CHECK(render(true, false, true, only_near, &stats_near));
    NF_CHECK(render(true, true, true, green_front, &stats_green));
    NF_CHECK(render(false, true, true, red_front, &stats_red));

    // Both panes must route through the forward pass, not the deferred one —
    // that is what puts a blend in the frame at all.
    NF_CHECK_EQ(stats_green.transparent_objects, 2u);
    NF_CHECK_EQ(stats_red.transparent_objects, 2u);

    // Pane footprint against the constant backdrop. The two solo frames share
    // one background, so a disagreement here means the backdrop is not uniform
    // and the footprint test is measuring nothing.
    const Pixel bg = only_far[0];
    NF_CHECK(std::abs(int(bg.r) - int(only_near[0].r)) <= 2);
    NF_CHECK(std::abs(int(bg.g) - int(only_near[0].g)) <= 2);
    NF_CHECK(std::abs(int(bg.b) - int(only_near[0].b)) <= 2);

    auto pane_covers = [&](const Pixel& p) {
        return std::abs(int(p.r) - int(bg.r)) > 12
            || std::abs(int(p.g) - int(bg.g)) > 12
            || std::abs(int(p.b) - int(bg.b)) > 12;
    };

    u32 far_only = 0, near_only = 0, overlap = 0;
    int green_over_red = 0; // sum(g - r) over the overlap, green in front
    int red_over_green = 0; // sum(r - g) over the overlap, red in front
    for (usize i = 0; i < green_front.size(); ++i) {
        const bool far_here = pane_covers(only_far[i]);
        const bool near_here = pane_covers(only_near[i]);
        if (far_here && !near_here) ++far_only;
        if (near_here && !far_here) ++near_only;
        if (!(far_here && near_here)) continue;
        ++overlap;
        green_over_red += int(green_front[i].g) - int(green_front[i].r);
        red_over_green += int(red_front[i].r) - int(red_front[i].g);
    }
    NF_LOG_WARN(nf::LogCategory::Core,
                "pane blend: far_only={} near_only={} overlap={} g_over_r={} r_over_g={}",
                far_only, near_only, overlap, green_over_red, red_over_green);

    // Each pane has a region it owns alone: both actually drew.
    NF_CHECK(far_only > 20);
    NF_CHECK(near_only > 20);
    // The panes really overlap.
    NF_CHECK(overlap > 40);
    // The nearer pane is on top in each frame, with a margin wide enough that
    // a single stray edge pixel cannot carry the check.
    NF_CHECK(green_over_red > int(overlap) * 5);
    NF_CHECK(red_over_green > int(overlap) * 5);
    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
}

NF_TEST(point_light_shadow_follows_a_moving_light) {
    // Every local-shadow test above holds the light still, which proves a
    // shadow is cast but not the P2 DoD — a moving point shadow. Movement is a
    // different failure mode: a cube-face tile whose projector was built once
    // and never rebuilt, a tile index that does not track `select_face`, or a
    // matrix that mirrors rather than translates the light. Each pins the
    // umbra while the light moves, and a static-light test passes through all
    // of them.
    //
    // Same CSM methodology as the rest of the local-shadow suite — the caster
    // is in both frames of every render and only `shadows_enabled` differs — so
    // geometry and the directional term are constant and the positive part of
    // (lit - shadowed) is the shadow's own darkness weight. The sky branch
    // depends on the directional light alone, and the occluder is in every
    // frame, so off-face pixels carry exactly zero weight and the centroid
    // needs no region mask.
    //
    // Light at (lx, ly, 4) with the blocker at z = 2.6 and the receiver's +Z
    // face at z = 1.3 lands the umbra centre at -0.9286 * (lx, ly): the puddle
    // sits on the opposite side of the face from the light. Two mirror pairs,
    // one per face axis — (0.8, 0) / (-0.8, 0) drives the tile's U and
    // (0, 0.8) / (0, -0.8) its V. 0.8 keeps the whole umbra inside the face's
    // ±1.3 half-width (0.9 would clip a sliver off the edge) and at 96x96 the
    // pair is ~26 px apart, well above the 15 px threshold.
    const GpuFixture& f = require_gpu();
    const SpotLight unused{};
    const u32 W = 96, H = 96;
    const double C = double(W) / 2.0 - 0.5;

    // Weighted centroid of the darkness, in pixels. Returns -1 when no fragment
    // got darker, which is the pinning failure made explicit instead of hiding
    // behind a divide by zero.
    auto centroid = [&](const std::vector<Pixel>& shadowed,
                        const std::vector<Pixel>& lit) -> std::array<double, 2> {
        double wsum = 0.0, sx = 0.0, sy = 0.0;
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const usize i = usize(y) * W + x;
                const int d = int(lit[i].r) + int(lit[i].g) + int(lit[i].b)
                              - int(shadowed[i].r) - int(shadowed[i].g)
                              - int(shadowed[i].b);
                if (d <= 0) continue;
                wsum += double(d);
                sx += double(x) * double(d);
                sy += double(y) * double(d);
            }
        }
        if (wsum <= 0.0) return {-1.0, -1.0};
        return {sx / wsum, sy / wsum};
    };

    auto pair = [&](float lx, float ly) -> std::array<double, 2> {
        const PointLight light{Vec3{lx, ly, 4.0f}, Vec3{1.0f, 1.0f, 1.0f}, 8.0f, 12.0f};
        std::vector<Pixel> on, off;
        NF_CHECK(render_local_shadow_frame(*f.device, W, H, light, unused, false, true, true, on));
        NF_CHECK(render_local_shadow_frame(*f.device, W, H, light, unused, false, false, true,
                                           off));
        NF_CHECK_EQ(rhi::validation_error_count(), 0u);
        return centroid(on, off);
    };

    const std::array<double, 2> px = pair(0.8f, 0.0f);
    const std::array<double, 2> nx = pair(-0.8f, 0.0f);
    const std::array<double, 2> py = pair(0.0f, 0.8f);
    const std::array<double, 2> ny = pair(0.0f, -0.8f);
    NF_LOG_WARN(nf::LogCategory::Core,
                "moving point shadow centroids: +X=({:.1f},{:.1f}) -X=({:.1f},{:.1f}) "
                "+Y=({:.1f},{:.1f}) -Y=({:.1f},{:.1f}) centre={:.1f}",
                px[0], px[1], nx[0], nx[1], py[0], py[1], ny[0], ny[1], C);

    for (const std::array<double, 2>& c : {px, nx, py, ny}) {
        NF_CHECK(c[0] > 0.0);
        NF_CHECK(c[1] > 0.0);
    }

    // The umbra must land on opposite sides of the frame centre along the axis
    // the light moved on, and well separated. A projector that pins, mirrors or
    // reuses a stale tile puts both puddles on the same spot and fails here.
    // The straddle form is used rather than a signed comparison because the
    // geometry guarantees the two puddles are on opposite sides, while which
    // one is left of centre also depends on the readback's row order — not a
    // contract this test should pin.
    NF_CHECK((px[0] < C) != (nx[0] < C));
    NF_CHECK(std::abs(px[0] - nx[0]) > 15.0);
    NF_CHECK((py[1] < C) != (ny[1] < C));
    NF_CHECK(std::abs(py[1] - ny[1]) > 15.0);

    // The cross-axis centroid must stay near the centre: the only check that
    // catches a transposed cube-face basis, which a single axis alone cannot
    // see. The umbra is ~8 px across here, so 10 px is a tight budget.
    NF_CHECK(std::abs(px[1] - C) < 10.0);
    NF_CHECK(std::abs(nx[1] - C) < 10.0);
    NF_CHECK(std::abs(py[0] - C) < 10.0);
    NF_CHECK(std::abs(ny[0] - C) < 10.0);
}
