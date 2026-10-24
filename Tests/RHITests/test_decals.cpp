// Tests/RHITests/test_decals.cpp — projected screen-space decals (Phase 36)
//
// Two layers of evidence, and the second is the one that matters:
//
//   1. The CPU contract: the authored size is baked into the world matrix, the
//      inverse really round-trips, the projected screen rect culls and clamps,
//      and the far-to-near sort is deterministic. These are exact arithmetic
//      claims, so they are pinned without a GPU.
//   2. The GPU claims: a decal changes GBuffer0/GBuffer2 at the pixels it
//      covers, it does NOT touch the normal or emissive attachments, a
//      disabled/offscreen/backfacing decal leaves the frame bit-identical, and
//      the surface attachment's alpha (emission strength) survives a partial-
//      coverage blend. That last one is why `ColorAttachment` grew per-
//      attachment alpha factors in this phase.
//
// Tests skip (never silently pass) when the GPU or the decal shaders are
// unavailable. `DecalFrame::supported` is checked by every GPU test so a build
// configured without the shader pair reports SKIPPED instead of a false green.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/Decals.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/RenderWorld.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <vector>

namespace {

using nf::i32;
using nf::u32;
using nf::u8;
using nf::usize;
using nf::Vec3;
using nf::Vec4;
using nf::Mat4;
using namespace nf::test;
using namespace nf::rendering;
namespace ecs = nf::ecs;
namespace scene = nf::scene;
namespace rhi = nf::rhi;
namespace runtime = nf::runtime;
namespace fs = std::filesystem;

#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

constexpr u32 kSize = 64;
constexpr float kPi = 3.14159265358979323846f;

fs::path basic3d_shader_dir() {
    fs::path p(NF_BASIC3D_SHADER_DIR);
    if (!p.empty() && fs::exists(p)) return p;
    return {};
}

Camera make_camera(float z = 3.0f, float aspect = 1.0f) {
    Camera cam{};
    cam.position = {0, 0, z};
    cam.target = {0, 0, 0};
    cam.up = {0, 1, 0};
    cam.aspect = aspect;
    cam.fov_y_rad = 60.0f * kPi / 180.0f;
    cam.near_plane = 0.1f;
    cam.far_plane = 100.0f;
    update_camera(cam);
    return cam;
}

u32 count_different_pixels(const std::vector<Pixel>& a, const std::vector<Pixel>& b,
                           u8 tolerance = 4) {
    NF_CHECK_EQ(a.size(), b.size());
    u32 n = 0;
    for (usize i = 0; i < a.size(); ++i) {
        const int dr = std::abs(int(a[i].r) - int(b[i].r));
        const int dg = std::abs(int(a[i].g) - int(b[i].g));
        const int db = std::abs(int(a[i].b) - int(b[i].b));
        const int da = std::abs(int(a[i].a) - int(b[i].a));
        if (dr > tolerance || dg > tolerance || db > tolerance || da > tolerance) ++n;
    }
    return n;
}

/// A decal whose local +Z faces `yaw_deg` degrees away from world +Z. Size is
/// baked, matching what Runtime::build_render_world does.
DecalInstance decal_at(const Vec3& position, const Vec3& size, float yaw_deg,
                       MaterialHandle material, float opacity = 1.0f,
                       bool enabled = true, u32 id = 1) {
    const Mat4 entity_world =
        Mat4::rotate_y(yaw_deg * kPi / 180.0f) * Mat4::translate(position);
    return make_decal(id, decal_world_matrix(entity_world, size), material, opacity,
                      enabled);
}

/// The GPU scene. A 3x3x3 cube centred at the origin gives the camera a flat
/// +Z face at z = 1.5 to project onto. The decal box sits across that face.
struct DecalScene {
    bool decal_present = true;
    bool decal_enabled = true;
    Vec3 decal_position{0.0f, 0.0f, 1.4f};
    Vec3 decal_size{1.0f, 1.0f, 0.4f};
    float decal_yaw_deg = 0.0f;
    float decal_opacity = 1.0f;
    float decal_alpha = 1.0f;
    float wall_emission = 0.0f;
};

struct DecalFrame {
    std::vector<Pixel> base;      // GBuffer0 — decals write here
    std::vector<Pixel> normal;    // GBuffer1 — decals must not write here
    std::vector<Pixel> surface;   // GBuffer2 — decals write here, alpha preserved
    std::vector<Pixel> emissive;  // GBuffer3 — decals must not write here
    std::vector<Pixel> image;     // tonemapped output — proves graph ordering
    Renderer3D::Stats stats{};
    bool supported = false;
};

/// Copies one texture to a host-visible buffer and unpacks RGBA8 pixels.
bool read_texture(rhi::IGraphicsDevice& dev, rhi::Texture* tex, std::vector<Pixel>& out) {
    if (tex == nullptr) return false;
    rhi::BufferDesc rb_desc{};
    rb_desc.size = usize(kSize) * kSize * 4;
    rb_desc.usage = rhi::BufferUsage::TransferDst;
    rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = dev.create_buffer(rb_desc);
    if (!rb) return false;

    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    if (!cmd || !fence) return false;
    cmd->begin();
    cmd->copy_texture_to_buffer(*tex, *rb, 0, 0, kSize, kSize, 0);
    cmd->end();
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(kGpuTimeoutNs)) return false;
    dev.wait_idle();

    auto* px = static_cast<const Pixel*>(rb->map());
    if (px == nullptr) return false;
    out.assign(px, px + usize(kSize) * kSize);
    rb->unmap();
    return true;
}

/// Renders one frame of the wall (+ optional decal) and reads back every
/// attachment the decal contract talks about. Returns false on setup/render
/// failure; `out.supported` distinguishes "decal shaders missing" (a skip)
/// from a real regression.
bool render_decal_frame(rhi::IGraphicsDevice& dev, const DecalScene& spec,
                        DecalFrame& out) {
    if (basic3d_shader_dir().empty()) return false;

    Renderer3D renderer;
    if (!renderer.init(dev, basic3d_shader_dir(), kSize, kSize)) return false;
    MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);

    const StaticMeshHandle wall = meshes.add(StaticMesh::create_cube(3.0f));
    if (!meshes.upload_all(dev)) {
        renderer.shutdown();
        return false;
    }

    PBRMaterialParams wall_params{};
    wall_params.base_color[0] = 0.5f;
    wall_params.base_color[1] = 0.5f;
    wall_params.base_color[2] = 0.5f;
    wall_params.roughness = 0.7f;
    wall_params.ao = 0.6f;
    wall_params.emission_strength = spec.wall_emission;
    const MaterialHandle wall_mat = renderer.materials().create_instance(
        *renderer.gbuffer_material(), wall_params, "decal-wall");

    PBRMaterialParams decal_params{};
    decal_params.base_color[0] = 0.9f;
    decal_params.base_color[1] = 0.1f;
    decal_params.base_color[2] = 0.1f;
    decal_params.base_color[3] = spec.decal_alpha;
    decal_params.roughness = 0.2f;
    decal_params.ao = 1.0f;
    const MaterialHandle decal_mat = renderer.materials().create_instance(
        *renderer.gbuffer_material(), decal_params, "decal-red");

    ecs::World world;
    const ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    world.add<MeshComponent>(e, MeshComponent{wall, wall_mat, true});
    scene::propagate_transforms(world);

    RenderWorld rw;
    runtime::extract_render_objects(world, meshes, rw);
    if (spec.decal_present) {
        rw.decals.push_back(decal_at(spec.decal_position, spec.decal_size,
                                     spec.decal_yaw_deg, decal_mat, spec.decal_opacity,
                                     spec.decal_enabled));
    }

    rhi::TextureDesc target_desc{};
    target_desc.width = kSize;
    target_desc.height = kSize;
    target_desc.format = rhi::Format::R8G8B8A8_UNorm;
    target_desc.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled |
                        rhi::ImageUsage::TransferSrc;
    auto target = dev.create_texture(target_desc);
    auto cmd = dev.create_command_buffer();
    auto fence = dev.create_fence(false);
    if (!target || !cmd || !fence) {
        renderer.shutdown();
        return false;
    }

    cmd->begin();
    const bool rendered = renderer.render(*cmd, rw, make_camera(), *target);
    cmd->end();
    if (!rendered) {
        renderer.shutdown();
        return false;
    }
    dev.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(kGpuTimeoutNs)) {
        renderer.shutdown();
        return false;
    }
    dev.wait_idle();

    out.stats = renderer.last_stats();
    out.supported = renderer.decals_supported();

    bool ok = read_texture(dev, target.get(), out.image) &&
              read_texture(dev, renderer.gbuffer_target(0), out.base) &&
              read_texture(dev, renderer.gbuffer_target(1), out.normal) &&
              read_texture(dev, renderer.gbuffer_target(2), out.surface) &&
              read_texture(dev, renderer.gbuffer_target(3), out.emissive);
    renderer.shutdown();
    return ok;
}

/// Asserts two frames agree in EVERY attachment, tolerance 0. Used for the
/// no-op cases (disabled, offscreen, backfacing): a pass that LOADs and writes
/// nothing must be indistinguishable from not having the pass at all.
void expect_frames_identical(const DecalFrame& a, const DecalFrame& b) {
    NF_CHECK_EQ(count_different_pixels(a.base, b.base, 0), 0u);
    NF_CHECK_EQ(count_different_pixels(a.normal, b.normal, 0), 0u);
    NF_CHECK_EQ(count_different_pixels(a.surface, b.surface, 0), 0u);
    NF_CHECK_EQ(count_different_pixels(a.emissive, b.emissive, 0), 0u);
    NF_CHECK_EQ(count_different_pixels(a.image, b.image, 0), 0u);
}

} // namespace

// ---------------------------------------------------------------------------
// CPU contract: size baking and inverse
// ---------------------------------------------------------------------------

NF_TEST(decal_world_matrix_bakes_size_before_the_entity_transform) {
    // The shader tests the unit cube. The authored size must therefore be part
    // of the matrix, applied FIRST (row-vector: v * S * W), so the box is
    // `size` large in the entity's own space and then placed by the entity.
    const Mat4 entity = Mat4::translate(Vec3{1.0f, 2.0f, 3.0f});
    const Mat4 world = decal_world_matrix(entity, Vec3{2.0f, 4.0f, 6.0f});

    // A local corner (0.5,0.5,0.5) lands at half the authored size, offset.
    const Vec4 corner = world * Vec4{0.5f, 0.5f, 0.5f, 1.0f};
    NF_CHECK_NEAR(corner.x, 2.0f, 1e-5f);
    NF_CHECK_NEAR(corner.y, 4.0f, 1e-5f);
    NF_CHECK_NEAR(corner.z, 6.0f, 1e-5f);

    // Scale is diagonal, translation is row 3.
    NF_CHECK_NEAR(world.m[0][0], 2.0f, 1e-5f);
    NF_CHECK_NEAR(world.m[1][1], 4.0f, 1e-5f);
    NF_CHECK_NEAR(world.m[2][2], 6.0f, 1e-5f);
    NF_CHECK_NEAR(world.m[3][0], 1.0f, 1e-5f);
    NF_CHECK_NEAR(world.m[3][1], 2.0f, 1e-5f);
    NF_CHECK_NEAR(world.m[3][2], 3.0f, 1e-5f);
}

NF_TEST(decal_world_matrix_keeps_a_degenerate_size_from_becoming_nan) {
    // A zero axis would collapse the box and make the inverse singular. The
    // clamp turns a bad scene line into "invisible decal", never NaNs.
    const Mat4 world = decal_world_matrix(Mat4::identity(), Vec3{0.0f, -1.0f, 0.0f});
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            NF_CHECK(std::isfinite(world.m[r][c]));
        }
    }
    NF_CHECK(world.m[0][0] >= 1e-4f);
    NF_CHECK(world.m[1][1] >= 1e-4f);
    NF_CHECK(world.m[2][2] >= 1e-4f);
    const Mat4 inv = world.inverse();
    NF_CHECK(std::isfinite(inv.m[0][0]));
}

NF_TEST(decal_world_matrix_rotation_sets_the_facing_axis) {
    // Local +Z is the decal's outward axis. In the row-vector matrix that is
    // row 2, and a yaw of 90 degrees must point it at +X. This is the contract
    // the renderer's push constant and the shader's backface test both read.
    const Mat4 yawed = decal_world_matrix(Mat4::rotate_y(90.0f * kPi / 180.0f),
                                          Vec3{1.0f, 1.0f, 1.0f});
    const Vec3 forward{yawed.m[2][0], yawed.m[2][1], yawed.m[2][2]};
    NF_CHECK_NEAR(forward.x, 1.0f, 1e-5f);
    NF_CHECK_NEAR(forward.y, 0.0f, 1e-5f);
    NF_CHECK_NEAR(forward.z, 0.0f, 1e-5f);

    // And a local +Z offset is placed along that axis, not world +Z.
    const Vec4 tip = yawed * Vec4{0.0f, 0.0f, 0.5f, 1.0f};
    NF_CHECK_NEAR(tip.x, 0.5f, 1e-5f);
    NF_CHECK_NEAR(tip.z, 0.0f, 1e-5f);
}

NF_TEST(make_decal_inverse_round_trips_a_composed_transform) {
    const Mat4 entity = Mat4::rotate_y(37.0f * kPi / 180.0f) *
                        Mat4::translate(Vec3{2.0f, -1.0f, 4.0f});
    const DecalInstance decal = make_decal(
        7, decal_world_matrix(entity, Vec3{1.5f, 0.5f, 2.0f}), MaterialHandle{});

    const Vec3 local{0.3f, -0.2f, 0.1f};
    const Vec4 world = decal.world * Vec4{local, 1.0f};
    const Vec4 back = decal.inverse_world * world;
    NF_CHECK_NEAR(back.x, local.x, 1e-4f);
    NF_CHECK_NEAR(back.y, local.y, 1e-4f);
    NF_CHECK_NEAR(back.z, local.z, 1e-4f);
    NF_CHECK_NEAR(back.w, 1.0f, 1e-4f);
}

NF_TEST(update_decal_inverse_refreshes_after_an_edit) {
    // A stale inverse silently projects from the old place; the only way to
    // catch it is to move the decal and round-trip again.
    DecalInstance decal =
        make_decal(1, decal_world_matrix(Mat4::identity(), Vec3::one), MaterialHandle{});
    decal.world = decal_world_matrix(Mat4::translate(Vec3{5.0f, 0.0f, 0.0f}),
                                    Vec3{2.0f, 2.0f, 2.0f});
    // Before the refresh the OLD inverse is still in place.
    update_decal_inverse(decal);

    const Vec3 local{-0.25f, 0.5f, 0.0f};
    const Vec4 world = decal.world * Vec4{local, 1.0f};
    const Vec4 back = decal.inverse_world * world;
    NF_CHECK_NEAR(back.x, local.x, 1e-4f);
    NF_CHECK_NEAR(back.y, local.y, 1e-4f);
    NF_CHECK_NEAR(back.z, local.z, 1e-4f);
}

NF_TEST(make_decal_clamps_opacity_and_carries_the_id) {
    const DecalInstance low = make_decal(42, Mat4::identity(), MaterialHandle{}, -1.0f);
    const DecalInstance high = make_decal(43, Mat4::identity(), MaterialHandle{}, 2.0f);
    NF_CHECK_NEAR(low.opacity, 0.0f, 1e-6f);
    NF_CHECK_NEAR(high.opacity, 1.0f, 1e-6f);
    NF_CHECK_EQ(low.id, 42u);
    NF_CHECK_EQ(high.id, 43u);
    NF_CHECK(low.enabled);
}

// ---------------------------------------------------------------------------
// CPU contract: projected screen rect
// ---------------------------------------------------------------------------

NF_TEST(decal_screen_rect_covers_the_centre_for_a_facing_decal) {
    const Camera cam = make_camera(3.0f);
    const DecalInstance decal =
        make_decal(1, decal_world_matrix(Mat4::identity(), Vec3{1.0f, 1.0f, 0.2f}),
                   MaterialHandle{});

    const DecalScreenRect rect =
        decal_screen_rect(decal, cam.view_projection, kSize, kSize);
    NF_CHECK(rect.visible);
    NF_CHECK(rect.width > 0);
    NF_CHECK(rect.height > 0);
    // A 1-unit box 3 units from the eye at 60 degrees vertical FOV is roughly
    // a third of the frame; the exact bound is loose enough for rounding but
    // tight enough to catch a rect built from the wrong plane.
    NF_CHECK(rect.width >= 15 && rect.width <= 30);
    NF_CHECK(rect.height >= 15 && rect.height <= 30);
    NF_CHECK(rect.x <= 32 && rect.x + rect.width > 32);
    NF_CHECK(rect.y <= 32 && rect.y + rect.height > 32);

    // Wholly inside the framebuffer.
    NF_CHECK(rect.x >= 0 && rect.y >= 0);
    NF_CHECK(rect.x + rect.width <= i32(kSize));
    NF_CHECK(rect.y + rect.height <= i32(kSize));
}

NF_TEST(decal_screen_rect_rejects_a_decal_behind_the_camera) {
    const Camera cam = make_camera(3.0f);
    // The camera looks down -Z from z=3, so z=5 is behind it.
    const DecalInstance decal =
        make_decal(1, decal_world_matrix(Mat4::translate(Vec3{0.0f, 0.0f, 5.0f}),
                                        Vec3{1.0f, 1.0f, 1.0f}),
                   MaterialHandle{});
    const DecalScreenRect rect =
        decal_screen_rect(decal, cam.view_projection, kSize, kSize);
    NF_CHECK(!rect.visible);
}

NF_TEST(decal_screen_rect_rejects_a_decal_outside_the_frame) {
    const Camera cam = make_camera(3.0f);
    const DecalInstance decal =
        make_decal(1, decal_world_matrix(Mat4::translate(Vec3{100.0f, 0.0f, 0.0f}),
                                        Vec3{1.0f, 1.0f, 1.0f}),
                   MaterialHandle{});
    const DecalScreenRect rect =
        decal_screen_rect(decal, cam.view_projection, kSize, kSize);
    NF_CHECK(!rect.visible);
}

NF_TEST(decal_screen_rect_rejects_a_decal_beyond_the_far_plane) {
    const Camera cam = make_camera(3.0f); // far = 100
    const DecalInstance decal =
        make_decal(1, decal_world_matrix(Mat4::translate(Vec3{0.0f, 0.0f, -200.0f}),
                                        Vec3{1.0f, 1.0f, 1.0f}),
                   MaterialHandle{});
    const DecalScreenRect rect =
        decal_screen_rect(decal, cam.view_projection, kSize, kSize);
    NF_CHECK(!rect.visible);
}

NF_TEST(decal_screen_rect_rejects_a_disabled_decal) {
    const Camera cam = make_camera(3.0f);
    const DecalInstance decal =
        make_decal(1, decal_world_matrix(Mat4::identity(), Vec3::one),
                   MaterialHandle{}, 1.0f, /*enabled=*/false);
    const DecalScreenRect rect =
        decal_screen_rect(decal, cam.view_projection, kSize, kSize);
    NF_CHECK(!rect.visible);
}

NF_TEST(decal_screen_rect_is_conservative_when_the_box_crosses_the_eye) {
    // One corner behind the near plane makes the projected hull meaningless.
    // The safe answer is the whole viewport: the shader's per-pixel box test
    // still rejects everything outside the decal, while a wrong scissor would
    // silently remove visible pixels.
    const Camera cam = make_camera(3.0f);
    const DecalInstance decal =
        make_decal(1, decal_world_matrix(Mat4::translate(Vec3{0.0f, 0.0f, 3.0f}),
                                        Vec3{1.0f, 1.0f, 10.0f}),
                   MaterialHandle{});
    const DecalScreenRect rect =
        decal_screen_rect(decal, cam.view_projection, kSize, kSize);
    NF_CHECK(rect.visible);
    NF_CHECK_EQ(rect.x, 0);
    NF_CHECK_EQ(rect.y, 0);
    NF_CHECK_EQ(rect.width, i32(kSize));
    NF_CHECK_EQ(rect.height, i32(kSize));
}

NF_TEST(decal_screen_rect_keeps_a_box_whose_centre_is_offscreen) {
    // The rect comes from all eight corners, not from the centre: a wide decal
    // whose centre projects outside can still overlap the frame.
    const Camera cam = make_camera(3.0f);
    const DecalInstance decal =
        make_decal(1, decal_world_matrix(Mat4::translate(Vec3{1.9f, 0.0f, 0.0f}),
                                        Vec3{2.0f, 1.0f, 0.2f}),
                   MaterialHandle{});
    const DecalScreenRect rect =
        decal_screen_rect(decal, cam.view_projection, kSize, kSize);
    NF_CHECK(rect.visible);
    NF_CHECK(rect.x < i32(kSize)); // the left edge reaches into the frame
    NF_CHECK(rect.x + rect.width > 0);
}

NF_TEST(decal_screen_rect_is_empty_for_a_zero_sized_target) {
    const Camera cam = make_camera(3.0f);
    const DecalInstance decal =
        make_decal(1, decal_world_matrix(Mat4::identity(), Vec3::one),
                   MaterialHandle{});
    const DecalScreenRect rect = decal_screen_rect(decal, cam.view_projection, 0, 0);
    NF_CHECK(!rect.visible);
}

// ---------------------------------------------------------------------------
// CPU contract: deterministic far-to-near ordering
// ---------------------------------------------------------------------------

NF_TEST(sort_decals_orders_far_to_near_with_an_id_tie_break) {
    // Alpha-over requires the nearest decal to draw last. Equal distances are
    // ordered by ascending id so a frame compared across runs cannot depend on
    // the sort's incidental order.
    std::vector<DecalInstance> decals;
    decals.push_back(make_decal(50, decal_world_matrix(Mat4::translate(Vec3{5, 0, 0}),
                                                       Vec3::one), MaterialHandle{}));
    decals.push_back(make_decal(10, decal_world_matrix(Mat4::translate(Vec3{1, 0, 0}),
                                                       Vec3::one), MaterialHandle{}));
    decals.push_back(make_decal(21, decal_world_matrix(Mat4::translate(Vec3{2, 0, 0}),
                                                       Vec3::one), MaterialHandle{}));
    decals.push_back(make_decal(30, decal_world_matrix(Mat4::translate(Vec3{3, 0, 0}),
                                                       Vec3::one), MaterialHandle{}));
    decals.push_back(make_decal(20, decal_world_matrix(Mat4::translate(Vec3{-2, 0, 0}),
                                                       Vec3::one), MaterialHandle{}));

    sort_decals_far_to_near(std::span<DecalInstance>(decals), Vec3{0, 0, 0});

    const std::array<u32, 5> want{50, 30, 20, 21, 10};
    for (usize i = 0; i < want.size(); ++i) {
        NF_CHECK_EQ(decals[i].id, want[i]);
    }
}

NF_TEST(sort_decals_is_stable_across_repeated_sorts) {
    std::vector<DecalInstance> decals;
    for (u32 i = 0; i < 8; ++i) {
        // Four distances, two decals each: the tie-break has to decide.
        const float x = float(i / 2) * 2.0f;
        decals.push_back(make_decal(100 + i,
                                    decal_world_matrix(Mat4::translate(Vec3{x, 0, 0}),
                                                       Vec3::one),
                                    MaterialHandle{}));
    }
    sort_decals_far_to_near(std::span<DecalInstance>(decals), Vec3{0, 0, 0});
    std::array<u32, 8> first{};
    for (usize i = 0; i < first.size(); ++i) first[i] = decals[i].id;

    // Perturb the input order, sort again: same deterministic answer.
    std::reverse(decals.begin(), decals.end());
    sort_decals_far_to_near(std::span<DecalInstance>(decals), Vec3{0, 0, 0});
    for (usize i = 0; i < first.size(); ++i) {
        NF_CHECK_EQ(decals[i].id, first[i]);
    }
}

NF_TEST(sort_decals_includes_disabled_entries_in_the_ordering) {
    // Disabled decals are skipped by the renderer but still sorted, so toggling
    // one never changes the relative order of the enabled ones.
    std::vector<DecalInstance> decals;
    decals.push_back(make_decal(1, decal_world_matrix(Mat4::translate(Vec3{1, 0, 0}),
                                                      Vec3::one),
                                MaterialHandle{}, 1.0f, false));
    decals.push_back(make_decal(2, decal_world_matrix(Mat4::translate(Vec3{4, 0, 0}),
                                                      Vec3::one),
                                MaterialHandle{}));
    decals.push_back(make_decal(3, decal_world_matrix(Mat4::translate(Vec3{2, 0, 0}),
                                                      Vec3::one),
                                MaterialHandle{}));

    sort_decals_far_to_near(std::span<DecalInstance>(decals), Vec3{0, 0, 0});
    NF_CHECK_EQ(decals[0].id, 2u);
    NF_CHECK_EQ(decals[1].id, 3u);
    NF_CHECK_EQ(decals[2].id, 1u);
    NF_CHECK(!decals[2].enabled);
}

// ---------------------------------------------------------------------------
// GPU: the decal reaches the gbuffer
// ---------------------------------------------------------------------------

NF_TEST(decal_projects_its_material_onto_the_wall) {
    const GpuFixture& f = require_gpu();
    DecalScene bare;
    bare.decal_present = false;
    DecalScene with;
    DecalFrame plain, decaled;
    NF_CHECK(render_decal_frame(*f.device, bare, plain));
    NF_CHECK(render_decal_frame(*f.device, with, decaled));
    if (!decaled.supported) NF_SKIP("decal shaders are not available");

    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    NF_CHECK_EQ(decaled.stats.decals_submitted, 1u);
    NF_CHECK_EQ(decaled.stats.decals_recorded, 1u);
    NF_CHECK_EQ(plain.stats.decals_submitted, 0u);
    NF_CHECK_EQ(plain.stats.decals_recorded, 0u);

    const usize center = usize(kSize / 2) * kSize + kSize / 2;
    // GBuffer0: the red decal replaced the gray wall under it.
    NF_CHECK(decaled.base[center].r > plain.base[center].r + 60);
    NF_CHECK(decaled.base[center].g + 60 < plain.base[center].g);
    NF_CHECK(decaled.base[center].b + 60 < plain.base[center].b);
    // GBuffer2: roughness became the decal's 0.2 (≈51) rather than the
    // wall's 0.7 (≈178). This proves the surface attachment is blended too.
    NF_CHECK(decaled.surface[center].g < 90);
    NF_CHECK(plain.surface[center].g > 150);

    // The change is local to the decal: a corner pixel is untouched.
    const usize corner = usize(2) * kSize + 2;
    NF_CHECK_EQ(int(decaled.base[corner].r), int(plain.base[corner].r));
    NF_CHECK_EQ(int(decaled.base[corner].g), int(plain.base[corner].g));

    NF_CHECK(count_different_pixels(decaled.base, plain.base, 2) > 300);
}

NF_TEST(decal_preserves_the_normal_and_emissive_attachments) {
    // A decal changes the surface appearance, not the geometry or the emitted
    // radiance. Those live in GBuffer1 and GBuffer3, which the decal pass does
    // not attach; this is the guard against someone "simplifying" the pass to
    // write all four attachments.
    const GpuFixture& f = require_gpu();
    DecalScene bare;
    bare.decal_present = false;
    bare.wall_emission = 0.75f;
    DecalScene with;
    with.wall_emission = 0.75f; // same wall in both frames

    DecalFrame plain, decaled;
    NF_CHECK(render_decal_frame(*f.device, bare, plain));
    NF_CHECK(render_decal_frame(*f.device, with, decaled));
    if (!decaled.supported) NF_SKIP("decal shaders are not available");

    // Normal and emissive are bit-identical across the whole frame.
    NF_CHECK_EQ(count_different_pixels(decaled.normal, plain.normal, 0), 0u);
    NF_CHECK_EQ(count_different_pixels(decaled.emissive, plain.emissive, 0), 0u);
    // ...while the base colour clearly changed.
    NF_CHECK(count_different_pixels(decaled.base, plain.base, 2) > 300);
}

NF_TEST(decal_partial_coverage_keeps_the_emission_strength_in_gbuffer2_alpha) {
    // GBuffer2.a carries the surface's emission strength and the lighting pass
    // reads it. With `opacity=0.5` the coverage is 0.5, so this fails loudly
    // (alpha 0.5 vs 0.75) if the surface attachment ever goes back to
    // One/Zero alpha blending instead of the Zero/One factors the decal pass
    // sets. It is the regression test for the per-attachment blend factors.
    const GpuFixture& f = require_gpu();
    DecalScene bare;
    bare.decal_present = false;
    bare.wall_emission = 0.75f;
    DecalScene half;
    half.wall_emission = 0.75f;
    half.decal_opacity = 0.5f;

    DecalFrame plain, decaled;
    NF_CHECK(render_decal_frame(*f.device, bare, plain));
    NF_CHECK(render_decal_frame(*f.device, half, decaled));
    if (!decaled.supported) NF_SKIP("decal shaders are not available");

    const usize center = usize(kSize / 2) * kSize + kSize / 2;
    // The wall wrote 0.75 into alpha; blending must leave it alone.
    NF_CHECK(std::abs(int(decaled.surface[center].a) - 191) <= 2);
    NF_CHECK_EQ(int(decaled.surface[center].a), int(plain.surface[center].a));

    // RGB was blended at half coverage: between the wall and the decal.
    NF_CHECK(decaled.base[center].r > plain.base[center].r + 20);
    NF_CHECK(decaled.base[center].r < 240);
    NF_CHECK(decaled.base[center].g > 40);   // not the full-coverage decal's 26
    NF_CHECK(decaled.base[center].g < plain.base[center].g - 20);
}

NF_TEST(decal_reaches_the_tonemapped_image) {
    // The Decals graph pass must run BEFORE Lighting. If it were appended after
    // Tonemap (or skipped), the gbuffer test above would still pass while the
    // screen showed nothing. Comparing the final image pins the ordering.
    const GpuFixture& f = require_gpu();
    DecalScene bare;
    bare.decal_present = false;
    DecalScene with;

    DecalFrame plain, decaled;
    NF_CHECK(render_decal_frame(*f.device, bare, plain));
    NF_CHECK(render_decal_frame(*f.device, with, decaled));
    if (!decaled.supported) NF_SKIP("decal shaders are not available");

    NF_CHECK_EQ(rhi::validation_error_count(), 0u);
    const usize center = usize(kSize / 2) * kSize + kSize / 2;
    NF_CHECK(count_different_pixels(decaled.image, plain.image, 3) > 100);
    // Direction: the center is red-dominant now, gray before.
    NF_CHECK(int(decaled.image[center].r) > int(decaled.image[center].g) + 20);
    NF_CHECK(std::abs(int(plain.image[center].r) - int(plain.image[center].g)) < 40);
}

// ---------------------------------------------------------------------------
// GPU: no-op cases must be bit-identical
// ---------------------------------------------------------------------------

NF_TEST(disabled_decal_is_not_submitted_and_leaves_the_frame_untouched) {
    const GpuFixture& f = require_gpu();
    DecalScene bare;
    bare.decal_present = false;
    DecalScene off;
    off.decal_enabled = false;

    DecalFrame plain, disabled;
    NF_CHECK(render_decal_frame(*f.device, bare, plain));
    NF_CHECK(render_decal_frame(*f.device, off, disabled));
    if (!disabled.supported) NF_SKIP("decal shaders are not available");

    NF_CHECK_EQ(disabled.stats.decals_submitted, 0u);
    NF_CHECK_EQ(disabled.stats.decals_recorded, 0u);
    expect_frames_identical(plain, disabled);
}

NF_TEST(offscreen_decal_is_culled_before_the_draw_and_leaves_the_frame_untouched) {
    // Submitted (the scene has it) but not recorded (the CPU screen rect culled
    // it). The frame must equal a frame with no decal at all, tolerance 0.
    const GpuFixture& f = require_gpu();
    DecalScene bare;
    bare.decal_present = false;
    DecalScene offscreen;
    offscreen.decal_position = Vec3{100.0f, 0.0f, 1.4f};

    DecalFrame plain, culled;
    NF_CHECK(render_decal_frame(*f.device, bare, plain));
    NF_CHECK(render_decal_frame(*f.device, offscreen, culled));
    if (!culled.supported) NF_SKIP("decal shaders are not available");

    NF_CHECK_EQ(culled.stats.decals_submitted, 1u);
    NF_CHECK_EQ(culled.stats.decals_recorded, 0u);
    expect_frames_identical(plain, culled);
}

NF_TEST(backfacing_decal_records_a_draw_but_writes_no_pixels) {
    // The box still covers the wall, so the CPU rect says "visible" and the
    // draw is recorded. The fragment shader rejects every fragment because the
    // surface normal disagrees with the decal's outward axis. Pixel equality
    // proves the rejection; the recorded count proves the test exercised it.
    const GpuFixture& f = require_gpu();
    DecalScene bare;
    bare.decal_present = false;
    DecalScene backwards;
    backwards.decal_yaw_deg = 180.0f;

    DecalFrame plain, rejected;
    NF_CHECK(render_decal_frame(*f.device, bare, plain));
    NF_CHECK(render_decal_frame(*f.device, backwards, rejected));
    if (!rejected.supported) NF_SKIP("decal shaders are not available");

    NF_CHECK_EQ(rejected.stats.decals_submitted, 1u);
    NF_CHECK_EQ(rejected.stats.decals_recorded, 1u);
    expect_frames_identical(plain, rejected);
}

NF_TEST(decal_with_a_zero_alpha_material_writes_nothing) {
    // Coverage = opacity * baseColor.a. A decal authored with alpha 0 must be
    // a no-op rather than writing zero-coloured fragments over the wall.
    const GpuFixture& f = require_gpu();
    DecalScene bare;
    bare.decal_present = false;
    DecalScene transparent;
    transparent.decal_alpha = 0.0f;

    DecalFrame plain, invisible;
    NF_CHECK(render_decal_frame(*f.device, bare, plain));
    NF_CHECK(render_decal_frame(*f.device, transparent, invisible));
    if (!invisible.supported) NF_SKIP("decal shaders are not available");

    NF_CHECK_EQ(invisible.stats.decals_recorded, 1u);
    expect_frames_identical(plain, invisible);
}
