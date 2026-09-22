// NF/Rendering/Terrain.cpp — deterministic heightfield terrain.

#include <NF/Rendering/Terrain.hpp>
#include <NF/Core/Math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace nf::rendering {

namespace {

// Integer lattice hash -> [0, 1). Bit-mixing (no floats, no RNG state),
// so results are identical on every platform.
float lattice_noise(i32 x, i32 z, u64 seed) {
    u64 h = static_cast<u64>(static_cast<u32>(x) * 374761393u + static_cast<u32>(z) * 668265263u);
    h += seed * 1442695040888963407ull;
    h = (h ^ (h >> 13)) * 1274126177ull;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

float smooth_interp(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float value_noise_2d(float x, float z, u64 seed) {
    const i32 xi = static_cast<i32>(std::floor(x));
    const i32 zi = static_cast<i32>(std::floor(z));
    const float xf = x - static_cast<float>(xi);
    const float zf = z - static_cast<float>(zi);
    const float a = lattice_noise(xi, zi, seed);
    const float b = lattice_noise(xi + 1, zi, seed);
    const float c = lattice_noise(xi, zi + 1, seed);
    const float d = lattice_noise(xi + 1, zi + 1, seed);
    const float u = smooth_interp(xf);
    const float v = smooth_interp(zf);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

} // namespace

float HeightField::sample(const float u, const float v) const {
    if (!ok()) return 0.0f;
    // The field is the terrain's own raster, so a sample outside [0, 1] is off
    // the edge: clamping extends the border rather than tiling the far side
    // back across it.
    const float cu = std::clamp(u, 0.0f, 1.0f);
    const float cv = std::clamp(v, 0.0f, 1.0f);
    const float fx = cu * static_cast<float>(width - 1u);
    const float fy = cv * static_cast<float>(height - 1u);
    const u32 x0 = static_cast<u32>(fx); // fx >= 0, so the cast floors
    const u32 y0 = static_cast<u32>(fy);
    // The +1 neighbour clamps too, so the last texel column interpolates
    // against itself instead of reading past the end.
    const u32 x1 = std::min(x0 + 1u, width - 1u);
    const u32 y1 = std::min(y0 + 1u, height - 1u);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const float a = heights[static_cast<usize>(y0) * width + x0];
    const float b = heights[static_cast<usize>(y0) * width + x1];
    const float c = heights[static_cast<usize>(y1) * width + x0];
    const float d = heights[static_cast<usize>(y1) * width + x1];
    return (a * (1.0f - tx) + b * tx) * (1.0f - ty) +
           (c * (1.0f - tx) + d * tx) * ty;
}

namespace {

/// Rec.601 luma: the heightmap is a grayscale image in practice, but an
/// RGB-authored one (a sculpted height layer, a texture misused as a
/// heightmap) reads the same value its greyscale conversion would carry.
float luma(const uint8_t* px) {
    const float r = static_cast<float>(px[0]);
    const float g = static_cast<float>(px[1]);
    const float b = static_cast<float>(px[2]);
    return (0.299f * r + 0.587f * g + 0.114f * b) * (1.0f / 255.0f);
}

} // namespace

HeightField build_heightfield_from_image(const DecodedImage& image) {
    HeightField field;
    if (!image.ok()) return field;
    field.width = static_cast<u32>(image.width);
    field.height = static_cast<u32>(image.height);
    field.heights.resize(static_cast<usize>(field.width) * field.height);
    for (u32 j = 0; j < field.height; ++j) {
        for (u32 i = 0; i < field.width; ++i) {
            const usize src = (static_cast<usize>(j) * image.width + i) * 4u;
            field.heights[static_cast<usize>(j) * field.width + i] = luma(&image.rgba[src]);
        }
    }
    return field;
}

HeightField build_heightfield_from_image(const std::string& physical_path, std::string& out_err) {
    return build_heightfield_from_image(decode_image_file(physical_path, out_err));
}

float terrain_height(float x, float z, const TerrainOptions& options) {
    // The heightmap replaces the noise field rather than adding to it: an
    // imported terrain is the artist's data, and blending procedural noise
    // through it would change what they painted. The field is scaled by
    // height_scale exactly as the noise is, so the splat bands and the world
    // units stay one knob either way.
    if (options.heightmap != nullptr && options.heightmap->ok()) {
        const float u = (x + options.size * 0.5f) / options.size;
        const float v = (z + options.size * 0.5f) / options.size;
        return options.heightmap->sample(u, v) * options.height_scale;
    }
    if (options.octaves == 0) return 0.0f;
    const u32 octaves = std::min(options.octaves, 8u);
    float sum = 0.0f;
    float amplitude = 0.5f;
    float frequency = options.noise_frequency;
    float norm = 0.0f;
    for (u32 o = 0; o < octaves; ++o) {
        sum += amplitude * value_noise_2d(x * frequency, z * frequency,
                                          options.seed + o * 1013904223ull);
        norm += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.03f; // slightly off 2x: fewer axis-aligned artifacts
    }
    return (norm > 0.0f ? sum / norm : 0.0f) * options.height_scale;
}

/// The band covering a point, as the strength of each covering layer. A layer
/// applies when the height and the slope both fall inside its gates; slope is
/// the angle of the surface from +Y, so a flat field is 0 and a cliff is pi/2.
/// Layers stack from the ground up: the base is the LOWEST covering slot — the
/// ground the point sits on — and the blend is how far the surface has risen
/// into the layer above it. That ordering is what makes a thin sand layer under
/// thick grass recede (blend 0.8 = 80% grass) rather than the grass vanishing,
/// and it makes the result independent of the layer list's construction order.
TerrainSplat terrain_splat(const float height, const float slope,
                           const std::vector<TerrainLayer>& layers) {
    TerrainSplat out;
    if (layers.empty()) return out;

    u32 base = u32_max;
    float base_weight = 0.0f;
    bool have_base = false;
    for (const TerrainLayer& layer : layers) {
        if (height < layer.min_height || height > layer.max_height) continue;
        if (slope < layer.min_slope || slope > layer.max_slope) continue;
        if (!(layer.weight > 0.0f)) continue;
        if (!have_base || layer.texture_slot < base) {
            base = layer.texture_slot;
            base_weight = layer.weight;
            have_base = true;
        }
    }
    if (!have_base) return out;
    out.slot = base;

    // The surface blends into the slot above it, so a vertex both bands cover
    // is a mix rather than a hard edge. A gap in the slot numbering is not an
    // error: nothing covers the vertex at slot+1, and the blend stays 0.
    float next_weight = 0.0f;
    for (const TerrainLayer& layer : layers) {
        if (layer.texture_slot != base + 1u) continue;
        if (height < layer.min_height || height > layer.max_height) continue;
        if (slope < layer.min_slope || slope > layer.max_slope) continue;
        if (!(layer.weight > 0.0f)) continue;
        next_weight = layer.weight;
        break;
    }
    if (next_weight > 0.0f) {
        out.blend = next_weight / (base_weight + next_weight);
    }
    return out;
}

/// Writes the grid into `lod`, with a splat map when `layers` is non-empty.
/// Shared by both public builders so the base path and the layered path cannot
/// drift apart in vertex layout or index winding.
/// Returns the highest texture slot any vertex drew, or 0 for a plain terrain.
namespace {
u32 build_grid(MeshLOD& lod, const TerrainOptions& options,
               const std::vector<TerrainLayer>* layers) {
    const u32 res = std::min(options.resolution, 1024u); // absurd-input guard
    const float half = options.size * 0.5f;
    const float step = options.size / static_cast<float>(res - 1);
    const float eps = step * 0.5f;

    u32 max_slot = 0u;
    for (u32 j = 0; j < res; ++j) {
        for (u32 i = 0; i < res; ++i) {
            const float x = -half + static_cast<float>(i) * step;
            const float z = -half + static_cast<float>(j) * step;
            const float h = terrain_height(x, z, options);
            // Central-difference normal (finite epsilon, normalized).
            const float hx = terrain_height(x + eps, z, options) - terrain_height(x - eps, z, options);
            const float hz = terrain_height(x, z + eps, options) - terrain_height(x, z - eps, options);
            Vec3 n{-hx / (2.0f * eps), 1.0f, -hz / (2.0f * eps)};
            const float len = n.length();
            if (len > 1e-9f) n = n / len;
            else n = Vec3{0.0f, 1.0f, 0.0f};
            Vertex v{};
            v.position[0] = x;
            v.position[1] = h;
            v.position[2] = z;
            v.normal[0] = n.x;
            v.normal[1] = n.y;
            v.normal[2] = n.z;
            v.tangent[0] = 1.0f;
            v.tangent[1] = 0.0f;
            v.tangent[2] = 0.0f;
            v.tangent[3] = 1.0f;
            v.uv0[0] = static_cast<float>(i) / static_cast<float>(res - 1);
            v.uv0[1] = static_cast<float>(j) / static_cast<float>(res - 1);
            if (layers != nullptr && !layers->empty()) {
                // Slope from the normal the lighting already uses, so the band
                // a vertex picks is the band its shading implies.
                const float slope = std::acos(std::clamp(n.y, -1.0f, 1.0f));
                const TerrainSplat s = terrain_splat(h, slope, *layers);
                v.uv1[0] = static_cast<float>(s.slot);
                v.uv1[1] = s.blend;
                max_slot = std::max(max_slot, s.slot);
            }
            lod.vertices.push_back(v);
        }
    }
    for (u32 j = 0; j + 1 < res; ++j) {
        for (u32 i = 0; i + 1 < res; ++i) {
            const u32 i0 = j * res + i;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + res;
            const u32 i3 = i2 + 1;
            // Counter-clockwise from above (+Y): (i0, i2, i1), (i1, i2, i3).
            lod.indices.push_back(i0);
            lod.indices.push_back(i2);
            lod.indices.push_back(i1);
            lod.indices.push_back(i1);
            lod.indices.push_back(i2);
            lod.indices.push_back(i3);
        }
    }
    return max_slot;
}

} // namespace

std::unique_ptr<StaticMesh> build_terrain_mesh(const TerrainOptions& options,
                                               const std::string& name) {
    return build_terrain_mesh(options, std::vector<TerrainLayer>{}, name);
}

std::unique_ptr<StaticMesh> build_terrain_mesh(const TerrainOptions& options,
                                               const std::vector<TerrainLayer>& layers,
                                               const std::string& name) {
    if (options.resolution < 2 || !(options.size > 0.0f)) return nullptr;
    auto mesh = std::make_unique<StaticMesh>(name);
    MeshLOD& lod = mesh->lod(0);
    lod.vertices.reserve(static_cast<usize>(options.resolution) * options.resolution);
    lod.indices.reserve(static_cast<usize>(options.resolution - 1) *
                        (options.resolution - 1) * 6);

    const u32 max_slot = build_grid(lod, options, layers.empty() ? nullptr : &layers);

    SubMesh sm;
    sm.index_offset = 0;
    sm.index_count = static_cast<u32>(lod.indices.size());
    sm.vertex_offset = 0;
    sm.vertex_count = static_cast<u32>(lod.vertices.size());
    sm.material_slot = 0;
    lod.submeshes.push_back(sm);
    // One slot per texture the layers actually drew: a band list that skips an
    // index still gets a slot for it (the shader samples the array by index),
    // but a list whose top band never appears on this terrain does not.
    const u32 slot_count = layers.empty() ? 1u : max_slot + 1u;
    for (u32 s = 0u; s < slot_count; ++s) {
        lod.material_slots.push_back(MaterialSlot{"layer_" + std::to_string(s)});
    }
    StaticMesh::compute_lod_bounds(lod);
    return mesh;
}

} // namespace nf::rendering
