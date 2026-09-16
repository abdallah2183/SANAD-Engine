// NF/Rendering/Terrain.cpp — deterministic heightfield terrain.

#include <NF/Rendering/Terrain.hpp>
#include <NF/Core/Math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

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

float terrain_height(float x, float z, const TerrainOptions& options) {
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

std::unique_ptr<StaticMesh> build_terrain_mesh(const TerrainOptions& options,
                                               const std::string& name) {
    if (options.resolution < 2 || !(options.size > 0.0f)) return nullptr;
    const u32 res = std::min(options.resolution, 1024u); // absurd-input guard
    auto mesh = std::make_unique<StaticMesh>(name);
    MeshLOD& lod = mesh->lod(0);
    lod.vertices.reserve(static_cast<usize>(res) * res);
    lod.indices.reserve(static_cast<usize>(res - 1) * (res - 1) * 6);

    const float half = options.size * 0.5f;
    const float step = options.size / static_cast<float>(res - 1);
    const float eps = step * 0.5f;

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
    SubMesh sm;
    sm.index_offset = 0;
    sm.index_count = static_cast<u32>(lod.indices.size());
    sm.vertex_offset = 0;
    sm.vertex_count = static_cast<u32>(lod.vertices.size());
    sm.material_slot = 0;
    lod.submeshes.push_back(sm);
    lod.material_slots.push_back(MaterialSlot{"default"});
    StaticMesh::compute_lod_bounds(lod);
    return mesh;
}

} // namespace nf::rendering
