#pragma once

// NF/Rendering/LodGenerator.hpp — offline mesh simplification (Phase 12).
//
// Grid-clustering decimation: vertices sharing a spatial cell merge into one
// (averaged position/normal, first uv/tangent), degenerate triangles drop.
// Runs per submesh so material slots and index ranges stay valid.
//
// Properties callers rely on:
//   - Deterministic: ordered cell map, no hashing, no RNG. Same input bytes
//     produce same output bytes (design doc Section 114 + 240).
//   - Conservative: never invents vertices outside the source bounds, never
//     emits an index past the vertex list, never touches LOD 0.
//   - CPU-only, no device: the cooker, the importer and load-time code share it.

#include <NF/Rendering/StaticMesh.hpp>

#include <cstdint>
#include <vector>

namespace nf::rendering {

struct LodGenerateOptions {
    float target_ratio = 0.5f; // desired triangle fraction per extra level (0,1)
    u32 max_levels = 3;        // extra levels beyond LOD 0
    u32 min_triangles = 12;    // stop when a level drops below this
};

struct LodGenerateStats {
    u32 levels_built = 0; // extra levels appended (LOD 0 excluded)
    std::vector<u32> triangles; // per level, LOD 0 first
    std::vector<float> cell_sizes; // per extra level
};

/// One simplification step. Pure function: src is untouched, the result is a
/// complete MeshLOD (vertices, indices, submeshes, slots, bounds).
/// cell_size <= 0 returns a copy of src (no-op, never an error).
MeshLOD simplify_lod(const MeshLOD& src, float cell_size);

/// Appends simplified levels to mesh (LOD 0 is the source and is preserved).
/// Stops early when a level cannot reduce the triangle count further or
/// drops below min_triangles. Returns per-level statistics.
LodGenerateStats build_lods(StaticMesh& mesh, const LodGenerateOptions& options = {});

} // namespace nf::rendering
