// RHITests — procedural terrain: determinism, shape, validity (Phase 12+).
//
// Pure CPU (no device): pins the height function and the mesh builder.

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/LodGenerator.hpp>
#include <NF/Rendering/Terrain.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// Splat maps (design doc 59): layer selection and the uv1 field it writes.
// ---------------------------------------------------------------------------

namespace {

/// A three-band mountain: sand at the foot, grass on the slopes, rock above the
/// snowline. Height bands are disjoint so each altitude has exactly one owner,
/// which is what makes the per-band assertions below unambiguous.
std::vector<TerrainLayer> mountain_layers() {
    std::vector<TerrainLayer> layers(3u);
    layers[0u].texture_slot = 0u;
    layers[0u].min_height = 0.0f;
    layers[0u].max_height = 4.0f;
    layers[1u].texture_slot = 1u;
    layers[1u].min_height = 4.0f;
    layers[1u].max_height = 8.0f;
    layers[2u].texture_slot = 2u;
    layers[2u].min_height = 8.0f;
    layers[2u].max_height = 12.0f;
    return layers;
}

} // namespace

NF_TEST(terrain_splat_picks_the_band_the_height_is_in) {
    const std::vector<TerrainLayer> layers = mountain_layers();
    NF_CHECK_EQ(terrain_splat(1.0f, 0.0f, layers).slot, 0u);
    NF_CHECK_EQ(terrain_splat(6.0f, 0.0f, layers).slot, 1u);
    NF_CHECK_EQ(terrain_splat(11.0f, 0.0f, layers).slot, 2u);
    // Boundaries belong to both, and the tie breaks to the lower slot — so the
    // band order, not the construction order, decides.
    NF_CHECK_EQ(terrain_splat(4.0f, 0.0f, layers).slot, 0u);
    NF_CHECK_EQ(terrain_splat(8.0f, 0.0f, layers).slot, 1u);
    // No blend: the neighbouring band does not cover this exact point, so the
    // vertex is 100% its owner.
    NF_CHECK_NEAR(terrain_splat(1.0f, 0.0f, layers).blend, 0.0f, 1e-6f);
}

NF_TEST(terrain_splat_blends_between_overlapping_bands) {
    const std::vector<TerrainLayer> layers = mountain_layers();
    // Re-open the grass band so it overlaps the sand band: a vertex at 3.0 is
    // in both, and the splat is a mix rather than a hard edge.
    std::vector<TerrainLayer> overlap = layers;
    overlap[1u].min_height = 2.0f;
    const TerrainSplat s = terrain_splat(3.0f, 0.0f, overlap);
    NF_CHECK_EQ(s.slot, 0u); // equal weights, tie breaks down
    // Equal weights => half and half.
    NF_CHECK_NEAR(s.blend, 0.5f, 1e-6f);
    // The weight ratio is honoured: a sand band four times weaker than grass
    // takes a fifth of the surface at an overlap.
    overlap[0u].weight = 0.25f;
    NF_CHECK_NEAR(terrain_splat(3.0f, 0.0f, overlap).blend, 0.8f, 1e-6f);
}

NF_TEST(terrain_splat_slope_gate_separates_flat_from_steep) {
    std::vector<TerrainLayer> layers(2u);
    layers[0u].texture_slot = 0u;
    layers[0u].min_height = 0.0f;
    layers[0u].max_height = 10.0f;
    layers[0u].max_slope = 0.5f; // flat ground only
    layers[1u].texture_slot = 1u;
    layers[1u].min_height = 0.0f;
    layers[1u].max_height = 10.0f;
    layers[1u].min_slope = 0.5f; // cliffs only
    // Same height, different steepness: slope, not height, picks the band.
    NF_CHECK_EQ(terrain_splat(5.0f, 0.0f, layers).slot, 0u);
    NF_CHECK_EQ(terrain_splat(5.0f, 1.0f, layers).slot, 1u);
}

NF_TEST(terrain_splat_is_pure_and_ignores_list_order) {
    const std::vector<TerrainLayer> layers = mountain_layers();
    std::vector<TerrainLayer> reversed(layers.rbegin(), layers.rend());
    // Tie-breaking to the lower slot means reversing the input cannot move a
    // boundary — a layer list is a set of bands, not a priority queue.
    for (float h = 0.0f; h <= 12.0f; h += 0.5f) {
        NF_CHECK_EQ(terrain_splat(h, 0.0f, layers).slot,
                    terrain_splat(h, 0.0f, reversed).slot);
    }
    // An empty list is the base texture, not an error.
    const TerrainSplat empty = terrain_splat(5.0f, 0.0f, std::vector<TerrainLayer>{});
    NF_CHECK_EQ(empty.slot, 0u);
    NF_CHECK_NEAR(empty.blend, 0.0f, 1e-6f);
    // A point no band covers falls through to the base too.
    NF_CHECK_EQ(terrain_splat(50.0f, 0.0f, layers).slot, 0u);
}

NF_TEST(terrain_splat_skipped_slot_does_not_blend_across_the_gap) {
    std::vector<TerrainLayer> layers(2u);
    layers[0u].texture_slot = 0u;
    layers[0u].min_height = 0.0f;
    layers[0u].max_height = 5.0f;
    layers[1u].texture_slot = 5u; // slot 1..4 are unused
    layers[1u].min_height = 0.0f;
    layers[1u].max_height = 5.0f;
    // The blend looks for slot+1, which nothing here owns: a gap in the
    // numbering degrades to a hard edge instead of sampling a missing texture.
    const TerrainSplat s = terrain_splat(2.0f, 0.0f, layers);
    NF_CHECK_EQ(s.slot, 0u);
    NF_CHECK_NEAR(s.blend, 0.0f, 1e-6f);
}

NF_TEST(terrain_mesh_with_layers_writes_the_splat_into_uv1) {
    TerrainOptions opt;
    opt.resolution = 17;
    opt.size = 40.0f;
    opt.height_scale = 12.0f; // reaches all three bands of the mountain
    const std::vector<TerrainLayer> layers = mountain_layers();
    auto mesh = build_terrain_mesh(opt, layers);
    NF_CHECK(mesh != nullptr);
    const MeshLOD& lod = mesh->lods()[0];
    NF_CHECK_EQ(lod.vertices.size(), 17u * 17u);
    NF_CHECK_EQ(lod.indices.size(), 16u * 16u * 6u);

    bool any_splat = false;
    std::vector<u32> seen;
    for (const Vertex& v : lod.vertices) {
        // uv1 must be an integer slot plus a blend in [0, 1], exactly what the
        // shader decodes. A NaN or an out-of-range slot reads a texture the
        // array does not have.
        const float slot_f = v.uv1[0];
        NF_CHECK(slot_f >= 0.0f && slot_f <= 2.0f);
        NF_CHECK_NEAR(slot_f - std::floor(slot_f), 0.0f, 1e-4f);
        NF_CHECK(v.uv1[1] >= 0.0f && v.uv1[1] <= 1.0f);
        // The slot the vertex reports is the band its own height is in: the
        // builder and the selection function must agree per vertex.
        const TerrainSplat expect = terrain_splat(v.position[1], 0.0f, layers);
        NF_CHECK_EQ(static_cast<u32>(slot_f), expect.slot);
        any_splat = any_splat || slot_f > 0.0f;
        seen.push_back(static_cast<u32>(slot_f));
    }
    NF_CHECK(any_splat);
    // All three bands actually appear on this terrain — a band the noise never
    // reaches is a layer the artist set up that nothing renders.
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    NF_CHECK_EQ(seen.size(), 3u);
    NF_CHECK_EQ(seen[0u], 0u);
    NF_CHECK_EQ(seen[1u], 1u);
    NF_CHECK_EQ(seen[2u], 2u);
    // One material slot per layer, so the renderer can bind a texture array.
    NF_CHECK_EQ(lod.material_slots.size(), 3u);
    // The base geometry is untouched: uv0, normals and winding are the plain
    // terrain's, so the layered mesh drops into the same render path.
    NF_CHECK_NEAR(lod.vertices[0u].uv0[0], 0.0f, 1e-6f);
    NF_CHECK_NEAR(lod.vertices.back().uv0[0], 1.0f, 1e-6f);
    for (const Vertex& v : lod.vertices) {
        const float n = std::sqrt(v.normal[0u] * v.normal[0u] + v.normal[1u] * v.normal[1u] +
                                  v.normal[2u] * v.normal[2u]);
        NF_CHECK_NEAR(n, 1.0f, 1e-4f);
    }
    for (u32 idx : lod.indices) NF_CHECK(idx < lod.vertices.size());
}

NF_TEST(terrain_mesh_layers_survive_lod_generation) {
    // The LOD generator averages uv1 (LodGenerator.cpp:97) the same way it
    // averages uv0, so a simplified terrain keeps a valid splat: a blended
    // vertex must not become a slot the original never drew.
    TerrainOptions opt;
    opt.resolution = 33;
    opt.size = 60.0f;
    opt.height_scale = 12.0f;
    auto mesh = build_terrain_mesh(opt, mountain_layers());
    NF_CHECK(mesh != nullptr);
    LodGenerateOptions lopt;
    lopt.max_levels = 2;
    const LodGenerateStats stats = build_lods(*mesh, lopt);
    NF_CHECK(stats.levels_built >= 1);
    for (const MeshLOD& lod : mesh->lods()) {
        for (const Vertex& v : lod.vertices) {
            NF_CHECK(v.uv1[0] >= 0.0f && v.uv1[0] <= 2.0f);
            NF_CHECK(v.uv1[1] >= 0.0f && v.uv1[1] <= 1.0f);
        }
        NF_CHECK_EQ(lod.material_slots.size(), 3u);
    }
}

NF_TEST(terrain_mesh_without_layers_is_the_base_mesh) {
    // A layered builder given no layers must produce the plain terrain, or
    // every existing caller's uv1 changes silently.
    TerrainOptions opt;
    opt.resolution = 9;
    opt.size = 18.0f;
    opt.height_scale = 5.0f;
    auto plain = build_terrain_mesh(opt);
    auto layered = build_terrain_mesh(opt, std::vector<TerrainLayer>{});
    NF_CHECK(plain != nullptr);
    NF_CHECK(layered != nullptr);
    NF_CHECK_EQ(plain->lods()[0].vertices.size(), layered->lods()[0].vertices.size());
    NF_CHECK_EQ(plain->lods()[0].material_slots.size(), 1u);
    NF_CHECK_EQ(layered->lods()[0].material_slots.size(), 1u);
    for (size_t i = 0u; i < plain->lods()[0].vertices.size(); ++i) {
        NF_CHECK_NEAR(layered->lods()[0].vertices[i].uv1[0], 0.0f, 1e-6f);
        NF_CHECK_NEAR(layered->lods()[0].vertices[i].uv1[1], 0.0f, 1e-6f);
    }
}

NF_TEST(terrain_mesh_with_layers_rejects_degenerate_options) {
    TerrainOptions bad;
    bad.resolution = 1;
    NF_CHECK(build_terrain_mesh(bad, mountain_layers()) == nullptr);
    bad.resolution = 8;
    bad.size = 0.0f;
    NF_CHECK(build_terrain_mesh(bad, mountain_layers()) == nullptr);
}

// ---------------------------------------------------------------------------
// Heightmaps (design doc 59): an imported raster in place of the noise field.
// Pure CPU — the field is sampled in terrain uv, so one low-resolution image
// drives a high-resolution mesh, and the same field answers terrain_height
// outside the builder.
// ---------------------------------------------------------------------------

namespace {

/// 2x2 field where the height is `u`: a linear ramp along +X. Bilinear
/// interpolation between two texel columns is exact, so the whole field is a
/// plane — which makes the gradient assertions below unambiguous.
HeightField ramp_field_x() {
    HeightField f;
    f.width = 2u;
    f.height = 2u;
    f.heights = {0.0f, 1.0f, 0.0f, 1.0f}; // [j * width + i]: h = i
    return f;
}

/// Writes one opaque pixel into an RGBA8 raster the test built by hand.
void put_px(DecodedImage& img, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + x) * 4u;
    img.rgba[o + 0u] = r;
    img.rgba[o + 1u] = g;
    img.rgba[o + 2u] = b;
    img.rgba[o + 3u] = 255u;
}

} // namespace

NF_TEST(heightfield_sample_interpolates_bilinearly) {
    const HeightField f = ramp_field_x();
    NF_CHECK(f.ok());
    // The ramp is u, independent of v.
    NF_CHECK_NEAR(f.sample(0.0f, 0.0f), 0.0f, 1e-6f);
    NF_CHECK_NEAR(f.sample(1.0f, 1.0f), 1.0f, 1e-6f);
    NF_CHECK_NEAR(f.sample(0.25f, 0.75f), 0.25f, 1e-6f);
    NF_CHECK_NEAR(f.sample(0.5f, 0.0f), 0.5f, 1e-6f);
    // A corner-weighted quad: the centre takes a quarter of the one bright
    // texel, and each edge takes half — the weights must sum the way a surface
    // interpolates, not the way a nearest-neighbour pick would.
    HeightField quad;
    quad.width = 2u;
    quad.height = 2u;
    quad.heights = {0.0f, 0.0f, 0.0f, 1.0f}; // only (i=1, j=1) is bright
    NF_CHECK_NEAR(quad.sample(0.5f, 0.5f), 0.25f, 1e-6f);
    NF_CHECK_NEAR(quad.sample(0.5f, 1.0f), 0.5f, 1e-6f);
    NF_CHECK_NEAR(quad.sample(1.0f, 0.5f), 0.5f, 1e-6f);
}

NF_TEST(heightfield_sample_clamps_outside_the_unit_square) {
    const HeightField f = ramp_field_x();
    // Off the -X edge is the border texel, not the far side of the raster
    // wrapped back across: a field smaller than the terrain extends its edge
    // instead of tiling a seam.
    NF_CHECK_NEAR(f.sample(-1.0f, 0.5f), 0.0f, 1e-6f);
    NF_CHECK_NEAR(f.sample(2.0f, 0.5f), 1.0f, 1e-6f);
    NF_CHECK_NEAR(f.sample(-10.0f, -10.0f), 0.0f, 1e-6f);
    NF_CHECK_NEAR(f.sample(10.0f, 10.0f), 1.0f, 1e-6f);
    // A single texel: the +1 neighbour folds onto the texel itself, so every
    // query is the one stored value and no index runs past the end.
    HeightField one;
    one.width = 1u;
    one.height = 1u;
    one.heights = {0.37f};
    NF_CHECK_NEAR(one.sample(0.0f, 0.0f), 0.37f, 1e-6f);
    NF_CHECK_NEAR(one.sample(0.5f, 0.5f), 0.37f, 1e-6f);
    NF_CHECK_NEAR(one.sample(1.0f, 1.0f), 0.37f, 1e-6f);
    NF_CHECK_NEAR(one.sample(-7.0f, 91.0f), 0.37f, 1e-6f);
    // An empty field is flat ground, never an out-of-range read.
    const HeightField empty;
    NF_CHECK(!empty.ok());
    NF_CHECK_NEAR(empty.sample(0.5f, 0.5f), 0.0f, 1e-6f);
}

NF_TEST(heightfield_from_image_reads_luma) {
    DecodedImage img;
    img.width = 2;
    img.height = 2;
    img.rgba.resize(16u);
    put_px(img, 0, 0, 0, 0, 0);       // black -> 0
    put_px(img, 1, 0, 255, 255, 255); // white -> 1
    put_px(img, 0, 1, 100, 100, 100); // mid grey
    put_px(img, 1, 1, 140, 80, 90);   // RGB whose luma is that same grey
    const HeightField f = build_heightfield_from_image(img);
    NF_CHECK(f.ok());
    NF_CHECK_EQ(f.width, 2u);
    NF_CHECK_EQ(f.height, 2u);
    NF_CHECK_NEAR(f.heights[0u], 0.0f, 1e-6f);
    NF_CHECK_NEAR(f.heights[1u], 1.0f, 1e-6f);
    NF_CHECK_NEAR(f.heights[2u], 100.0f / 255.0f, 1e-4f);
    // An authored colour heightmap and its greyscale conversion are the same
    // terrain: the artist's file format does not change the heights.
    NF_CHECK_NEAR(f.heights[2u], f.heights[3u], 1.5f / 255.0f);
    for (const float h : f.heights) NF_CHECK(h >= 0.0f && h <= 1.0f);
}

NF_TEST(heightfield_from_image_rejects_a_broken_raster) {
    NF_CHECK(!build_heightfield_from_image(DecodedImage{}).ok());
    // A raster that does not match its declared size is not silently truncated
    // to fit: the caller sees the field as unusable and falls back to noise
    // rather than building a terrain from half an image.
    DecodedImage mismatched;
    mismatched.width = 4;
    mismatched.height = 4;
    mismatched.rgba.resize(8u); // two pixels' worth, not sixteen
    NF_CHECK(!build_heightfield_from_image(mismatched).ok());
}

NF_TEST(heightfield_terrain_uv_maps_the_world_onto_the_raster) {
    HeightField f;
    f.width = 2u;
    f.height = 2u;
    f.heights = {0.0f, 0.5f, 0.5f, 1.0f}; // h = (u + v) / 2
    TerrainOptions opt;
    opt.size = 40.0f;
    opt.height_scale = 12.0f;
    opt.heightmap = &f;
    // The terrain's corners land on the raster's corners.
    NF_CHECK_NEAR(terrain_height(-20.0f, -20.0f, opt), 0.0f, 1e-5f);
    NF_CHECK_NEAR(terrain_height(20.0f, 20.0f, opt), 12.0f, 1e-5f);
    NF_CHECK_NEAR(terrain_height(20.0f, -20.0f, opt), 6.0f, 1e-5f);
    NF_CHECK_NEAR(terrain_height(-20.0f, 20.0f, opt), 6.0f, 1e-5f);
    // The centre is the middle of the field, in the world units the scale says.
    NF_CHECK_NEAR(terrain_height(0.0f, 0.0f, opt), 6.0f, 1e-5f);
    // Past the +X edge the border texel extends: u clamps to 1 (h = 0.5 -> 6.0)
    // where a tiling sample would have wrapped u back to 0 (h = 0 -> 0.0).
    NF_CHECK_NEAR(terrain_height(100.0f, -20.0f, opt), 6.0f, 1e-5f);
    NF_CHECK_NEAR(terrain_height(-100.0f, -20.0f, opt), 0.0f, 1e-5f);
}

NF_TEST(heightfield_terrain_height_ignores_the_noise_settings) {
    const HeightField f = ramp_field_x();
    TerrainOptions opt;
    opt.size = 40.0f;
    opt.height_scale = 8.0f;
    opt.heightmap = &f;
    // octaves 0 flattens the NOISE path and a changed seed moves it; with a
    // field attached the noise is not consulted at all, so an imported terrain
    // is exactly what was painted.
    opt.octaves = 0;
    opt.seed = 999;
    opt.noise_frequency = 0.5f;
    NF_CHECK_NEAR(terrain_height(20.0f, 0.0f, opt), 8.0f, 1e-5f);
    NF_CHECK_NEAR(terrain_height(-20.0f, 0.0f, opt), 0.0f, 1e-5f);
    NF_CHECK_NEAR(terrain_height(0.0f, 0.0f, opt), 4.0f, 1e-5f);
}

NF_TEST(terrain_mesh_from_a_heightfield_follows_the_field_per_vertex) {
    HeightField f;
    f.width = 4u;
    f.height = 4u;
    f.heights.resize(16u);
    for (u32 j = 0u; j < 4u; ++j) {
        for (u32 i = 0u; i < 4u; ++i) {
            f.heights[j * 4u + i] = static_cast<float>(i + j) / 6.0f; // spans 0..1
        }
    }
    TerrainOptions opt;
    opt.resolution = 33;
    opt.size = 40.0f;
    opt.height_scale = 6.0f;
    opt.heightmap = &f;
    auto mesh = build_terrain_mesh(opt);
    NF_CHECK(mesh != nullptr);
    const MeshLOD& lod = mesh->lods()[0];
    NF_CHECK_EQ(lod.vertices.size(), 33u * 33u);
    NF_CHECK_EQ(lod.indices.size(), 32u * 32u * 6u);
    // Every vertex stands on the height function the camera and the gameplay
    // code sample later: the builder and the query are one source of truth, or
    // a cube planted by terrain_height would hover above or sink through the
    // mesh the artist is looking at.
    for (const Vertex& v : lod.vertices) {
        NF_CHECK_NEAR(v.position[1], terrain_height(v.position[0], v.position[2], opt), 1e-4f);
        NF_CHECK(v.position[1] >= 0.0f && v.position[1] <= 6.0f);
    }
    // The field actually raised the mesh rather than the builder ignoring the
    // pointer: the bounds leave the flat plane.
    NF_CHECK(lod.bounds.max_y > 1.0f);
    NF_CHECK(lod.bounds.min_y >= -1e-4f);
}

NF_TEST(terrain_mesh_from_a_heightfield_normals_are_the_field_gradient) {
    const HeightField f = ramp_field_x();
    TerrainOptions opt;
    opt.resolution = 17;
    opt.size = 32.0f;
    opt.height_scale = 4.0f;
    opt.heightmap = &f;
    auto mesh = build_terrain_mesh(opt);
    NF_CHECK(mesh != nullptr);
    // h = u = (x + 16) / 32 scaled by 4, so the surface is a plane rising
    // 4 units over 32: dh/dx = 0.125 everywhere, and every INTERIOR vertex
    // carries the same tilted normal. The two boundary columns are excluded on
    // purpose — their central difference reaches one sample past the terrain,
    // and the documented border clamp flattens that half of the gradient. A
    // field that sampled wrongly would kink in the interior, not at the edge.
    const MeshLOD& lod = mesh->lods()[0];
    const float slope = 4.0f / 32.0f;
    const float expect_x = -slope / std::sqrt(1.0f + slope * slope);
    for (const Vertex& v : lod.vertices) {
        const float n = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                                  v.normal[2] * v.normal[2]);
        NF_CHECK_NEAR(n, 1.0f, 1e-4f);
        NF_CHECK(v.normal[1] > 0.0f); // up-facing hemisphere
        if (v.position[0] <= -16.0f || v.position[0] >= 16.0f) continue;
        NF_CHECK_NEAR(v.normal[0], expect_x, 1e-3f);
        NF_CHECK_NEAR(v.normal[2], 0.0f, 1e-3f);
    }
}

NF_TEST(terrain_mesh_from_a_heightfield_drives_the_splat_layers) {
    HeightField f;
    f.width = 3u;
    f.height = 3u;
    f.heights = {0.05f, 0.35f, 0.75f, 0.35f, 0.75f, 0.95f, 0.75f, 0.95f, 0.75f};
    TerrainOptions opt;
    opt.resolution = 33;
    opt.size = 60.0f;
    opt.height_scale = 12.0f; // reaches all three bands of the mountain
    opt.heightmap = &f;
    const std::vector<TerrainLayer> layers = mountain_layers();
    auto mesh = build_terrain_mesh(opt, layers);
    NF_CHECK(mesh != nullptr);
    const MeshLOD& lod = mesh->lods()[0];
    std::vector<u32> seen;
    for (const Vertex& v : lod.vertices) {
        const float slot_f = v.uv1[0];
        NF_CHECK(slot_f >= 0.0f && slot_f <= 2.0f);
        NF_CHECK(v.uv1[1] >= 0.0f && v.uv1[1] <= 1.0f);
        // The band the vertex reports is the band its own height is in — the
        // layer list reads the same heights whether they came from noise or an
        // imported raster.
        const TerrainSplat expect = terrain_splat(v.position[1], 0.0f, layers);
        NF_CHECK_EQ(static_cast<u32>(slot_f), expect.slot);
        seen.push_back(static_cast<u32>(slot_f));
    }
    // The imported terrain spans all three bands, so all three textures are
    // bound: a heightmap that only reached the low band would leave the rock
    // texture sampled but never drawn.
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    NF_CHECK_EQ(seen.size(), 3u);
    NF_CHECK_EQ(lod.material_slots.size(), 3u);
}

NF_TEST(terrain_mesh_from_a_heightfield_is_deterministic) {
    DecodedImage img;
    img.width = 4;
    img.height = 4;
    img.rgba.resize(64u);
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            put_px(img, i, j, static_cast<uint8_t>(30 * i + 20 * j), 0, 0);
        }
    }
    const HeightField f = build_heightfield_from_image(img);
    NF_CHECK(f.ok());
    TerrainOptions opt;
    opt.resolution = 17;
    opt.size = 48.0f;
    opt.height_scale = 5.0f;
    opt.heightmap = &f;
    auto a = build_terrain_mesh(opt, mountain_layers());
    auto b = build_terrain_mesh(opt, mountain_layers());
    NF_CHECK(a != nullptr && b != nullptr);
    // A fixed raster and a pure interpolation: two builds are bit-identical, on
    // every platform, the same way the noise path is (design doc Section 114).
    NF_CHECK_EQ(a->lods()[0].vertices.size(), b->lods()[0].vertices.size());
    for (size_t i = 0; i < a->lods()[0].vertices.size(); ++i) {
        const Vertex& va = a->lods()[0].vertices[i];
        const Vertex& vb = b->lods()[0].vertices[i];
        NF_CHECK(va.position[1] == vb.position[1]);
        NF_CHECK(va.uv1[0] == vb.uv1[0]);
    }
}

NF_TEST(terrain_height_without_a_valid_heightfield_is_the_noise_path) {
    TerrainOptions opt;
    opt.resolution = 9;
    opt.size = 18.0f;
    opt.height_scale = 5.0f;
    auto noise = build_terrain_mesh(opt);
    // A nullptr field, and a field that failed to decode, must both build the
    // noise terrain every existing caller already builds — or attaching a
    // heightmap would change every scene that never asked for one.
    opt.heightmap = nullptr;
    NF_CHECK(build_terrain_mesh(opt)->lods()[0].vertices.size() == noise->lods()[0].vertices.size());
    HeightField empty; // width 0: ok() is false
    opt.heightmap = &empty;
    auto fallback = build_terrain_mesh(opt);
    NF_CHECK_EQ(fallback->lods()[0].vertices.size(), noise->lods()[0].vertices.size());
    for (size_t i = 0; i < noise->lods()[0].vertices.size(); ++i) {
        NF_CHECK(fallback->lods()[0].vertices[i].position[1] == noise->lods()[0].vertices[i].position[1]);
    }
}

NF_TEST(heightfield_from_a_missing_file_reports_the_failure) {
    std::string err;
    const HeightField f = build_heightfield_from_image("nf_does_not_exist_heightmap.png", err);
    // A missing or undecodable file is reported, not swallowed: the caller
    // decides whether to fall back to noise or fail the load, and an empty
    // field with no error would mean a valid image of flat terrain.
    NF_CHECK(!f.ok());
    NF_CHECK(!err.empty());
}

namespace {

/// Writes a binary P5 PGM (the format every paint program exports a greyscale
/// heightmap as) and returns the path. `heights` is [0, 1]; it is quantised to
/// 8 bits, so a test that needs exact values must use multiples of 1/255.
std::string write_pgm(const std::string& name, u32 w, u32 h, const std::vector<float>& heights) {
    const std::filesystem::path path = std::filesystem::current_path() / name;
    std::string out = "P5\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
    out.reserve(out.size() + w * h);
    for (const float v : heights) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(
            std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f))));
    }
    std::ofstream f(path, std::ios::binary);
    f << out;
    f.close();
    return path.string();
}

} // namespace

NF_TEST(heightfield_imports_a_binary_pgm_heightmap) {
    // 4x4 ramp rising along +X and +Z: h = (i + 4*j) / 15. Both axes vary, so
    // the corners pin the row order — a decoder that flipped the image
    // vertically would hand the terrain the last row first, and every corner
    // assertion below would move. Values are exact multiples of 1/255 once
    // quantised, so the file round trip (P5 header, maxval scaling, row-major
    // byte order) is what this measures.
    const u32 w = 4u;
    const u32 h = 4u;
    std::vector<float> heights;
    for (u32 j = 0u; j < h; ++j) {
        for (u32 i = 0u; i < w; ++i) {
            heights.push_back(static_cast<float>(i + 4u * j) / 15.0f);
        }
    }
    const std::string path = write_pgm("nf_test_terrain_ramp.pgm", w, h, heights);
    std::error_code ec;
    const auto cleanup = [&]() { std::filesystem::remove(path, ec); };

    std::string err;
    const HeightField f = build_heightfield_from_image(path, err);
    if (!err.empty()) {
        cleanup();
        NF_CHECK(err.empty());
    }
    NF_CHECK(f.ok());
    NF_CHECK_EQ(f.width, w);
    NF_CHECK_EQ(f.height, h);
    // Row 0 is the -Z edge and the image reads the same way up from above, so
    // the first stored height is the first pixel the file holds.
    NF_CHECK_NEAR(f.heights[0u], 0.0f, 0.5f / 255.0f);              // (0, 0)
    NF_CHECK_NEAR(f.heights[w - 1u], 3.0f / 15.0f, 0.5f / 255.0f);  // (+X, 0)
    NF_CHECK_NEAR(f.heights[(h - 1u) * w], 12.0f / 15.0f, 0.5f / 255.0f); // (0, +Z)
    NF_CHECK_NEAR(f.heights[w * h - 1u], 1.0f, 0.5f / 255.0f);      // (+X, +Z)
    for (const float v : f.heights) NF_CHECK(v >= 0.0f && v <= 1.0f);

    // The imported field is the same field the in-memory builder would have
    // made from the same raster, so an artist's .pgm reaches the mesh exactly
    // the way a procedural one does.
    TerrainOptions opt;
    opt.resolution = 17;
    opt.size = 32.0f;
    opt.height_scale = 9.0f;
    opt.heightmap = &f;
    auto mesh = build_terrain_mesh(opt);
    NF_CHECK(mesh != nullptr);
    for (const Vertex& v : mesh->lods()[0].vertices) {
        NF_CHECK(v.position[1] >= 0.0f && v.position[1] <= 9.0f + 1e-4f);
    }
    cleanup();
}

NF_TEST(heightfield_import_rejects_a_corrupt_pgm_header) {
    // A file that is not an image at all: the decoder reports the failure and
    // the field stays empty, so the terrain falls back to noise instead of
    // building from garbage.
    const std::filesystem::path path =
        std::filesystem::current_path() / "nf_test_terrain_corrupt.pgm";
    {
        std::ofstream f(path, std::ios::binary);
        f << "not an image at all";
    }
    std::string err;
    const HeightField f = build_heightfield_from_image(path.string(), err);
    NF_CHECK(!f.ok());
    NF_CHECK(!err.empty());
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
