// NF/Assets/MeshExport.cpp — the writers behind the editor's Export action.
//
// One asset in, bytes out: every writer here is a pure function of a MeshAsset
// (see the header), so a format is checked by re-reading what it wrote rather
// than by eyeballing a file. Four rules keep the output loadable by real tools
// *and* repeatable in CI:
//
//   * Deterministic — nothing here reads the clock, the locale or an unordered
//     container, so the same asset always produces the same bytes.
//   * Locale-independent floats — every float that reaches a text format goes
//     through snprintf("%.6g"), which always emits '.'; std::to_string prints
//     six fixed decimals and std::ostream obeys the global locale, so neither
//     may touch file content.
//   * Submeshes are honored — faces and primitives come from a submesh's
//     index_offset/index_count and start at its vertex_offset, never from "the
//     whole buffer", so a multi-slot mesh survives the round trip.
//   * Corrupt input is survivable — every index is range-checked before it is
//     dereferenced, and a mesh with missing or bogus submeshes still exports
//     something a tool can open instead of crashing the editor.
//
// The euler convention is deliberately not re-derived here: bake_transform()
// calls scene::compose_trs_mat4, the same helper the runtime composes
// transforms with. Core once grew a second euler conversion that disagreed
// with this one and produced incompatible .nfmesh files; NFAssets links NFScene
// precisely so that this file can borrow the convention instead of copying it.

#include <NF/Assets/MeshExport.hpp>

#include <NF/Core/FileSystem.hpp>
#include <NF/Core/Math.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace nf::assets {

namespace {

constexpr const char* kGenerator = "NOVAForge MeshExport";

// The neutral albedo both OBJ's Kd and glTF's baseColorFactor get: a MeshAsset
// carries no material data, and inventing colours would be worse than saying
// "untextured grey".
constexpr const char* kNeutralKd = "0.8 0.8 0.8";

// --- text ---------------------------------------------------------------------

// The one float formatter. snprintf writes '.' for the decimal point no matter
// what setlocale() the host process installed, which the C++ formatting helpers
// do not promise, and that is the reason this file never uses them for content.
std::string fmt_float(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return std::string(buf);
}

std::string fmt_vec2(const float* v) {
    return fmt_float(v[0]) + " " + fmt_float(v[1]);
}

std::string fmt_vec3(const float* v) {
    return fmt_float(v[0]) + " " + fmt_float(v[1]) + " " + fmt_float(v[2]);
}

// JSON numbers have to be finite: one corrupt vertex would otherwise turn the
// whole document into something no parser accepts.
std::string json_number(float v) {
    return std::isfinite(v) ? fmt_float(v) : std::string("0");
}

// Escaping for the one user-derived string in a glTF document (the buffer's
// file name): a project called `weird"name.gltf` must still parse.
std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned code = static_cast<unsigned char>(c);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (code < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", code);
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

bool ends_with_ci(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) return false;
    return equals_ci(text.substr(text.size() - suffix.size()), suffix);
}

// --- little-endian binary -----------------------------------------------------
// Written byte by byte rather than by memcpy of a native value: the formats say
// little-endian, and a host that is not must not silently write a file nobody
// can read.

void push_u16_le(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
}

void push_u32_le(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

void push_f32_le(std::vector<u8>& out, float v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    push_u32_le(out, bits);
}

std::vector<u8> string_bytes(const std::string& text) {
    std::vector<u8> bytes(text.size());
    for (usize i = 0; i < text.size(); ++i) bytes[i] = static_cast<u8>(text[i]);
    return bytes;
}

// --- shapes shared by the writers ---------------------------------------------

// The references inside OBJ/glTF are relative to the file that holds them, so
// they never carry the directory the caller chose; a sidecar's own path keeps
// it, since that is what export_mesh_to_file writes.
struct ExportNames {
    std::string primary;    // primary file name, extension included
    std::string stem;       // primary minus its extension: sidecar prefix
    std::string stem_name;  // stem with no directory: what the files reference
};

// A complete triangle, already range-checked against the vertex array.
struct Triangle {
    u32 a = 0;
    u32 b = 0;
    u32 c = 0;
};

// The submeshes a writer should walk. A cooked asset normally has at least one,
// but an asset built by hand may hold indices without any: treating the whole
// buffer as a single slot is what "export this mesh" means there, and it keeps
// the writers uniform.
std::vector<AssetSubMesh> writable_submeshes(const MeshAsset& mesh) {
    if (!mesh.submeshes.empty()) return mesh.submeshes;
    if (mesh.indices.empty()) return {};

    AssetSubMesh whole;
    whole.index_offset = 0;
    whole.index_count = static_cast<u32>(mesh.indices.size());
    whole.vertex_offset = 0;
    whole.vertex_count = static_cast<u32>(mesh.vertices.size());
    whole.material_slot = 0;
    return {whole};
}

// Appends the triangles of one submesh. Incomplete triangles at the end of the
// range and indices outside the vertex array are dropped: both are what a
// truncated cook looks like, and a writer that trusted them would read past the
// end of the vertex buffer.
void append_triangles(const MeshAsset& mesh, const AssetSubMesh& sub, std::vector<Triangle>& out) {
    const usize first = static_cast<usize>(sub.index_offset);
    if (first >= mesh.indices.size()) return;

    const usize available = mesh.indices.size() - first;
    const usize wanted = static_cast<usize>(sub.index_count);
    const usize usable = wanted < available ? wanted : available;

    for (usize i = 0; i + 3 <= usable; i += 3) {
        const u32 a = mesh.indices[first + i];
        const u32 b = mesh.indices[first + i + 1];
        const u32 c = mesh.indices[first + i + 2];
        if (static_cast<usize>(a) >= mesh.vertices.size() ||
            static_cast<usize>(b) >= mesh.vertices.size() ||
            static_cast<usize>(c) >= mesh.vertices.size()) {
            continue;
        }
        out.push_back(Triangle{a, b, c});
    }
}

// Every triangle of the asset, submesh by submesh, in a stable order.
std::vector<Triangle> collect_triangles(const MeshAsset& mesh) {
    std::vector<Triangle> triangles;
    for (const AssetSubMesh& sub : writable_submeshes(mesh)) {
        append_triangles(mesh, sub, triangles);
    }
    return triangles;
}

// Mesh AABB + sphere about the AABB center, recomputed here rather than by
// calling rendering::make_mesh_asset: NFAssets must not link NFRendering (see
// Engine/Assets/CMakeLists.txt). Same shape as GltfImport's local helper and as
// StaticMesh::compute_lod_bounds, which is the consumer of these fields.
void recompute_bounds(MeshAsset& mesh) {
    mesh.bounds = AssetAABB{};
    mesh.sphere = AssetSphere{};
    if (mesh.vertices.empty()) return;

    const float* first = mesh.vertices[0].position;
    AssetAABB box;
    box.min_x = box.max_x = first[0];
    box.min_y = box.max_y = first[1];
    box.min_z = box.max_z = first[2];
    for (const AssetVertex& v : mesh.vertices) {
        const float* p = v.position;
        if (p[0] < box.min_x) box.min_x = p[0];
        if (p[0] > box.max_x) box.max_x = p[0];
        if (p[1] < box.min_y) box.min_y = p[1];
        if (p[1] > box.max_y) box.max_y = p[1];
        if (p[2] < box.min_z) box.min_z = p[2];
        if (p[2] > box.max_z) box.max_z = p[2];
    }

    const float cx = (box.min_x + box.max_x) * 0.5f;
    const float cy = (box.min_y + box.max_y) * 0.5f;
    const float cz = (box.min_z + box.max_z) * 0.5f;
    float radius_sq = 0.0f;
    for (const AssetVertex& v : mesh.vertices) {
        const float dx = v.position[0] - cx;
        const float dy = v.position[1] - cy;
        const float dz = v.position[2] - cz;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > radius_sq) radius_sq = d2;
    }

    mesh.bounds = box;
    mesh.sphere = AssetSphere{cx, cy, cz, std::sqrt(radius_sq)};

    // The renderer keeps every submesh's bounds equal to the LOD's
    // (StaticMesh::compute_bounds does exactly this), so mirror that instead of
    // leaving boxes behind that describe pre-bake geometry.
    for (AssetSubMesh& sub : mesh.submeshes) {
        sub.bounds = mesh.bounds;
        sub.sphere = mesh.sphere;
    }
}

// --- NFMesh -------------------------------------------------------------------

// The engine's own container: save_to_bytes() *is* the format, so this writer is
// a pass-through and the round trip is lossless by construction.
MeshExportResult write_nfmesh(const MeshAsset& mesh, const ExportNames& names) {
    MeshExportResult result;
    MeshExportFile file;
    file.relative_path = names.primary;
    if (!mesh.save_to_bytes(file.bytes)) {
        result.error = "Mesh export: NFMesh serialization failed";
        return result;
    }
    result.files.push_back(std::move(file));
    result.ok = true;
    return result;
}

// --- Wavefront OBJ ------------------------------------------------------------

// OBJ indices are 1-based, and every face names the same slot in v/vt/vn
// because the writer emits exactly one of each per vertex.
std::string obj_ref(u32 index) {
    const std::string one_based = std::to_string(static_cast<u64>(index) + 1u);
    return one_based + "/" + one_based + "/" + one_based;
}

// uv1 is dropped, deliberately: OBJ has exactly one texture-coordinate channel,
// so a second one has nowhere to go — and quietly folding it into uv0 would
// rewrite the asset instead of exporting it.
std::string write_obj_text(const MeshAsset& mesh, const ExportNames& names) {
    const std::vector<Triangle> triangles = collect_triangles(mesh);

    std::string out;
    out.reserve(64 * mesh.vertices.size() + 48 * triangles.size() + 256);
    out += "# NOVAForge MeshExport\n";
    out += "# vertices " + std::to_string(mesh.vertices.size()) +
           " triangles " + std::to_string(triangles.size()) + "\n";
    out += "mtllib " + names.stem_name + ".mtl\n";
    out += "o " + names.stem_name + "\n";
    for (const AssetVertex& v : mesh.vertices) {
        out += "v " + fmt_vec3(v.position) + "\n";
        out += "vt " + fmt_vec2(v.uv0) + "\n";
        out += "vn " + fmt_vec3(v.normal) + "\n";
    }

    for (const AssetSubMesh& sub : writable_submeshes(mesh)) {
        std::vector<Triangle> sub_triangles;
        append_triangles(mesh, sub, sub_triangles);
        if (sub_triangles.empty()) continue;
        out += "usemtl slot" + std::to_string(sub.material_slot) + "\n";
        for (const Triangle& t : sub_triangles) {
            out += "f " + obj_ref(t.a) + " " + obj_ref(t.b) + " " + obj_ref(t.c) + "\n";
        }
    }
    return out;
}

// One material per *used* slot, in first-use order: two submeshes that share
// slot 3 share a material, because that is what the slot number means.
std::string write_mtl_text(const MeshAsset& mesh) {
    std::vector<u32> slots;
    for (const AssetSubMesh& sub : writable_submeshes(mesh)) {
        if (std::find(slots.begin(), slots.end(), sub.material_slot) == slots.end()) {
            slots.push_back(sub.material_slot);
        }
    }

    std::string out;
    out += "# NOVAForge MeshExport\n";
    out += "# A MeshAsset carries material slots, not materials: every entry is the\n";
    out += "# neutral default the editor shows until a real material is assigned.\n";
    for (u32 slot : slots) {
        out += "newmtl slot" + std::to_string(slot) + "\n";
        out += "Kd ";
        out += kNeutralKd;
        out += "\n";
    }
    return out;
}

MeshExportResult write_obj(const MeshAsset& mesh, const ExportNames& names) {
    MeshExportResult result;
    result.files.push_back(MeshExportFile{names.primary, string_bytes(write_obj_text(mesh, names))});
    // The sidecar is written even for a mesh with no submeshes: an OBJ that
    // references an empty .mtl loads everywhere, and a caller that asked for
    // "the OBJ files" then gets a stable list instead of a format-dependent one.
    result.files.push_back(MeshExportFile{names.stem + ".mtl", string_bytes(write_mtl_text(mesh))});
    result.ok = true;
    return result;
}

// --- binary STL ---------------------------------------------------------------

// 80-byte header, uint32 triangle count, then 50 bytes per triangle (normal,
// three vertices, uint16 attribute). STL has no index buffer and no UVs, so
// everything a triangle needs is written three times; the winding carries the
// facing and is taken from the index buffer unchanged.
std::vector<u8> write_stl_bytes(const MeshAsset& mesh) {
    const std::vector<Triangle> triangles = collect_triangles(mesh);

    std::vector<u8> out;
    out.reserve(84 + triangles.size() * 50);
    const char header[] = "NFMesh STL export";
    out.insert(out.end(), header, header + sizeof(header) - 1);
    out.resize(80, 0);  // the rest of the header is the writer's to ignore

    push_u32_le(out, static_cast<u32>(triangles.size()));
    for (const Triangle& t : triangles) {
        const float* p0 = mesh.vertices[t.a].position;
        const float* p1 = mesh.vertices[t.b].position;
        const float* p2 = mesh.vertices[t.c].position;

        // Normal from the wound triangle, (p1 - p0) x (p2 - p0): the same
        // right-handed convention the glTF importer uses to build smooth
        // normals, so a mesh exported from here and re-imported keeps facing.
        const float ux = p1[0] - p0[0], uy = p1[1] - p0[1], uz = p1[2] - p0[2];
        const float vx = p2[0] - p0[0], vy = p2[1] - p0[1], vz = p2[2] - p0[2];
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-20f) {
            nx /= len;
            ny /= len;
            nz /= len;
        } else {
            // Degenerate triangle: any unit normal is as wrong as any other,
            // but a zero vector is what makes parsers divide by zero.
            nx = 0.0f;
            ny = 0.0f;
            nz = 1.0f;
        }

        push_f32_le(out, nx);
        push_f32_le(out, ny);
        push_f32_le(out, nz);
        for (const float* p : {p0, p1, p2}) {
            push_f32_le(out, p[0]);
            push_f32_le(out, p[1]);
            push_f32_le(out, p[2]);
        }
        push_u16_le(out, 0);  // attribute byte count: 0 = no per-triangle colour
    }
    return out;
}

// --- ASCII PLY ----------------------------------------------------------------

// Header first (the counts have to be known before any data line), then one
// vertex per line in property order and one `3 i0 i1 i2` per triangle. The
// property names are the ones every reader looks for, and uv0 becomes s/t
// because that is what a PLY reader calls a texture coordinate.
std::vector<u8> write_ply_bytes(const MeshAsset& mesh) {
    const std::vector<Triangle> triangles = collect_triangles(mesh);

    std::string out;
    out.reserve(128 * mesh.vertices.size() + 32 * triangles.size() + 256);
    out += "ply\n";
    out += "format ascii 1.0\n";
    out += "element vertex " + std::to_string(mesh.vertices.size()) + "\n";
    out += "property float x\n";
    out += "property float y\n";
    out += "property float z\n";
    out += "property float nx\n";
    out += "property float ny\n";
    out += "property float nz\n";
    out += "property float s\n";
    out += "property float t\n";
    out += "element face " + std::to_string(triangles.size()) + "\n";
    out += "property list uchar int vertex_indices\n";
    out += "end_header\n";

    for (const AssetVertex& v : mesh.vertices) {
        out += fmt_vec3(v.position) + " " + fmt_vec3(v.normal) + " " + fmt_vec2(v.uv0) + "\n";
    }
    for (const Triangle& t : triangles) {
        out += "3 " + std::to_string(t.a) + " " + std::to_string(t.b) + " " + std::to_string(t.c) + "\n";
    }
    return string_bytes(out);
}

// --- glTF 2.0 -----------------------------------------------------------------
//
// The .gltf and the .glb share every byte of their payload and differ only in
// where it lives: a sibling <base>.bin for the JSON form, an embedded BIN chunk
// (and no `uri`) for the container. Building both from one function is what
// keeps the two from drifting apart.
//
// The payload is the cooked vertex array followed by the index array, verbatim:
// the accessors pick position/normal/uv0 out of each 56-byte vertex with
// byteStride, and a submesh's vertex_offset/index_offset become accessor
// byteOffsets. Tangent and uv1 stay in the buffer unreferenced — the stride has
// to cover them anyway, and copying the array is what keeps the exported bytes
// identical to the cooked ones.

constexpr usize kVertexStride = sizeof(AssetVertex);
constexpr usize kPositionOffset = offsetof(AssetVertex, position);
constexpr usize kNormalOffset = offsetof(AssetVertex, normal);
constexpr usize kUv0Offset = offsetof(AssetVertex, uv0);

// One submesh, reduced to what its four accessors need.
struct GltfPrimitive {
    u32 vertex_offset = 0;
    u32 vertex_count = 0;
    u32 index_offset = 0;
    u32 index_count = 0;
    float min[3] = {0.0f, 0.0f, 0.0f};  // POSITION min/max are required by the
    float max[3] = {0.0f, 0.0f, 0.0f};  // spec, so they are measured, not guessed
};

// A hand-edited asset can claim more vertices or indices than it stores, and an
// accessor that runs past the end of its bufferView is rejected by every glTF
// validator, so every range is clamped to what the buffers actually hold.
void clamp_to_buffers(u32 offset, u32 count, usize size, u32& out_offset, u32& out_count) {
    const usize start = static_cast<usize>(offset) < size ? static_cast<usize>(offset) : size;
    const usize available = size - start;
    const usize wanted = static_cast<usize>(count);
    out_offset = static_cast<u32>(start);
    out_count = static_cast<u32>(wanted < available ? wanted : available);
}

std::vector<GltfPrimitive> build_gltf_primitives(const MeshAsset& mesh) {
    std::vector<GltfPrimitive> primitives;
    for (const AssetSubMesh& sub : writable_submeshes(mesh)) {
        GltfPrimitive prim;
        clamp_to_buffers(sub.vertex_offset, sub.vertex_count, mesh.vertices.size(),
                         prim.vertex_offset, prim.vertex_count);
        clamp_to_buffers(sub.index_offset, sub.index_count, mesh.indices.size(),
                         prim.index_offset, prim.index_count);
        // A primitive with no vertices or no indices is not something glTF can
        // describe (an accessor with count 0 is invalid), so it is left out
        // instead of emitting a document every loader has to reject.
        if (prim.vertex_count == 0 || prim.index_count == 0) continue;

        const float* first = mesh.vertices[prim.vertex_offset].position;
        prim.min[0] = prim.max[0] = first[0];
        prim.min[1] = prim.max[1] = first[1];
        prim.min[2] = prim.max[2] = first[2];
        for (u32 i = 1; i < prim.vertex_count; ++i) {
            const float* p = mesh.vertices[prim.vertex_offset + i].position;
            for (int c = 0; c < 3; ++c) {
                if (p[c] < prim.min[c]) prim.min[c] = p[c];
                if (p[c] > prim.max[c]) prim.max[c] = p[c];
            }
        }
        primitives.push_back(prim);
    }
    return primitives;
}

std::vector<u8> build_gltf_bin(const MeshAsset& mesh) {
    const usize vertex_bytes = mesh.vertices.size() * sizeof(AssetVertex);
    const usize index_bytes = mesh.indices.size() * sizeof(u32);
    std::vector<u8> bin(vertex_bytes + index_bytes);
    if (vertex_bytes > 0) {
        std::memcpy(bin.data(), mesh.vertices.data(), vertex_bytes);
    }
    if (index_bytes > 0) {
        std::memcpy(bin.data() + vertex_bytes, mesh.indices.data(), index_bytes);
    }
    return bin;
}

// The document itself. `buffer_uri` is the sibling .bin name for a .gltf and
// empty for a .glb, where the payload is embedded and the spec forbids a uri.
std::string write_gltf_json(const std::vector<GltfPrimitive>& primitives, usize vertex_bytes,
                            usize index_bytes, const std::string& buffer_uri) {
    std::string json = "{\n";
    json += "  \"asset\": {\"version\": \"2.0\", \"generator\": \"";
    json += kGenerator;
    json += "\"}";
    if (primitives.empty()) {
        // Nothing renderable is left: a zero-length buffer and a mesh with no
        // primitives are both invalid, so the document keeps its identity block
        // and says no more.
        json += "\n}\n";
        return json;
    }
    json += ",\n";

    // A glTF document is normally read through its default scene, and a mesh no
    // node references stays invisible in Blender, three.js and every other
    // viewer — so each submesh gets a node and the scene lists them all.
    json += "  \"scene\": 0,\n";
    json += "  \"scenes\": [{\"nodes\": [";
    for (usize i = 0; i < primitives.size(); ++i) {
        if (i > 0) json += ", ";
        json += std::to_string(i);
    }
    json += "]}],\n";
    json += "  \"nodes\": [";
    for (usize i = 0; i < primitives.size(); ++i) {
        if (i > 0) json += ", ";
        json += "{\"mesh\": " + std::to_string(i) + ", \"name\": \"mesh_" + std::to_string(i) + "\"}";
    }
    json += "],\n";

    json += "  \"buffers\": [{";
    if (!buffer_uri.empty()) {
        json += "\"uri\": \"" + json_escape(buffer_uri) + "\", ";
    }
    json += "\"byteLength\": " + std::to_string(vertex_bytes + index_bytes) + "}],\n";

    // View 0 is the interleaved vertex array (stride 56, so every attribute is
    // an accessor offset into one vertex); view 1 is the index array, which has
    // no stride because it is not interleaved with anything.
    json += "  \"bufferViews\": [\n";
    json += "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": " + std::to_string(vertex_bytes) +
            ", \"byteStride\": " + std::to_string(kVertexStride) + ", \"target\": 34962}";
    if (index_bytes > 0) {
        json += ",\n    {\"buffer\": 0, \"byteOffset\": " + std::to_string(vertex_bytes) +
                ", \"byteLength\": " + std::to_string(index_bytes) + ", \"target\": 34963}";
    }
    json += "\n  ],\n";

    json += "  \"accessors\": [\n";
    for (usize i = 0; i < primitives.size(); ++i) {
        const GltfPrimitive& p = primitives[i];
        const usize vertex_base = static_cast<usize>(p.vertex_offset) * kVertexStride;
        const usize index_base = static_cast<usize>(p.index_offset) * sizeof(u32);
        const std::string count = std::to_string(p.vertex_count);

        json += "    {\"bufferView\": 0, \"byteOffset\": " + std::to_string(vertex_base + kPositionOffset) +
                ", \"componentType\": 5126, \"count\": " + count + ", \"type\": \"VEC3\", \"min\": [" +
                json_number(p.min[0]) + ", " + json_number(p.min[1]) + ", " + json_number(p.min[2]) +
                "], \"max\": [" + json_number(p.max[0]) + ", " + json_number(p.max[1]) + ", " +
                json_number(p.max[2]) + "]},\n";
        json += "    {\"bufferView\": 0, \"byteOffset\": " + std::to_string(vertex_base + kNormalOffset) +
                ", \"componentType\": 5126, \"count\": " + count + ", \"type\": \"VEC3\"},\n";
        json += "    {\"bufferView\": 0, \"byteOffset\": " + std::to_string(vertex_base + kUv0Offset) +
                ", \"componentType\": 5126, \"count\": " + count + ", \"type\": \"VEC2\"},\n";
        json += "    {\"bufferView\": 1, \"byteOffset\": " + std::to_string(index_base) +
                ", \"componentType\": 5125, \"count\": " + std::to_string(p.index_count) +
                ", \"type\": \"SCALAR\"}";
        json += (i + 1 < primitives.size()) ? ",\n" : "\n";
    }
    json += "  ],\n";

    json += "  \"meshes\": [\n";
    for (usize i = 0; i < primitives.size(); ++i) {
        const std::string base = std::to_string(i * 4);
        json += "    {\"name\": \"mesh_" + std::to_string(i) + "\", \"primitives\": [{\"attributes\": "
                "{\"POSITION\": " + base + ", \"NORMAL\": " + std::to_string(i * 4 + 1) +
                ", \"TEXCOORD_0\": " + std::to_string(i * 4 + 2) + "}, \"indices\": " +
                std::to_string(i * 4 + 3) + ", \"material\": 0, \"mode\": 4}]}";
        json += (i + 1 < primitives.size()) ? ",\n" : "\n";
    }
    json += "  ],\n";

    // One material for the whole asset: a MeshAsset records slots, not
    // materials, so every primitive points at the same neutral default.
    json += "  \"materials\": [\n";
    json += "    {\"name\": \"default\", \"pbrMetallicRoughness\": {\"baseColorFactor\": [0.8, 0.8, 0.8, 1]}}\n";
    json += "  ]\n";
    json += "}\n";
    return json;
}

} // namespace

// --- public API ---------------------------------------------------------------

const std::vector<MeshFormat>& mesh_formats() {
    static const std::vector<MeshFormat> kFormats{
        MeshFormat::NfMesh, MeshFormat::Obj, MeshFormat::Stl,
        MeshFormat::Ply, MeshFormat::Gltf, MeshFormat::Glb,
    };
    return kFormats;
}

const char* mesh_format_name(MeshFormat format) {
    switch (format) {
        case MeshFormat::NfMesh: return "NOVAForge Mesh (.nfmesh)";
        case MeshFormat::Obj: return "Wavefront OBJ (+ .mtl)";
        case MeshFormat::Stl: return "Stereolithography (binary STL)";
        case MeshFormat::Ply: return "Polygon File (ASCII PLY)";
        case MeshFormat::Gltf: return "glTF 2.0 (+ .bin)";
        case MeshFormat::Glb: return "glTF 2.0 (binary GLB)";
    }
    return "Unknown";
}

const char* mesh_format_extension(MeshFormat format) {
    switch (format) {
        case MeshFormat::NfMesh: return "nfmesh";
        case MeshFormat::Obj: return "obj";
        case MeshFormat::Stl: return "stl";
        case MeshFormat::Ply: return "ply";
        case MeshFormat::Gltf: return "gltf";
        case MeshFormat::Glb: return "glb";
    }
    return "mesh";
}

bool mesh_format_from_extension(std::string_view extension, MeshFormat& out) {
    std::string_view ext = extension;
    if (!ext.empty() && ext.front() == '.') {
        ext.remove_prefix(1);
    }
    if (ext.empty()) {
        return false;
    }
    for (const MeshFormat f : mesh_formats()) {
        if (equals_ci(ext, mesh_format_extension(f))) {
            out = f;
            return true;
        }
    }
    return false;
}

/// Unit-length in place; a zero vector is left alone (there is no direction to
/// recover from it, and dividing by its length would produce NaN).
void normalize(float v[3]) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-20f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

/// File stem of a user-facing name: no directories, no extension. A degenerate
/// name falls back to "mesh" so an export is never nameless.
std::string file_stem(const std::string& name) {
    const std::filesystem::path p(name);
    std::string stem = p.stem().string();
    if (stem.empty() || stem == "." || stem == "..") {
        stem = "mesh";
    }
    return stem;
}

// The GLB container: a 12-byte header, a JSON chunk padded with spaces and a BIN
// chunk padded with zeros, both 4-byte aligned as the spec requires. The JSON is
// the same document the .gltf form gets, minus the uri (the payload is inside).
std::vector<u8> build_glb_bytes(const MeshAsset& mesh) {
    const std::vector<GltfPrimitive> primitives = build_gltf_primitives(mesh);
    const std::vector<u8> bin = build_gltf_bin(mesh);
    const std::string json = write_gltf_json(primitives, mesh.vertices.size() * sizeof(AssetVertex),
                                             mesh.indices.size() * sizeof(u32), std::string());

    std::vector<u8> json_chunk = string_bytes(json);
    while (json_chunk.size() % 4 != 0) {
        json_chunk.push_back(0x20);  // JSON chunks pad with spaces
    }
    std::vector<u8> bin_chunk = bin;
    while (bin_chunk.size() % 4 != 0) {
        bin_chunk.push_back(0x00);  // BIN chunks pad with zeros
    }

    const u32 total = static_cast<u32>(12 + 8 + json_chunk.size() + 8 + bin_chunk.size());
    std::vector<u8> out;
    out.reserve(total);
    push_u32_le(out, 0x46546C67u);  // "glTF"
    push_u32_le(out, 2u);
    push_u32_le(out, total);
    push_u32_le(out, static_cast<u32>(json_chunk.size()));
    push_u32_le(out, 0x4E4F534Au);  // "JSON"
    out.insert(out.end(), json_chunk.begin(), json_chunk.end());
    push_u32_le(out, static_cast<u32>(bin_chunk.size()));
    push_u32_le(out, 0x004E4942u);  // "BIN\0"
    out.insert(out.end(), bin_chunk.begin(), bin_chunk.end());
    return out;
}

MeshExportResult write_gltf_pair(const MeshAsset& mesh, const ExportNames& names) {
    MeshExportResult result;
    const std::vector<GltfPrimitive> primitives = build_gltf_primitives(mesh);
    const std::vector<u8> bin = build_gltf_bin(mesh);
    // The caller may have named the file something else, so the reference inside
    // the document names the sidecar exactly as it is written.
    const std::string bin_name = names.stem_name + ".bin";
    const std::string json = write_gltf_json(primitives, mesh.vertices.size() * sizeof(AssetVertex),
                                             mesh.indices.size() * sizeof(u32), bin_name);
    result.files.push_back(MeshExportFile{names.primary, string_bytes(json)});
    result.files.push_back(MeshExportFile{names.stem + ".bin", bin});
    result.ok = true;
    return result;
}
MeshExportResult export_mesh(const MeshAsset& mesh, MeshFormat format, const std::string& base_name) {
    MeshExportResult result;
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        result.error = "Nothing to export: the mesh has no vertices or no triangles";
        return result;
    }

    ExportNames names;
    // The base name is a file name the user chose: keep its stem (not its
    // directory — references inside the files are relative to themselves) and
    // give the primary file the canonical extension for the format.
    names.stem = file_stem(base_name);
    names.primary = names.stem + "." + mesh_format_extension(format);
    names.stem_name = names.stem;

    switch (format) {
        case MeshFormat::NfMesh:
            return write_nfmesh(mesh, names);
        case MeshFormat::Obj:
            return write_obj(mesh, names);
        case MeshFormat::Stl:
            result.files.push_back(MeshExportFile{names.primary, write_stl_bytes(mesh)});
            result.ok = true;
            return result;
        case MeshFormat::Ply:
            result.files.push_back(MeshExportFile{names.primary, write_ply_bytes(mesh)});
            result.ok = true;
            return result;
        case MeshFormat::Gltf:
            return write_gltf_pair(mesh, names);
        case MeshFormat::Glb:
            result.files.push_back(MeshExportFile{names.primary, build_glb_bytes(mesh)});
            result.ok = true;
            return result;
    }
    result.error = "Unsupported mesh format";
    return result;
}

bool export_mesh_to_file(const MeshAsset& mesh, const std::string& physical_path, std::string& out_error,
                         bool has_format_override, MeshFormat format_override) {
    if (physical_path.empty()) {
        out_error = "No output path";
        return false;
    }
    const std::filesystem::path out_path(physical_path);
    MeshFormat format = format_override;
    if (!has_format_override && !mesh_format_from_extension(out_path.extension().string(), format)) {
        std::string supported;
        for (const MeshFormat f : mesh_formats()) {
            supported += std::string(".") + mesh_format_extension(f) + " ";
        }
        out_error = "Unsupported extension '" + out_path.extension().string() +
                    "' (supported: " + supported + ")";
        return false;
    }

    MeshExportResult result = export_mesh(mesh, format, out_path.filename().string());
    if (!result.ok || result.files.empty()) {
        out_error = result.error.empty() ? "Export produced no data" : result.error;
        return false;
    }
    // The caller's name wins for the primary file (its extension case, and the
    // fact that it may be a full path), while sidecars keep the names the
    // document inside them references.
    result.files[0].relative_path = out_path.filename().string();

    std::error_code ec;
    const std::filesystem::path dir = out_path.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            out_error = "Cannot create '" + dir.string() + "': " + ec.message();
            return false;
        }
    }
    for (const MeshExportFile& file : result.files) {
        const std::filesystem::path target =
            dir.empty() ? std::filesystem::path(file.relative_path) : (dir / file.relative_path);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            out_error = "Cannot write '" + target.string() + "'";
            return false;
        }
        if (!file.bytes.empty()) {
            out.write(reinterpret_cast<const char*>(file.bytes.data()),
                      static_cast<std::streamsize>(file.bytes.size()));
        }
        if (!out) {
            out_error = "Write failed for '" + target.string() + "'";
            return false;
        }
    }
    out_error.clear();
    return true;
}

} // namespace nf::assets


