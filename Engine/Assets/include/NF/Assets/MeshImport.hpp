#pragma once

// NF/Assets/MeshImport.hpp — the single READ-side entry point for mesh files.
//
// Before this header the engine could WRITE six mesh formats (MeshExport:
// .nfmesh, .obj, .stl, .ply, .gltf, .glb) and READ exactly one of them
// (GltfImport: .gltf/.glb). A round trip through the engine's own exporter was
// therefore impossible: exporting a mesh to OBJ produced a file the engine
// could not open again. This header closes that asymmetry.
//
// It also OWNS the import payload types. GltfImport.hpp includes this header
// and keeps `GltfImportResult` as an alias, so the glTF reader and the unified
// dispatcher publish one and the same result — a caller cannot accidentally
// handle "the glTF kind" differently from "the imported kind", because there is
// only one kind.
//
// Formats, and what each one honestly cannot carry:
//   NFMesh — the engine's cooked container. Lossless (it *is* MeshAsset).
//   glTF   — .gltf JSON; external .bin resolves beside the file.
//   GLB    — glTF binary container, everything embedded.
//   OBJ    — positions/normals/one UV channel, `o`/`g` groups, `usemtl` slots.
//            uv1 and tangents are not in the format; they default.
//   STL    — triangle soup. No indices, no UVs, no vertex normals: geometry
//            comes back flat-shaded, and that is reported, not hidden.
//   PLY    — positions/normals/UVs/faces, ASCII or binary (both endiannesses).
//            uv1 and tangents are not in the format; they default.
//
// Materials and textures come out too, because geometry without them is not an
// import a developer can use:
//   - glTF materials fill `materials` (base colour, metallic, roughness,
//     emissive) and resolve their base-colour map to an index in `images`.
//   - OBJ has no material parameters at all — `usemtl` only names one — so the
//     reader resolves the `.mtl` its `mtllib` line points at, and a submesh's
//     `material_slot` becomes the index of a real material.
//   - `images` holds every image in its ORIGINAL encoded bytes (PNG/JPEG as the
//     file stored them), ready to be written as a texture asset.
//   - The engine's material block has room for ONE texture and no specular,
//     clearcoat, sheen or normal map, so anything else the source declared is
//     NAMED per material in `GltfMaterialInfo::dropped`. A Blinn-Phong exponent
//     is never silently turned into a roughness.
//
// Rules this module obeys (project-wide, not optional):
//   - No silent format substitution. Whatever could not be used is counted in
//     the result's ledger or named in `warnings`.
//   - Extension wins over content when both are known; content sniffing is the
//     fallback for missing/unknown extensions. When the extension names one
//     format family and the bytes clearly belong to another, the import FAILS
//     with "extension/content mismatch" instead of guessing.
//   - Failure is `ok == false` plus a human-readable error. Nothing throws.
//   - All functions are synchronous and thread-safe (no shared state).

#include <NF/Assets/MeshAsset.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nf::assets {

// ---------------------------------------------------------------------------
// Shared import payload
// ---------------------------------------------------------------------------

/// PBR material factors in material order (slot i == materials[i]).
///
/// This is a deliberately narrow parameter set — what the engine's
/// rendering::MaterialAsset can actually store. Everything the source declared
/// that does not fit is NAMED in `dropped` rather than approximated: mapping a
/// Blinn-Phong exponent onto a roughness value, or folding a specular colour
/// into metallic, would be a silent substitution of a material the artist did
/// not author.
struct GltfMaterialInfo {
    std::string name;
    float base_color[4] = {1, 1, 1, 1};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float emissive[3] = {0, 0, 0};
    float emissive_strength = 1.0f;

    /// Index into MeshImportResult::images for the base-colour (albedo) texture,
    /// -1 when the material has none.
    int albedo_image = -1;
    /// glTF PBR map slots this pipeline now carries (indices into the same
    /// image list, -1 when absent). metallic_roughness follows the glTF
    /// packing the shader reads: G = roughness, B = metallic. occlusion is
    /// the glTF occlusionTexture (R channel).
    int normal_image = -1;
    int metallic_roughness_image = -1;
    int occlusion_image = -1;
    int emissive_image = -1;

    /// Slots and fields the source declared that this pipeline does not carry,
    /// by human-readable name ("normal map", "specular exponent"). Empty for a
    /// material that survived intact. Named, never implied.
    std::vector<std::string> dropped;
};

/// One image carried out of a model source, still in its ORIGINAL encoded form
/// (the PNG/JPEG bytes exactly as the file stored them). Decoding is the
/// renderer's job and re-encoding here would lose data for nothing — and the
/// bytes are what a texture asset needs to be written to disk.
struct MeshImportImage {
    std::string name;      // "Hero_BaseColor", or the source file's stem
    std::string source;    // where it came from: a glTF uri, a bufferView, a map_Kd path
    std::string extension; // ".png"/".jpg"/... derived from the bytes, never from the name
    std::vector<uint8_t> bytes;
};

/// File extension (with the dot) for encoded image bytes, from their magic
/// number. Empty when the bytes are not an image this engine decodes. Sniffing
/// beats trusting a source's own naming: a .gltf may point a "texture.png" uri
/// at JPEG bytes, and the extension is what a decoder dispatch keys on.
const char* image_extension_for_bytes(std::span<const uint8_t> bytes);

/// One node: local transform + optional mesh binding (index into the result's
/// meshes, -1 when the node carries no mesh).
struct GltfNodeInfo {
    std::string name;
    float translation[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 1}; // xyzw quaternion
    float scale[3] = {1, 1, 1};
    int mesh_index = -1;
    int parent_index = -1;
    int skin_index = -1; // skin bound to this node, -1 when none
};

/// Which local transform component an animation channel drives.
enum class GltfAnimationPath {
    Translation,
    Rotation,
    Scale,
};

/// One animation channel: the sampled curve for (node, path). Times are in
/// seconds and sorted ascending; values are 3 floats per key (TRS paths) or
/// 4 floats per key (rotation, xyzw quaternion order, matching glTF).
struct GltfAnimationChannel {
    int node = -1;
    GltfAnimationPath path = GltfAnimationPath::Translation;
    std::vector<float> times;
    std::vector<float> values; // component count per key (3, or 4 for Rotation)
};

/// One animation: a flat list of channels plus the duration implied by the
/// samplers (glTF has no explicit clip duration; this is the maximum input time
/// across all channels).
struct GltfAnimationInfo {
    std::string name;
    float duration = 0.0f;
    std::vector<GltfAnimationChannel> channels;
};

/// One skin: the joint nodes in skin order, the skin's root node, and the
/// inverse bind matrices (16 floats per joint, column-major as in glTF).
struct GltfSkinInfo {
    std::string name;
    std::vector<int> joint_nodes; // node indices, glTF joint order
    int root_node = -1;           // -1 when the file does not name a skeleton root
    std::vector<float> inverse_bind_matrices; // 16 per joint
};

/// Per-vertex skin binding for the mesh at the matching index of
/// `MeshImportResult::meshes`. `skin_index == -1` means the mesh is static —
/// recorded explicitly, never substituted with a fake rig.
struct GltfMeshSkin {
    int skin_index = -1;
    u32 vertex_count = 0;
    std::vector<u16> joints;    // 4 per vertex, joint indices into GltfSkinInfo
    std::vector<float> weights; // 4 per vertex, normalized to sum 1
};

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------

/// Every mesh format this module can READ, in display order. The read side
/// covers the same six containers as MeshFormat's write side, which is what
/// makes a round trip a test rather than a hope.
enum class MeshImportFormat : uint8_t {
    Unknown = 0,
    NfMesh = 1,
    Gltf = 2,
    Glb = 3,
    Obj = 4,
    Stl = 5,
    Ply = 6,
};

/// Every format import_mesh_*() accepts, in display order.
const std::vector<MeshImportFormat>& mesh_import_formats();

/// Display name ("Wavefront OBJ", "glTF 2.0 (binary)").
const char* mesh_import_format_name(MeshImportFormat format);
/// File extension without the dot ("obj", "glb"). Empty for Unknown.
const char* mesh_import_format_extension(MeshImportFormat format);
/// Parses an extension (with or without the dot, any case) into a format.
/// Returns false for anything this module does not read.
bool mesh_import_format_from_extension(std::string_view extension, MeshImportFormat& out);

/// The format family a format belongs to. glTF and GLB share one family: the
/// same parser reads both, so a .gltf holding GLB bytes is that parser's
/// mismatch to report, not this dispatcher's.
MeshImportFormat mesh_import_format_family(MeshImportFormat format);

/// Content sniffing, for files with a missing or lying extension. Returns
/// Unknown when the bytes carry no recognisable signature (plain OBJ text is
/// the important case — it has none, by design of the format).
MeshImportFormat sniff_mesh_format(std::span<const uint8_t> bytes);

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

/// The result of one import.
struct MeshImportResult {
    bool ok = false;
    std::string error;
    MeshImportFormat format = MeshImportFormat::Unknown;

    std::vector<std::unique_ptr<MeshAsset>> meshes; // >= 1 on success
    /// Parallel to `meshes`: the name the source gave each one (glTF mesh name,
    /// OBJ `o`/`g` name, or the file stem when the source names nothing).
    std::vector<std::string> mesh_names;

    // --- the payload every format fills: meshes, materials, images. ---
    /// PBR material factors in material order. A submesh's `material_slot`
    /// indexes this list — and the list can be EMPTY while slots still read 0,
    /// because neither OBJ nor glTF requires a primitive to name a material. A
    /// consumer must bounds-check rather than assume slot 0 exists.
    std::vector<GltfMaterialInfo> materials;
    std::vector<GltfNodeInfo> nodes;
    std::vector<GltfSkinInfo> skins;
    std::vector<GltfAnimationInfo> animations;
    std::vector<GltfMeshSkin> mesh_skins;

    /// Every image the source carried, in encoded form. A material's
    /// `albedo_image` indexes this list. Empty when the source has no textures.
    std::vector<MeshImportImage> images;

    // --- the skip ledger. Nothing here is ever silently dropped. ---
    u32 primitives_skipped = 0;     // non-triangle / unusable primitives (glTF)
    u32 anim_channels_skipped = 0;  // STEP / CUBICSPLINE / morph (glTF)
    u32 skin_bindings_rejected = 0; // malformed JOINTS_0/WEIGHTS_0 (glTF)
    u32 faces_skipped = 0;          // malformed faces/triangles (OBJ/STL/PLY)
    u32 vertices_skipped = 0;       // vertices with non-finite components
    u32 images_skipped = 0;         // images that could not be read out
    /// Non-fatal notes a developer must be able to see: "STL carries no vertex
    /// normals, geometry is flat-shaded", "uv1/tangent are not in OBJ", ...
    std::vector<std::string> warnings;
};

// ---------------------------------------------------------------------------
// Shared geometry helpers
//
// Every reader finishes its mesh through these two, so no two formats can
// disagree about what "the bounds" or "smooth normals" mean — which is what
// makes an export/import round trip comparable across formats at all.
// ---------------------------------------------------------------------------

/// Recomputes the AABB and the sphere about its center from the current vertex
/// array, and mirrors both onto every submesh (the renderer keeps submesh
/// bounds equal to the LOD's — see StaticMesh::compute_bounds). A mesh with no
/// vertices gets zeroed bounds.
void recompute_mesh_bounds(MeshAsset& mesh);

/// Angle-averaged normals over the triangles in
/// indices[index_start, index_start + index_count), accumulated onto every
/// vertex from `base_vertex` on and then normalized. An accumulator that
/// cancels to zero becomes (0,0,1) rather than a vector a shader would divide
/// by. Existing normals in that range are overwritten, never blended.
void compute_smooth_normals(std::vector<AssetVertex>& vertices,
                            const std::vector<u32>& indices,
                            usize base_vertex, usize index_start, usize index_count);

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

/// Parses mesh bytes whose format is taken from `logical_path`'s extension,
/// falling back to content sniffing when the extension is unknown or absent.
MeshImportResult import_mesh_memory(std::span<const uint8_t> bytes,
                                    const std::string& logical_path = "<memory>");

/// import_mesh_memory() with the format stated explicitly. `logical_path` is
/// only used for naming and diagnostics.
MeshImportResult import_mesh_as(std::span<const uint8_t> bytes, MeshImportFormat format,
                                const std::string& logical_path = "<memory>");

/// Loads `path` and imports it. glTF/GLB go through import_gltf_file so that a
/// .gltf's sibling .bin resolves; every other format is read into memory and
/// handed to import_mesh_memory().
MeshImportResult import_mesh_file(const std::string& path);

} // namespace nf::assets
