#pragma once

// NF/Assets/MeshExport.hpp — mesh writers for the editor's Export action.
//
// MeshAsset is CPU-pure data (bytes in, bytes out), so every writer here is a
// pure function of the asset: no device, no filesystem unless the caller asks
// for export_mesh_to_file(). The editor's File > Export dialog offers exactly
// the formats in mesh_formats(), and tests in Tests/AssetTests cover each one
// by re-reading what it wrote.
//
// Formats:
//   NFMesh — the engine's own cooked container (binary, round-trips losslessly)
//   OBJ    — Wavefront OBJ + a sidecar .mtl naming one slot per submesh
//   STL    — binary STL, one solid, triangles only (no UVs, by definition)
//   PLY    — ASCII PLY with positions, normals, UVs and faces
//   GLTF   — glTF 2.0 JSON + sibling .bin buffer (self-contained pair)
//   GLB    — glTF 2.0 binary container (everything in one file)

#include <NF/Assets/MeshAsset.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nf::assets {

enum class MeshFormat : uint8_t {
    NfMesh = 0,
    Obj = 1,
    Stl = 2,
    Ply = 3,
    Gltf = 4,
    Glb = 5,
};

/// Every format the export dialog can offer, in display order.
const std::vector<MeshFormat>& mesh_formats();

/// Display name ("Wavefront OBJ", "glTF 2.0 (binary)") — shown in the dialog.
const char* mesh_format_name(MeshFormat format);
/// File extension without the dot ("obj", "glb", ...) — used for Save-As filters.
const char* mesh_format_extension(MeshFormat format);

/// Parses an extension (with or without the dot, any case) into a format.
/// Returns false when the extension is not one this module writes.
bool mesh_format_from_extension(std::string_view extension, MeshFormat& out);

/// One file produced by an export. `relative_path` is what the caller joins
/// with the chosen output path: the primary file carries the chosen name and
/// any sidecar (OBJ's .mtl, glTF's .bin) sits beside it.
struct MeshExportFile {
    std::string relative_path;
    std::vector<uint8_t> bytes;
};

struct MeshExportResult {
    bool ok = false;
    std::string error;
    // files[0] is always the primary file; the rest are sidecars.
    std::vector<MeshExportFile> files;
};

/// Serializes `mesh` in `format`. `base_name` is the file name the primary
/// file should get (with or without an extension); sidecar names are derived
/// from it. Never touches the filesystem.
MeshExportResult export_mesh(const MeshAsset& mesh, MeshFormat format, const std::string& base_name);

/// export_mesh() + writing every produced file next to `physical_path`
/// (creating parent directories as needed). On success `out_error` is clear and
/// the primary file is exactly `physical_path`; the format is taken from
/// `physical_path`'s extension unless `format_override` is given.
bool export_mesh_to_file(const MeshAsset& mesh, const std::string& physical_path, std::string& out_error,
                         bool has_format_override = false, MeshFormat format_override = MeshFormat::Obj);

/// Copies `mesh` is NOT what this module does — world-baking needs the scene
/// euler convention (scene::compose_trs_mat4), and NFAssets deliberately does
/// not depend on NFScene. The editor owns that step (EditorApp::
/// export_meshes_to_file), exactly like rendering::make_mesh_asset owns the
/// StaticMesh <-> MeshAsset boundary: the glue lives with the type that has to
/// know both sides.

} // namespace nf::assets
