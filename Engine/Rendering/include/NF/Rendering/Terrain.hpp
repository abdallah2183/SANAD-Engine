#pragma once

// NF/Rendering/Terrain.hpp — procedural heightfield terrain (design doc 59).
//
// Deterministic value-noise fBm height function, an optional raster heightmap
// in its place, and an indexed grid mesh with analytic-ish normals (central
// differences). CPU-only, no device: shares the LOD generator and the
// StaticMesh upload path with every other mesh.
//
// Determinism: integer-lattice hashing (no RNG state, no floats in the
// hash), so the same (x, z, seed) always yields the same height on every
// platform (design doc Section 114). The heightmap path is deterministic by
// construction — a fixed raster and a pure interpolation.

#include <NF/Core/Types.hpp>
#include <NF/Rendering/ImageDecode.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nf::rendering {

/// A grayscale raster the terrain reads instead of the fBm noise (design doc
/// 59: heightmaps). The terrain samples it in its OWN uv space —
/// `u = (x + size/2) / size`, `v = (z + size/2) / size` — so a 64x64 field can
/// drive a 1025x1025 mesh without the mesh resolution dictating the height
/// data, and the same field answers `terrain_height` outside the mesh builder:
/// the cubes and the camera in Basic3D plant themselves on the imported data
/// rather than on the noise the builder happened to use.
struct HeightField {
    u32 width = 0;
    u32 height = 0;
    /// [0, 1], row-major. Row 0 is the `-Z` edge, so an image laid on the
    /// terrain appears the same way up viewed from above as it does in a paint
    /// program.
    std::vector<float> heights;

    bool ok() const {
        return width > 0 && height > 0 &&
               heights.size() == static_cast<usize>(width) * static_cast<usize>(height);
    }

    /// Bilinear sample at (u, v) in [0, 1]. Addressing CLAMPS at the edges: the
    /// field is the terrain's own raster, so a position outside it is off the
    /// edge, and extending the border keeps an overshooting mesh continuous
    /// instead of tiling a seam. An empty field returns 0 — a
    /// default-constructed HeightField is flat ground, never a crash.
    float sample(float u, float v) const;
};

/// Builds a heightfield from a decoded image. Heights are Rec.601 luma of the
/// RGB channels, so a grayscale PNG and an RGB-authored heightmap both read the
/// same; the alpha stb fills in for opaque decodes is ignored. An invalid image
/// yields an empty field — the caller checks `ok()`, and the terrain falls back
/// to noise rather than rendering flat silently.
HeightField build_heightfield_from_image(const DecodedImage& image);

/// As above, decoding `physical_path` first. `out_err` receives the decode
/// failure when the file is not a usable image; an empty field with an empty
/// error means the file decoded but carries no height range.
HeightField build_heightfield_from_image(const std::string& physical_path, std::string& out_err);

struct TerrainOptions {
    u32 resolution = 64; // vertices per side (>= 2)
    float size = 100.0f; // world extent in X and Z
    float height_scale = 10.0f; // multiplies the 0..1 noise field
    float noise_frequency = 0.03f; // features per world unit
    u32 octaves = 4; // fBm octaves (0 = flat)
    u64 seed = 1;
    /// When non-null and valid, replaces the noise field. Not owned: options is
    /// a plain data struct, so the field must outlive it (a mesh build is
    /// synchronous, so a stack local is enough).
    const HeightField* heightmap = nullptr;
};

/// One band of the surface, selected by where a vertex sits (design doc 59:
/// splat maps and layers).
struct TerrainLayer {
    /// Texture slot the band draws. Slots are dense from 0: a layer list of
    /// three bands uses 0, 1, 2 and the mesh grows a material slot for each.
    u32 texture_slot = 0;
    /// World-Y band. A vertex inside it takes this layer's strength; a vertex
    /// outside it contributes nothing.
    float min_height = 0.0f;
    float max_height = 0.0f;
    /// Slope gate in radians from +Y. A band that only owns steep ground (rock)
    /// or only flat ground (sand) sets both; a band that owns every slope leaves
    /// them at the defaults.
    float min_slope = 0.0f;
    float max_slope = 3.14159265358979323846f;
    /// How strongly this band wins where it applies, before normalisation.
    /// Two overlapping bands at 1.0 each blend half and half; a band at 0.25
    /// against one at 1.0 takes a fifth of the surface.
    float weight = 1.0f;
};

/// The splat the mesh builder writes into `uv1`. The vertex carries two
/// channels, so the encoding is a slot index plus how far the surface has
/// moved into the *next* slot — `uv1 = (slot, blend)`. A shader samples the
/// array at `slot` and `slot + 1` and mixes them by `blend`, which serves any
/// number of layers in the two attributes the mesh already uploads. Blending
/// is between adjacent slots by construction, so a band that skips a slot
/// blends with the one after the gap only if it covers it too.
struct TerrainSplat {
    u32 slot = 0;
    float blend = 0.0f;
};

/// Height in world units at (x, z). Pure and deterministic. Reads
/// `options.heightmap` when one is attached — the field is sampled in terrain
/// uv and scaled by `height_scale` exactly as the noise field is, so the two
/// sources are interchangeable and the splat bands see the same world heights
/// either way.
float terrain_height(float x, float z, const TerrainOptions& options);

/// The band covering (height, slope): the slot to draw and how far the surface
/// has moved into the slot above it. Pure — the same inputs always pick the
/// same pair. An empty list, or one where no band covers the point, returns
/// the base slot with no blend.
TerrainSplat terrain_splat(float height, float slope, const std::vector<TerrainLayer>& layers);

/// Builds an indexed grid mesh centered on the origin (XZ plane, +Y up).
/// Returns nullptr when the options are degenerate (resolution < 2).
std::unique_ptr<StaticMesh> build_terrain_mesh(const TerrainOptions& options,
                                               const std::string& name = "Terrain");

/// As above, with a splat map: `uv1` carries the layer weights the shader
/// blends. Empty or degenerate layer lists are ignored rather than rejected —
/// a terrain with no layers still renders as the base texture.
std::unique_ptr<StaticMesh> build_terrain_mesh(const TerrainOptions& options,
                                               const std::vector<TerrainLayer>& layers,
                                               const std::string& name = "Terrain");

} // namespace nf::rendering
