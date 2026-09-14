#pragma once

// NF/Rendering/MeshAsset.hpp — simple binary mesh asset ("cook / import")
//
// The full asset pipeline (source importers for glTF/OBJ, texture cooking,
// compression) is a later milestone. For now the runtime needs a stable,
// fast-to-load format it can round-trip meshes through, so the "cook" step
// is a byte-exact dump of the runtime representation:
//
//   Mesh Asset (NFMesh) --save--> disk --load--> Runtime StaticMesh --upload--> GPU
//
// Layout (little-endian):
//   magic      : 4 bytes  "NFM1"
//   name       : u32 length + bytes
//   lod_count  : u32
//   per LOD:
//     vertex_count : u32
//     vertices     : per vertex — pos(3f) normal(3f) tangent(4f) uv0(2f) uv1(2f)
//     index_count  : u32
//     indices      : u32 each
//     submesh_count: u32
//     submeshes    : per submesh — index_offset,index_count,vertex_offset,
//                    vertex_count,material_slot (5×u32) + aabb(6f) + sphere(4f)
//     aabb         : 6 floats
//     sphere       : 4 floats
//
// The vertex payload is written field-by-field (never memcpy of the struct)
// so the on-disk format does not depend on C++ layout or padding. New
// optional attributes (color, skinning) extend the format with a version
// bump, not by chance-alignment.

#include <NF/Rendering/StaticMesh.hpp>

#include <filesystem>
#include <memory>

namespace nf::rendering::mesh_asset {

inline constexpr char kMagic[4] = {'N', 'F', 'M', '1'};

/// Writes a runtime mesh to disk in the NFMesh format. Returns false on any
/// I/O failure; the destination file contents are unspecified then.
bool save_mesh_asset(const StaticMesh& mesh, const std::filesystem::path& path);

/// Reads an NFMesh file into `out_mesh` (replacing its contents). Returns
/// false on I/O failure, bad magic, or a truncated/corrupt payload.
bool load_mesh_asset(const std::filesystem::path& path, StaticMesh& out_mesh);

/// Convenience: load into a fresh mesh (nullptr on failure).
std::unique_ptr<StaticMesh> load_mesh_asset(const std::filesystem::path& path);

} // namespace nf::rendering::mesh_asset
