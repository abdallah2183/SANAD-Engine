// RHITests — procedural terrain: determinism, shape, validity (Phase 12+).
//
// Pure CPU (no device): pins the height function and the mesh builder.

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/LodGenerator.hpp>
#include <NF/Rendering/Terrain.hpp>

#include <cmath>

using namespace nf;
using namespace nf::rendering;

NF_TEST(terrain_height_is_deterministic) {
    TerrainOptions opt;
    opt.seed = 7;
    NF_CHECK_NEAR(terrain_height(1.5f, -2.25f, opt), terrain_height(1.5f, -2.25f, opt), 0.0f);
    TerrainOptions other = opt;
    other.seed = 8;
    // Different seeds (almost surely) differ somewhere.
    bool differ = false;
    for (int i = 0; i < 16; ++i) {
        if (terrain_height(i * 1.7f, i * 0.9f, opt) != terrain_height(i * 1.7f, i * 0.9f, other)) {
            differ = true;
            break;
        }
    }
    NF_CHECK(differ);
}

NF_TEST(terrain_height_scales_and_flattens) {
    TerrainOptions opt;
    opt.height_scale = 10.0f;
    const float h = terrain_height(3.0f, 4.0f, opt);
    NF_CHECK(h >= 0.0f && h <= 10.0f); // normalized field x scale
    TerrainOptions flat = opt;
    flat.octaves = 0;
    NF_CHECK_NEAR(terrain_height(3.0f, 4.0f, flat), 0.0f, 1e-6f);
}

NF_TEST(terrain_mesh_grid_shape) {
    TerrainOptions opt;
    opt.resolution = 9;
    opt.size = 18.0f;
    opt.height_scale = 5.0f;
    auto mesh = build_terrain_mesh(opt);
    NF_CHECK(mesh != nullptr);
    const MeshLOD& lod = mesh->lods()[0];
    NF_CHECK(lod.vertices.size() == 81);
    NF_CHECK(lod.indices.size() == 8 * 8 * 6);
    NF_CHECK(lod.submeshes.size() == 1);
    NF_CHECK(lod.submeshes[0].index_count == 8 * 8 * 6);
    // Center vertex sits at the origin in XZ.
    const Vertex& center = lod.vertices[4 * 9 + 4];
    NF_CHECK_NEAR(center.position[0], 0.0f, 1e-5f);
    NF_CHECK_NEAR(center.position[2], 0.0f, 1e-5f);
    NF_CHECK_NEAR(center.position[1], terrain_height(0.0f, 0.0f, opt), 1e-5f);
    // Corner reaches the extent.
    NF_CHECK_NEAR(lod.vertices[0].position[0], -9.0f, 1e-5f);
    NF_CHECK_NEAR(lod.vertices[0].position[2], -9.0f, 1e-5f);
    // UVs span the unit square.
    NF_CHECK_NEAR(lod.vertices.back().uv0[0], 1.0f, 1e-6f);
    NF_CHECK_NEAR(lod.vertices.back().uv0[1], 1.0f, 1e-6f);
    // Normals are unit length.
    for (const auto& v : lod.vertices) {
        const float n = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                                  v.normal[2] * v.normal[2]);
        NF_CHECK_NEAR(n, 1.0f, 1e-4f);
        NF_CHECK(v.normal[1] > 0.0f); // up-facing hemisphere
    }
    // Indices resolve inside the vertex list.
    for (u32 idx : lod.indices) NF_CHECK(idx < lod.vertices.size());
    // Bounds contain the mesh.
    NF_CHECK(lod.bounds.min_x <= -9.0f && lod.bounds.max_x >= 9.0f);
    NF_CHECK(lod.sphere.radius > 0.0f);
}

NF_TEST(terrain_mesh_rejects_degenerate_options) {
    TerrainOptions bad;
    bad.resolution = 1;
    NF_CHECK(build_terrain_mesh(bad) == nullptr);
    bad.resolution = 8;
    bad.size = 0.0f;
    NF_CHECK(build_terrain_mesh(bad) == nullptr);
}

NF_TEST(terrain_mesh_takes_lods) {
    TerrainOptions opt;
    opt.resolution = 33;
    opt.size = 64.0f;
    auto mesh = build_terrain_mesh(opt);
    NF_CHECK(mesh != nullptr);
    LodGenerateOptions lopt;
    lopt.max_levels = 2;
    const LodGenerateStats stats = build_lods(*mesh, lopt);
    NF_CHECK(stats.levels_built >= 1);
    NF_CHECK(mesh->lods().size() == 1 + stats.levels_built);
}
