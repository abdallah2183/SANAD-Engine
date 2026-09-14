// Tests/RHITests/test_benchmarks_3d.cpp — CPU-side pipeline benchmarks
//
// Measures the three stages the Basic 3D Renderer milestone owns on the CPU:
//   Extraction    (ecs::World → RenderWorld)
//   Frustum Culling (RenderWorld → visible indices)
//   Draw Preparation (visible → per-draw data, mirroring Renderer3D::render)
//
// 100 / 1K / 10K / 100K render objects. No GPU-driven rendering here — the
// point is to pin down the CPU baseline and make sure the architecture does
// not prevent GPU-driven culling later. Assertions check correctness only;
// the timings are logged for the milestone report (perf thresholds would be
// flaky across machines).

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/Rendering/Culling.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>
#include <vector>

namespace {

using namespace nf;
using namespace nf::test;
using namespace nf::rendering;
using namespace nf::ecs;

struct BenchResult {
    f64 extract_us = 0;
    f64 cull_us = 0;
    f64 draw_prep_us = 0;
    u32 visible = 0;
};

BenchResult bench_once(u32 count) {
    MeshLibrary meshes;
    auto cube = StaticMesh::create_cube(1.0f);
    StaticMeshHandle h = meshes.add(std::move(cube));

    World world;
    world.add<scene::Transform>(world.create_entity(), scene::Transform{}); // camera placeholder entity
    for (u32 i = 0; i < count; ++i) {
        Entity e = world.create_entity();
        auto& t = world.add<scene::Transform>(e, scene::Transform{});
        // Spread objects on a grid; ~1 in 3 lands inside the frustum.
        const float ring = 40.0f;
        t.local_x = std::fmod(float(i) * 7.3f, ring * 2.0f) - ring;
        t.local_y = std::fmod(float(i) * 3.1f, ring * 2.0f) - ring;
        t.local_z = std::fmod(float(i) * 5.7f, 30.0f) - 15.0f;
        world.add<MeshComponent>(e, MeshComponent{h, kInvalidMaterialHandle, (i % 7) != 0});
    }
    scene::propagate_transforms(world);

    BenchResult r;

    // --- Extraction ---
    RenderWorld rw;
    nf::Clock c;
    nf::runtime::extract_render_objects(world, meshes, rw);
    r.extract_us = c.elapsed_us();

    // --- Frustum culling ---
    Camera cam{};
    cam.position = {0, 0, 25};
    cam.target = {0, 0, 0};
    cam.fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
    cam.aspect = 1.0f;
    cam.near_plane = 0.1f;
    cam.far_plane = 100.0f;
    update_camera(cam);
    std::vector<u32> visible;
    c.reset();
    cull_render_world(rw, cam, visible);
    r.cull_us = c.elapsed_us();
    r.visible = static_cast<u32>(visible.size());

    // --- Draw preparation (mirrors Renderer3D's per-draw CPU work) ---
    struct Push { float view_proj[16]; float model[16]; };
    Push push{};
    std::memcpy(push.view_proj, cam.view_projection.m, sizeof(push.view_proj));
    c.reset();
    u32 submesh_draws = 0;
    for (u32 idx : visible) {
        const RenderObject& ro = rw.objects[idx];
        std::memcpy(push.model, ro.world.m, sizeof(push.model));     // per-draw constants
        (void)ro.mesh_handle;                                        // handle → mesh resolve
        (void)ro.material_handle;                                    // handle → material resolve
        submesh_draws += 1;                                          // one submesh per cube
    }
    r.draw_prep_us = c.elapsed_us();
    (void)submesh_draws;
    return r;
}

} // namespace

NF_TEST(benchmark_100_render_objects) {
    BenchResult r = bench_once(100);
    NF_LOG_INFO(LogCategory::Core, "[bench] 100 objects: extract={:.1f}us cull={:.1f}us draw_prep={:.1f}us visible={}",
                r.extract_us, r.cull_us, r.draw_prep_us, r.visible);
    NF_CHECK(r.visible > 0);
}

NF_TEST(benchmark_1k_render_objects) {
    BenchResult r = bench_once(1'000);
    NF_LOG_INFO(LogCategory::Core, "[bench] 1K objects: extract={:.1f}us cull={:.1f}us draw_prep={:.1f}us visible={}",
                r.extract_us, r.cull_us, r.draw_prep_us, r.visible);
    NF_CHECK(r.visible > 0);
}

NF_TEST(benchmark_10k_render_objects) {
    BenchResult r = bench_once(10'000);
    NF_LOG_INFO(LogCategory::Core, "[bench] 10K objects: extract={:.1f}us cull={:.1f}us draw_prep={:.1f}us visible={}",
                r.extract_us, r.cull_us, r.draw_prep_us, r.visible);
    NF_CHECK(r.visible > 0);
}

NF_TEST(benchmark_100k_render_objects) {
    BenchResult r = bench_once(100'000);
    NF_LOG_INFO(LogCategory::Core, "[bench] 100K objects: extract={:.1f}us cull={:.1f}us draw_prep={:.1f}us visible={}",
                r.extract_us, r.cull_us, r.draw_prep_us, r.visible);
    NF_CHECK(r.visible > 0);
}
