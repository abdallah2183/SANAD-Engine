// Tests/AssetTests/test_mesh_import.cpp — the unified mesh reader, pinned by
// round trip.
//
// The point of this file is the property the engine did not have before
// MeshImport existed: anything MeshExport can write, MeshImport can read back.
// So every format test here is export -> import -> compare, never a fixture
// string that only the test knows how to make. A fixture would prove the parser
// agrees with the test author; a round trip proves it agrees with the engine.
//
// Hand-written fixtures appear only where a round trip cannot reach — negative
// OBJ indices, n-gons, ASCII STL, binary-little-endian PLY — and each one is
// still checked against a triangle set computed by hand.

#include <NF/Test/TestFramework.hpp>

#include <NF/Assets/MeshExport.hpp>
#include <NF/Assets/MeshImport.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Rendering/StaticMesh.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::assets;

namespace {

// --- scratch filesystem -----------------------------------------------------

struct Scratch {
    std::filesystem::path root;

    explicit Scratch(const std::string& name) {
        std::error_code ec;
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    std::filesystem::path file(const std::string& name) const { return root / name; }
};

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

std::vector<uint8_t> as_bytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

// --- geometry comparison ----------------------------------------------------

// A canonical, order-independent description of a mesh's triangles: every
// triangle as its three positions in winding order, formatted to a fixed number
// of decimals so two meshes compare by geometry rather than by float luck.
std::set<std::string> triangle_keys(const MeshAsset& mesh) {
    std::set<std::string> keys;
    for (usize i = 0; i + 3 <= mesh.indices.size(); i += 3) {
        std::string key;
        for (int c = 0; c < 3; ++c) {
            const u32 index = mesh.indices[i + static_cast<usize>(c)];
            if (index >= mesh.vertices.size()) return {};
            char buf[64] = {};
            std::snprintf(buf, sizeof(buf), "%.5f,%.5f,%.5f;", mesh.vertices[index].position[0],
                          mesh.vertices[index].position[1], mesh.vertices[index].position[2]);
            key += buf;
        }
        keys.insert(key);
    }
    return keys;
}

usize triangle_count(const MeshAsset& mesh) { return mesh.indices.size() / 3; }

void check_bounds_match(const MeshAsset& a, const MeshAsset& b) {
    NF_CHECK_NEAR(a.bounds.min_x, b.bounds.min_x, 1e-5f);
    NF_CHECK_NEAR(a.bounds.min_y, b.bounds.min_y, 1e-5f);
    NF_CHECK_NEAR(a.bounds.min_z, b.bounds.min_z, 1e-5f);
    NF_CHECK_NEAR(a.bounds.max_x, b.bounds.max_x, 1e-5f);
    NF_CHECK_NEAR(a.bounds.max_y, b.bounds.max_y, 1e-5f);
    NF_CHECK_NEAR(a.bounds.max_z, b.bounds.max_z, 1e-5f);
}

/// The engine's own cube: 24 vertices (4 per face, so every face keeps its own
/// normal), 12 triangles, one implicit material slot.
std::unique_ptr<MeshAsset> cube_asset() {
    auto cube = rendering::StaticMesh::create_cube(1.0f);
    return rendering::make_mesh_asset(*cube, AssetId::generate(), "content://Meshes/Cube.nfmesh");
}

/// Exports `mesh` to `<scratch>/<stem>.<ext>` and imports the file back.
MeshImportResult round_trip(const MeshAsset& mesh, MeshFormat format, const Scratch& scratch,
                            const std::string& stem) {
    const std::filesystem::path path =
        scratch.file(stem + "." + std::string(mesh_format_extension(format)));
    std::string err;
    if (!export_mesh_to_file(mesh, path.string(), err)) {
        throw std::runtime_error("export failed: " + err);
    }
    return import_mesh_file(path.string());
}

} // namespace

// ---------------------------------------------------------------------------
// 1. the format table mirrors the writer's, so the two menus cannot drift
// ---------------------------------------------------------------------------

NF_TEST(mesh_import_format_table_mirrors_export) {
    NF_CHECK(mesh_import_formats().size() == mesh_formats().size());

    // Same containers, same order, same extensions: a format the engine writes
    // is a format it reads. That equality is the whole point of this module.
    for (usize i = 0; i < mesh_formats().size(); ++i) {
        const MeshFormat write = mesh_formats()[i];
        const MeshImportFormat read = mesh_import_formats()[i];
        NF_CHECK(std::string(mesh_format_extension(write)) ==
                 std::string(mesh_import_format_extension(read)));
        MeshImportFormat parsed = MeshImportFormat::Unknown;
        NF_CHECK(mesh_import_format_from_extension(mesh_format_extension(write), parsed));
        NF_CHECK(parsed == read);
    }

    // Extensions parse with or without the dot, in any case.
    MeshImportFormat parsed = MeshImportFormat::Unknown;
    NF_CHECK(mesh_import_format_from_extension(".GLB", parsed) && parsed == MeshImportFormat::Glb);
    NF_CHECK(mesh_import_format_from_extension("Obj", parsed) && parsed == MeshImportFormat::Obj);
    NF_CHECK(!mesh_import_format_from_extension(".fbx", parsed));
    NF_CHECK(!mesh_import_format_from_extension("", parsed));

    // glTF and GLB are one family: the glTF reader owns their mismatch check.
    NF_CHECK(mesh_import_format_family(MeshImportFormat::Glb) == MeshImportFormat::Gltf);
    NF_CHECK(mesh_import_format_family(MeshImportFormat::Obj) == MeshImportFormat::Obj);
}

// ---------------------------------------------------------------------------
// 2. content sniffing, for files whose extension is missing or wrong
// ---------------------------------------------------------------------------

NF_TEST(mesh_import_sniffs_signatures) {
    // .nfmesh magic, exactly as MeshAsset writes it.
    auto nfmesh = cube_asset();
    std::vector<uint8_t> nfmesh_bytes;
    NF_CHECK(nfmesh->save_to_bytes(nfmesh_bytes));
    NF_CHECK(sniff_mesh_format(nfmesh_bytes) == MeshImportFormat::NfMesh);

    NF_CHECK(sniff_mesh_format(as_bytes("glTF\x02\x00\x00\x00")) == MeshImportFormat::Glb);
    NF_CHECK(sniff_mesh_format(as_bytes("{\"asset\":{\"version\":\"2.0\"}}")) ==
             MeshImportFormat::Gltf);
    NF_CHECK(sniff_mesh_format(as_bytes("ply\nformat ascii 1.0\n")) == MeshImportFormat::Ply);
    NF_CHECK(sniff_mesh_format(as_bytes("solid cube\nfacet normal 0 0 1\n")) ==
             MeshImportFormat::Stl);

    // A binary STL whose 80-byte header happens to begin with "solid" is still
    // binary: the size test has to win over the word.
    std::vector<uint8_t> binary_stl(84 + 50, 0);
    std::memcpy(binary_stl.data(), "solid", 5);
    binary_stl[80] = 1; // one triangle
    NF_CHECK(sniff_mesh_format(binary_stl) == MeshImportFormat::Stl);

    // OBJ has no signature at all, by design of the format. Unknown is honest.
    NF_CHECK(sniff_mesh_format(as_bytes("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")) ==
             MeshImportFormat::Unknown);
    NF_CHECK(sniff_mesh_format(as_bytes("")) == MeshImportFormat::Unknown);
}

// ---------------------------------------------------------------------------
// 3-7. round trips: everything MeshExport writes, MeshImport reads back
// ---------------------------------------------------------------------------

NF_TEST(mesh_import_nfmesh_round_trip) {
    Scratch scratch("nf_import_nfmesh");
    auto source = cube_asset();
    const MeshImportResult r = round_trip(*source, MeshFormat::NfMesh, scratch, "Cube");

    NF_CHECK(r.ok);
    NF_CHECK(r.format == MeshImportFormat::NfMesh);
    NF_CHECK(r.meshes.size() == 1u);
    // Lossless by construction: .nfmesh *is* the MeshAsset.
    NF_CHECK(r.meshes[0]->vertices.size() == source->vertices.size());
    NF_CHECK(r.meshes[0]->indices == source->indices);
    check_bounds_match(*r.meshes[0], *source);
}

NF_TEST(mesh_import_obj_round_trip) {
    Scratch scratch("nf_import_obj");
    auto source = cube_asset();
    const MeshImportResult r = round_trip(*source, MeshFormat::Obj, scratch, "Cube");

    NF_CHECK(r.ok);
    NF_CHECK(r.format == MeshImportFormat::Obj);
    NF_CHECK(r.meshes.size() == 1u);
    // OBJ identifies a vertex by its (v, vt, vn) triple, so deduping on the
    // triple recovers the source vertex set exactly — not 36 expanded corners.
    NF_CHECK(r.meshes[0]->vertices.size() == source->vertices.size());
    NF_CHECK(triangle_count(*r.meshes[0]) == triangle_count(*source));
    NF_CHECK(triangle_keys(*r.meshes[0]) == triangle_keys(*source));
    check_bounds_match(*r.meshes[0], *source);

    // The .mtl sidecar is written; the importer keeps the slot number the
    // writer named ("slot0") instead of renumbering it.
    NF_CHECK(std::filesystem::exists(scratch.file("Cube.mtl")));
    NF_CHECK(r.meshes[0]->submeshes.size() == 1u);
    NF_CHECK(r.meshes[0]->submeshes[0].material_slot == 0u);
    NF_CHECK(r.meshes[0]->submeshes[0].index_count == source->indices.size());
    // Normals came from the file, so nothing was recomputed and nothing warned.
    NF_CHECK(r.warnings.empty());
}

NF_TEST(mesh_import_stl_round_trip) {
    Scratch scratch("nf_import_stl");
    auto source = cube_asset();
    const MeshImportResult r = round_trip(*source, MeshFormat::Stl, scratch, "Cube");

    NF_CHECK(r.ok);
    NF_CHECK(r.format == MeshImportFormat::Stl);
    NF_CHECK(r.meshes.size() == 1u);
    // STL carries no indices, so the writer expanded 24 vertices into 36. The
    // reader re-shares them where position AND normal agree, which recovers the
    // flat-shaded cube exactly — without inventing a smooth normal.
    NF_CHECK(r.meshes[0]->vertices.size() == source->vertices.size());
    NF_CHECK(triangle_count(*r.meshes[0]) == triangle_count(*source));
    NF_CHECK(triangle_keys(*r.meshes[0]) == triangle_keys(*source));
    check_bounds_match(*r.meshes[0], *source);

    // The format's losses are named, never silent.
    NF_CHECK(r.warnings.size() == 1u);
    NF_CHECK(r.warnings[0].find("no UVs") != std::string::npos);
}

NF_TEST(mesh_import_ply_round_trip) {
    Scratch scratch("nf_import_ply");
    auto source = cube_asset();
    const MeshImportResult r = round_trip(*source, MeshFormat::Ply, scratch, "Cube");

    NF_CHECK(r.ok);
    NF_CHECK(r.format == MeshImportFormat::Ply);
    NF_CHECK(r.meshes.size() == 1u);
    // PLY has a real vertex array and face indices, so this is exact: counts,
    // order, UVs and normals all come back as written.
    NF_CHECK(r.meshes[0]->vertices.size() == source->vertices.size());
    NF_CHECK(r.meshes[0]->indices == source->indices);
    NF_CHECK(triangle_keys(*r.meshes[0]) == triangle_keys(*source));
    check_bounds_match(*r.meshes[0], *source);
    for (usize i = 0; i < source->vertices.size(); ++i) {
        NF_CHECK_NEAR(r.meshes[0]->vertices[i].uv0[0], source->vertices[i].uv0[0], 1e-5f);
        NF_CHECK_NEAR(r.meshes[0]->vertices[i].uv0[1], source->vertices[i].uv0[1], 1e-5f);
        NF_CHECK_NEAR(r.meshes[0]->vertices[i].normal[0], source->vertices[i].normal[0], 1e-5f);
        NF_CHECK_NEAR(r.meshes[0]->vertices[i].normal[1], source->vertices[i].normal[1], 1e-5f);
        NF_CHECK_NEAR(r.meshes[0]->vertices[i].normal[2], source->vertices[i].normal[2], 1e-5f);
    }
    NF_CHECK(r.warnings.empty());
}

NF_TEST(mesh_import_glb_round_trip) {
    Scratch scratch("nf_import_glb");
    auto source = cube_asset();
    const MeshImportResult r = round_trip(*source, MeshFormat::Glb, scratch, "Cube");

    NF_CHECK(r.ok);
    NF_CHECK(r.format == MeshImportFormat::Glb);
    NF_CHECK(r.meshes.size() == 1u);
    NF_CHECK(r.meshes[0]->vertices.size() == source->vertices.size());
    NF_CHECK(triangle_count(*r.meshes[0]) == triangle_count(*source));
    NF_CHECK(triangle_keys(*r.meshes[0]) == triangle_keys(*source));
    // The glTF writer emits one node per submesh, so the reader sees one node
    // and reports the mesh as static — the explicit fact, not a default rig.
    NF_CHECK(r.nodes.size() == 1u);
    NF_CHECK(r.skins.empty());
    NF_CHECK(r.mesh_skins.size() == 1u);
    NF_CHECK(r.mesh_skins[0].skin_index == -1);
}

// ---------------------------------------------------------------------------
// 8. OBJ text features a round trip cannot reach
// ---------------------------------------------------------------------------

NF_TEST(mesh_import_obj_negative_indices_ngons_and_slots) {
    Scratch scratch("nf_import_obj_features");
    // One quad written with negative (relative) indices and one triangle, split
    // across two material runs with names the writer never produces.
    const std::string obj =
        "# hand-written\n"
        "mtllib ignored.mtl\n"
        "o Panel\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n"
        "vn 0 0 1\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 1 1\n"
        "vt 0 1\n"
        "usemtl wood\n"
        "f -4/-4/1 -3/-3/1 -2/-2/1 -1/-1/1\n"   // n-gon, relative indices
        "usemtl metal\n"
        "f 1/1/1 2/2/1 3/3/1\n"
        "s off\n";
    const auto path = scratch.file("Panel.obj");
    write_text(path, obj);

    const MeshImportResult r = import_mesh_file(path.string());
    NF_CHECK(r.ok);
    NF_CHECK(r.meshes.size() == 1u);
    NF_CHECK(r.meshes[0]->vertices.size() == 4u);
    // A 4-gon fans into 2 triangles, plus the explicit triangle = 3.
    NF_CHECK(triangle_count(*r.meshes[0]) == 3u);
    // Two usemtl runs -> two submeshes, auto-numbered in first-use order.
    NF_CHECK(r.meshes[0]->submeshes.size() == 2u);
    NF_CHECK(r.meshes[0]->submeshes[0].material_slot == 0u);
    NF_CHECK(r.meshes[0]->submeshes[0].index_count == 6u);
    NF_CHECK(r.meshes[0]->submeshes[1].material_slot == 1u);
    NF_CHECK(r.meshes[0]->submeshes[1].index_count == 3u);
    NF_CHECK(r.faces_skipped == 0u);
    NF_CHECK(r.mesh_names.size() == 1u && r.mesh_names[0] == "Panel");
}

NF_TEST(mesh_import_obj_groups_become_separate_meshes) {
    Scratch scratch("nf_import_obj_groups");
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "g First\n"
        "f 1 2 3\n"
        "g Second\n"
        "f 3 2 1\n";
    const auto path = scratch.file("Two.obj");
    write_text(path, obj);

    const MeshImportResult r = import_mesh_file(path.string());
    NF_CHECK(r.ok);
    // Two groups are two meshes — the same shape a multi-mesh glTF produces.
    NF_CHECK(r.meshes.size() == 2u);
    NF_CHECK(r.mesh_names.size() == 2u);
    NF_CHECK(r.mesh_names[0] == "First");
    NF_CHECK(r.mesh_names[1] == "Second");
    NF_CHECK(triangle_count(*r.meshes[0]) == 1u);
    NF_CHECK(triangle_count(*r.meshes[1]) == 1u);
}

NF_TEST(mesh_import_obj_missing_normals_are_computed_and_reported) {
    Scratch scratch("nf_import_obj_normals");
    const auto path = scratch.file("Flat.obj");
    write_text(path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    const MeshImportResult r = import_mesh_file(path.string());
    NF_CHECK(r.ok);
    NF_CHECK(r.meshes.size() == 1u);
    NF_CHECK(r.warnings.size() == 1u);
    NF_CHECK(r.warnings[0].find("no vertex normals") != std::string::npos);
    // A triangle in the XY plane, wound counterclockwise: the computed normal
    // is +Z. This is what "smooth normals were computed" has to mean.
    const AssetVertex& v = r.meshes[0]->vertices[0];
    NF_CHECK_NEAR(v.normal[0], 0.0f, 1e-5f);
    NF_CHECK_NEAR(v.normal[1], 0.0f, 1e-5f);
    NF_CHECK_NEAR(v.normal[2], 1.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// 9. ASCII STL
// ---------------------------------------------------------------------------

NF_TEST(mesh_import_stl_ascii) {
    Scratch scratch("nf_import_stl_ascii");
    const std::string stl =
        "solid one_triangle\n"
        "  facet normal 0 0 1\n"
        "    outer loop\n"
        "      vertex 0 0 0\n"
        "      vertex 1 0 0\n"
        "      vertex 0 1 0\n"
        "    endloop\n"
        "  endfacet\n"
        "endsolid one_triangle\n";
    const auto path = scratch.file("Tri.stl");
    write_text(path, stl);

    const MeshImportResult r = import_mesh_file(path.string());
    NF_CHECK(r.ok);
    NF_CHECK(r.format == MeshImportFormat::Stl);
    NF_CHECK(r.meshes.size() == 1u);
    NF_CHECK(triangle_count(*r.meshes[0]) == 1u);
    NF_CHECK(r.meshes[0]->vertices.size() == 3u);
    NF_CHECK_NEAR(r.meshes[0]->vertices[0].normal[2], 1.0f, 1e-5f);
    NF_CHECK_NEAR(r.meshes[0]->bounds.max_x, 1.0f, 1e-5f);
    NF_CHECK_NEAR(r.meshes[0]->bounds.max_y, 1.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// 10. PLY: ASCII, and binary little endian
// ---------------------------------------------------------------------------

NF_TEST(mesh_import_ply_ascii_with_extra_property) {
    Scratch scratch("nf_import_ply_ascii");
    // A per-vertex colour property sits between the UVs and the faces: an
    // unknown property must be consumed, not skipped by name, or the face rows
    // land on the wrong bytes.
    const std::string ply =
        "ply\n"
        "format ascii 1.0\n"
        "comment made by hand\n"
        "element vertex 3\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property float nx\n"
        "property float ny\n"
        "property float nz\n"
        "property uchar red\n"
        "property uchar green\n"
        "property uchar blue\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n"
        "0 0 0 0 0 1 255 0 0\n"
        "1 0 0 0 0 1 0 255 0\n"
        "0 1 0 0 0 1 0 0 255\n"
        "3 0 1 2\n";
    const auto path = scratch.file("Tri.ply");
    write_text(path, ply);

    const MeshImportResult r = import_mesh_file(path.string());
    NF_CHECK(r.ok);
    NF_CHECK(r.format == MeshImportFormat::Ply);
    NF_CHECK(r.meshes[0]->vertices.size() == 3u);
    NF_CHECK(r.meshes[0]->indices.size() == 3u);
    NF_CHECK(r.meshes[0]->indices[0] == 0u);
    NF_CHECK(r.meshes[0]->indices[1] == 1u);
    NF_CHECK(r.meshes[0]->indices[2] == 2u);
    NF_CHECK(r.warnings.empty()); // normals were in the file
}

NF_TEST(mesh_import_ply_binary_little_endian) {
    Scratch scratch("nf_import_ply_binary");
    std::vector<uint8_t> bytes = as_bytes(
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n");

    auto push_f32 = [&bytes](float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFFu));
        }
    };
    auto push_i32 = [&bytes](int32_t v) {
        const uint32_t bits = static_cast<uint32_t>(v);
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFFu));
        }
    };
    push_f32(0.0f); push_f32(0.0f); push_f32(0.0f);
    push_f32(2.0f); push_f32(0.0f); push_f32(0.0f);
    push_f32(0.0f); push_f32(2.0f); push_f32(0.0f);
    bytes.push_back(3); // list count
    push_i32(0); push_i32(1); push_i32(2);

    const auto path = scratch.file("Tri.ply");
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    const MeshImportResult r = import_mesh_file(path.string());
    NF_CHECK(r.ok);
    NF_CHECK(r.meshes[0]->vertices.size() == 3u);
    NF_CHECK(triangle_count(*r.meshes[0]) == 1u);
    NF_CHECK_NEAR(r.meshes[0]->bounds.max_x, 2.0f, 1e-5f);
    NF_CHECK_NEAR(r.meshes[0]->bounds.max_y, 2.0f, 1e-5f);
    // No normals in the file, so they were computed — and said so.
    NF_CHECK(r.warnings.size() == 1u);
    NF_CHECK(r.warnings[0].find("no vertex normals") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 11. the memory entry point works without a path or an extension
// ---------------------------------------------------------------------------

NF_TEST(mesh_import_memory_needs_no_extension) {
    Scratch scratch("nf_import_memory");
    auto source = cube_asset();
    std::string err;
    NF_CHECK(export_mesh_to_file(*source, scratch.file("Cube.stl").string(), err));

    const std::vector<uint8_t> bytes = read_bytes(scratch.file("Cube.stl"));
    NF_CHECK(!bytes.empty());

    // "<memory>" has no extension: the signature alone has to identify it.
    const MeshImportResult sniffed = import_mesh_memory(bytes, "<memory>");
    NF_CHECK(sniffed.ok);
    NF_CHECK(sniffed.format == MeshImportFormat::Stl);
    NF_CHECK(sniffed.meshes.size() == 1u);

    // The same bytes, claimed explicitly, take the same reader.
    const MeshImportResult explicit_format =
        import_mesh_as(bytes, MeshImportFormat::Stl, "whatever");
    NF_CHECK(explicit_format.ok);
    NF_CHECK(explicit_format.format == MeshImportFormat::Stl);
}

// ---------------------------------------------------------------------------
// 12. bad input fails with a reason, and never half-imports
// ---------------------------------------------------------------------------

NF_TEST(mesh_import_rejects_bad_input) {
    // Empty.
    MeshImportResult empty = import_mesh_memory({}, "empty.obj");
    NF_CHECK(!empty.ok);
    NF_CHECK(!empty.error.empty());
    NF_CHECK(empty.meshes.empty());

    // Truncated .nfmesh: a valid magic and nothing else.
    std::vector<uint8_t> tiny = {0x45, 0x4D, 0x46, 0x4E, 1, 0, 0, 0};
    MeshImportResult truncated = import_mesh_memory(tiny, "tiny.nfmesh");
    NF_CHECK(!truncated.ok);
    NF_CHECK(truncated.meshes.empty());

    // OBJ with vertices but no faces is not a mesh.
    Scratch scratch("nf_import_bad");
    const auto no_faces = scratch.file("NoFaces.obj");
    write_text(no_faces, "v 0 0 0\nv 1 0 0\nv 0 1 0\n");
    MeshImportResult obj = import_mesh_file(no_faces.string());
    NF_CHECK(!obj.ok);
    NF_CHECK(obj.error.find("no usable faces") != std::string::npos);

    // A PLY point cloud has no triangles, and says so rather than inventing them.
    const auto cloud = scratch.file("Cloud.ply");
    write_text(cloud,
               "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\n"
               "property float z\nend_header\n0 0 0\n");
    MeshImportResult ply = import_mesh_file(cloud.string());
    NF_CHECK(!ply.ok);
    NF_CHECK(ply.error.find("point cloud") != std::string::npos);

    // A file that is neither shape of STL.
    const auto not_stl = scratch.file("Broken.stl");
    write_text(not_stl, "this is not an stl at all, not even close");
    MeshImportResult stl = import_mesh_file(not_stl.string());
    NF_CHECK(!stl.ok);
    NF_CHECK(stl.error.find("not an STL") != std::string::npos);

    // Unidentifiable bytes with no usable extension.
    const auto blob = scratch.file("thing.bin");
    write_text(blob, "\x01\x02\x03\x04\x05\x06\x07\x08");
    MeshImportResult unknown = import_mesh_file(blob.string());
    NF_CHECK(!unknown.ok);
    NF_CHECK(unknown.error.find("cannot determine the mesh format") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 13. a lying extension is refused, not guessed at
// ---------------------------------------------------------------------------

NF_TEST(mesh_import_rejects_extension_content_mismatch) {
    Scratch scratch("nf_import_mismatch");
    auto source = cube_asset();
    std::string err;
    const auto real = scratch.file("Cube.stl");
    NF_CHECK(export_mesh_to_file(*source, real.string(), err));

    // The same STL bytes under a .ply name. Guessing by content would import
    // something the caller did not ask for, so this fails loudly instead.
    const auto misnamed = scratch.file("Cube.ply");
    std::filesystem::copy_file(real, misnamed);
    const MeshImportResult r = import_mesh_file(misnamed.string());
    NF_CHECK(!r.ok);
    NF_CHECK(r.error.find("extension/content mismatch") != std::string::npos);
    NF_CHECK(r.meshes.empty());
}

// ---------------------------------------------------------------------------
// 14. materials and textures: geometry alone is not an import you can use
// ---------------------------------------------------------------------------

namespace {

/// A few bytes that start like a PNG. The importer never decodes an image — it
/// carries the encoded bytes through — so a signature is all a test needs, and
/// using one keeps this file free of a binary fixture.
std::vector<uint8_t> fake_png_bytes() {
    return std::vector<uint8_t>{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'};
}

std::string base64_encode(const std::vector<uint8_t>& bytes) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    usize i = 0;
    while (i + 2 < bytes.size()) {
        const u32 v = (static_cast<u32>(bytes[i]) << 16) | (static_cast<u32>(bytes[i + 1]) << 8) |
                      static_cast<u32>(bytes[i + 2]);
        out.push_back(kTable[(v >> 18) & 63u]);
        out.push_back(kTable[(v >> 12) & 63u]);
        out.push_back(kTable[(v >> 6) & 63u]);
        out.push_back(kTable[v & 63u]);
        i += 3;
    }
    const usize rest = bytes.size() - i;
    if (rest == 1) {
        const u32 v = static_cast<u32>(bytes[i]) << 16;
        out.push_back(kTable[(v >> 18) & 63u]);
        out.push_back(kTable[(v >> 12) & 63u]);
        out += "==";
    } else if (rest == 2) {
        const u32 v = (static_cast<u32>(bytes[i]) << 16) | (static_cast<u32>(bytes[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 63u]);
        out.push_back(kTable[(v >> 12) & 63u]);
        out.push_back(kTable[(v >> 6) & 63u]);
        out.push_back('=');
    }
    return out;
}

void push_f32_le(std::vector<uint8_t>& out, float v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFFu));
    }
}

} // namespace

NF_TEST(mesh_image_extension_comes_from_the_bytes) {
    NF_CHECK(std::string(image_extension_for_bytes(fake_png_bytes())) == ".png");

    const std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
    NF_CHECK(std::string(image_extension_for_bytes(jpeg)) == ".jpg");

    const std::vector<uint8_t> bmp = {'B', 'M', 0x36, 0x00};
    NF_CHECK(std::string(image_extension_for_bytes(bmp)) == ".bmp");

    // TGA has no magic number, so the honest answer is "no idea" — the callers
    // fall back to the source's own extension rather than inventing one.
    const std::vector<uint8_t> tga = {0x00, 0x00, 0x02, 0x00};
    NF_CHECK(std::string(image_extension_for_bytes(tga)).empty());
    NF_CHECK(std::string(image_extension_for_bytes({})).empty());
}

NF_TEST(mesh_import_obj_resolves_its_mtl) {
    Scratch scratch("nf_import_obj_mtl");
    // An OBJ on its own carries NO material values — only `usemtl` names. If the
    // reader ignores the .mtl, every colour the artist authored is gone.
    write_text(scratch.file("Panel.obj"),
               "# hand-written\n"
               "mtllib Panel.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "vn 0 0 1\n"
               "usemtl wood\n"
               "f 1/1/1 2/2/1 3/3/1\n");
    write_text(scratch.file("Panel.mtl"),
               "# hand-written\n"
               "newmtl wood\n"
               "Ka 0.1 0.1 0.1\n"
               "Kd 0.55 0.35 0.15\n"
               "Ks 1 1 1\n"
               "Ns 32\n"
               "d 0.75\n"
               "illum 2\n"
               "map_Kd -s 1 1 1 wood.png\n");
    const std::vector<uint8_t> png = fake_png_bytes();
    {
        std::ofstream out(scratch.file("wood.png"), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(png.data()),
                  static_cast<std::streamsize>(png.size()));
    }

    const MeshImportResult r = import_mesh_file(scratch.file("Panel.obj").string());
    NF_CHECK(r.ok);
    NF_CHECK(r.meshes.size() == 1u);
    NF_CHECK(r.materials.size() == 1u);

    const GltfMaterialInfo& m = r.materials[0];
    NF_CHECK(m.name == "wood");
    NF_CHECK_NEAR(m.base_color[0], 0.55f, 1e-5f);
    NF_CHECK_NEAR(m.base_color[1], 0.35f, 1e-5f);
    NF_CHECK_NEAR(m.base_color[2], 0.15f, 1e-5f);
    NF_CHECK_NEAR(m.base_color[3], 0.75f, 1e-5f); // from `d`

    // The base-colour map came through, with an extension sniffed from the
    // bytes rather than trusted from the filename.
    NF_CHECK(r.images.size() == 1u);
    NF_CHECK(r.images[0].extension == ".png");
    NF_CHECK(r.images[0].bytes == png);
    NF_CHECK(m.albedo_image == 0);

    // The slot indexes a real material now, not an arbitrary number.
    NF_CHECK(r.meshes[0]->submeshes[0].material_slot == 0u);

    // And what the pipeline cannot store is NAMED, never approximated: turning
    // Ns into a roughness or Ks into metallic would invent a material.
    NF_CHECK(m.dropped.size() == 4u);
    std::string joined;
    for (const std::string& entry : m.dropped) joined += entry + "|";
    NF_CHECK(joined.find("Ka ") != std::string::npos);
    NF_CHECK(joined.find("Ks ") != std::string::npos);
    NF_CHECK(joined.find("Ns ") != std::string::npos);
    NF_CHECK(joined.find("illum ") != std::string::npos);
}

NF_TEST(mesh_import_obj_round_trip_keeps_material_slots) {
    Scratch scratch("nf_import_obj_mtl_roundtrip");
    auto source = cube_asset();
    // MeshExport writes `usemtl slotN` plus an .mtl defining each one, so a
    // round trip has to come back with the same number of materials and the
    // same slot numbers.
    const MeshImportResult r = round_trip(*source, MeshFormat::Obj, scratch, "Cube");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1u);
    NF_CHECK(r.materials[0].name == "slot0");
    // The neutral Kd the writer emits, read back from the sidecar.
    NF_CHECK_NEAR(r.materials[0].base_color[0], 0.8f, 1e-5f);
    NF_CHECK(r.meshes[0]->submeshes[0].material_slot == 0u);
    // No texture: the exporter writes factors only, so nothing to extract.
    NF_CHECK(r.images.empty());
    NF_CHECK(r.images_skipped == 0u);
}

NF_TEST(mesh_import_obj_survives_a_missing_mtl) {
    Scratch scratch("nf_import_obj_no_mtl");
    // A referenced .mtl that is not there is a warning and a default material,
    // not a failed import: the geometry is still perfectly good.
    write_text(scratch.file("Lone.obj"),
               "mtllib Nowhere.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "usemtl paint\n"
               "f 1 2 3\n");
    const MeshImportResult r = import_mesh_file(scratch.file("Lone.obj").string());
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1u);
    NF_CHECK(r.materials[0].name == "paint");
    // Two warnings, both earned: the missing .mtl, and the missing normals this
    // file also has (it declares no `vn`). Neither one fails the import.
    NF_CHECK(r.warnings.size() == 2u);
    std::string joined;
    for (const std::string& warning : r.warnings) joined += warning + "|";
    NF_CHECK(joined.find("Nowhere.mtl") != std::string::npos);
    NF_CHECK(joined.find("no vertex normals") != std::string::npos);
}

NF_TEST(mesh_import_gltf_carries_an_embedded_texture) {
    Scratch scratch("nf_import_gltf_texture");
    const std::vector<uint8_t> png = fake_png_bytes();

    // Buffer: 3 positions (36 bytes) followed by the image bytes, base64-embedded
    // so the document is self-contained — the shape a "single-file" .gltf has.
    std::vector<uint8_t> buffer;
    push_f32_le(buffer, 0.0f); push_f32_le(buffer, 0.0f); push_f32_le(buffer, 0.0f);
    push_f32_le(buffer, 1.0f); push_f32_le(buffer, 0.0f); push_f32_le(buffer, 0.0f);
    push_f32_le(buffer, 0.0f); push_f32_le(buffer, 1.0f); push_f32_le(buffer, 0.0f);
    const usize image_offset = buffer.size();
    buffer.insert(buffer.end(), png.begin(), png.end());

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) +
        ",\"uri\":\"data:application/octet-stream;base64," + base64_encode(buffer) + "\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(image_offset) +
        ",\"byteLength\":" + std::to_string(png.size()) + "}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"images\":[{\"name\":\"HeroBase\",\"bufferView\":1,\"mimeType\":\"image/png\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"name\":\"HeroMat\",\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.5,0.25,0.125,1.0],\"metallicFactor\":0.2,"
        "\"roughnessFactor\":0.6,\"baseColorTexture\":{\"index\":0}},"
        "\"normalTexture\":{\"index\":0}}],"
        "\"meshes\":[{\"name\":\"Hero\",\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"material\":0,\"mode\":4}]}],"
        "\"nodes\":[{\"name\":\"Hero\",\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";

    const MeshImportResult r = import_mesh_memory(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(json.data()), json.size()),
        "Hero.gltf");
    NF_CHECK(r.ok);
    NF_CHECK(r.meshes.size() == 1u);
    NF_CHECK(r.materials.size() == 1u);

    // The image came out byte-for-byte, with the extension sniffed from it.
    NF_CHECK(r.images.size() == 1u);
    NF_CHECK(r.images[0].name == "HeroBase");
    NF_CHECK(r.images[0].extension == ".png");
    NF_CHECK(r.images[0].bytes == png);
    NF_CHECK(r.images_skipped == 0u);

    // The material points at it, and its factors survived.
    const GltfMaterialInfo& m = r.materials[0];
    NF_CHECK(m.name == "HeroMat");
    NF_CHECK(m.albedo_image == 0);
    NF_CHECK_NEAR(m.base_color[0], 0.5f, 1e-5f);
    NF_CHECK_NEAR(m.base_color[1], 0.25f, 1e-5f);
    NF_CHECK_NEAR(m.base_color[2], 0.125f, 1e-5f);
    NF_CHECK_NEAR(m.metallic, 0.2f, 1e-5f);
    NF_CHECK_NEAR(m.roughness, 0.6f, 1e-5f);

    // The normal map it also declares has nowhere to go, and says so.
    NF_CHECK(m.dropped.size() == 1u);
    NF_CHECK(m.dropped[0].find("normal map") != std::string::npos);
}

NF_TEST(mesh_import_gltf_carries_a_data_uri_texture) {
    Scratch scratch("nf_import_gltf_data_uri");
    const std::vector<uint8_t> png = fake_png_bytes();

    std::vector<uint8_t> buffer;
    push_f32_le(buffer, 0.0f); push_f32_le(buffer, 0.0f); push_f32_le(buffer, 0.0f);
    push_f32_le(buffer, 1.0f); push_f32_le(buffer, 0.0f); push_f32_le(buffer, 0.0f);
    push_f32_le(buffer, 0.0f); push_f32_le(buffer, 1.0f); push_f32_le(buffer, 0.0f);

    // The image is inline as a data URI rather than a bufferView: the other way
    // a "self-contained" glTF stores one.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) +
        ",\"uri\":\"data:application/octet-stream;base64," + base64_encode(buffer) + "\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"images\":[{\"name\":\"Inline\",\"uri\":\"data:image/png;base64," +
        base64_encode(png) + "\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"name\":\"InlineMat\",\"pbrMetallicRoughness\":{"
        "\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"name\":\"Tri\",\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"material\":0,\"mode\":4}]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";

    const MeshImportResult r = import_mesh_memory(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(json.data()), json.size()),
        "Inline.gltf");
    NF_CHECK(r.ok);
    NF_CHECK(r.images.size() == 1u);
    NF_CHECK(r.images[0].name == "Inline");
    NF_CHECK(r.images[0].extension == ".png");
    NF_CHECK(r.images[0].bytes == png);
    NF_CHECK(r.materials[0].albedo_image == 0);
}

NF_TEST(mesh_import_obj_v_slash_slash_vn_and_face_rollback) {
    Scratch scratch("nf_import_obj_rollback");

    // `v//vn` — no texture coordinate at all. Every normal reference has to
    // exist: `2//2` with a single `vn` is a broken file, not a broken reader.
    write_text(scratch.file("Flat.obj"),
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vn 0 0 1\n"
               "f 1//1 2//1 3//1\n");
    const MeshImportResult ok = import_mesh_file(scratch.file("Flat.obj").string());
    NF_CHECK(ok.ok);
    NF_CHECK(ok.meshes.size() == 1u);
    NF_CHECK(ok.meshes[0]->vertices.size() == 3u);
    NF_CHECK(ok.meshes[0]->indices.size() == 3u);
    NF_CHECK(ok.faces_skipped == 0u);
    NF_CHECK_NEAR(ok.meshes[0]->vertices[0].normal[2], 1.0f, 1e-5f);

    // A face whose THIRD reference is out of range. The first two references
    // added vertices before the face was rejected; those must not survive, or a
    // malformed file silently inflates the asset's vertex array.
    write_text(scratch.file("Broken.obj"),
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vn 0 0 1\n"
               "f 1//1 2//1 3//9\n"); // normal 9 does not exist
    const MeshImportResult bad = import_mesh_file(scratch.file("Broken.obj").string());
    NF_CHECK(!bad.ok);
    NF_CHECK(bad.error.find("no usable faces") != std::string::npos);
    NF_CHECK(bad.faces_skipped == 1u);

    // One good face plus one broken one: the good face's geometry is kept, the
    // broken face leaves nothing behind.
    write_text(scratch.file("Mixed.obj"),
               "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
               "vn 0 0 1\n"
               "f 1//1 2//1 3//1\n"
               "f 1//1 2//1 4//9\n");
    const MeshImportResult mixed = import_mesh_file(scratch.file("Mixed.obj").string());
    NF_CHECK(mixed.ok);
    NF_CHECK(mixed.faces_skipped == 1u);
    // Three vertices — not five: the rejected face's two orphan vertices are
    // gone, so vertex_count still describes the geometry that exists.
    NF_CHECK(mixed.meshes[0]->vertices.size() == 3u);
    NF_CHECK(mixed.meshes[0]->indices.size() == 3u);
}
