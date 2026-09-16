#pragma once

// NF/Rendering/Terrain.hpp — procedural heightfield terrain (design doc 59).
//
// Deterministic value-noise fBm height function + indexed grid mesh with
// analytic-ish normals (central differences). CPU-only, no device: shares
// the LOD generator and the StaticMesh upload path with every other mesh.
//
// Determinism: integer-lattice hashing (no RNG state, no floats in the
// hash), so the same (x, z, seed) always yields the same height on every
// platform (design doc Section 114).

#include <NF/Core/Types.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <cstdint>
#include <memory>

namespace nf::rendering {

struct TerrainOptions {
    u32 resolution = 64; // vertices per side (>= 2)
    float size = 100.0f; // world extent in X and Z
    float height_scale = 10.0f; // multiplies the 0..1 noise field
    float noise_frequency = 0.03f; // features per world unit
    u32 octaves = 4; // fBm octaves (0 = flat)
    u64 seed = 1;
};

/// Height in world units at (x, z). Pure and deterministic.
float terrain_height(float x, float z, const TerrainOptions& options);

/// Builds an indexed grid mesh centered on the origin (XZ plane, +Y up).
/// Returns nullptr when the options are degenerate (resolution < 2).
std::unique_ptr<StaticMesh> build_terrain_mesh(const TerrainOptions& options,
                                               const std::string& name = "Terrain");

} // namespace nf::rendering
