// AssetTests — glTF 2.0 import: embedded + external buffers, attributes,
// materials, nodes, and failure paths.
//
// Hermetic by design: every document is synthesized in memory (tiny base64
// encoder + JSON template below), so no binary fixtures. External-buffer
// loading is covered by a temp-dir round-trip.

#include <NF/Assets/GltfImport.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::assets;

namespace {

std::string base64_encode(const std::vector<u8>& in) {
    static const char* kDigits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (usize i = 0; i < in.size(); i += 3) {
        const u32 a = in[i];
        const u32 b = i + 1 < in.size() ? in[i + 1] : 0;
        const u32 c = i + 2 < in.size() ? in[i + 2] : 0;
        const u32 triple = (a << 16) | (b << 8) | c;
        out.push_back(kDigits[(triple >> 18) & 63]);
        out.push_back(kDigits[(triple >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? kDigits[(triple >> 6) & 63] : '=');
        out.push_back(i + 2 < in.size() ? kDigits[triple & 63] : '=');
    }
    return out;
}

void push_f32(std::vector<u8>& b, float v) {
    u32 u = 0;
    std::memcpy(&u, &v, 4);
    b.push_back(static_cast<u8>(u & 0xFF));
    b.push_back(static_cast<u8>((u >> 8) & 0xFF));
    b.push_back(static_cast<u8>((u >> 16) & 0xFF));
    b.push_back(static_cast<u8>((u >> 24) & 0xFF));
}
void push_u16(std::vector<u8>& b, u16 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
}

// Triangle buffer: positions, normals, uvs, u16 indices. Flags drop sections.
std::vector<u8> make_tri_buffer(bool with_normals, bool with_indices) {
    std::vector<u8> b;
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (float v : pos) push_f32(b, v);
    if (with_normals) {
        const float nrm[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        for (float v : nrm) push_f32(b, v);
    }
    const float uv[6] = {0, 0, 1, 0, 0, 1};
    for (float v : uv) push_f32(b, v);
    if (with_indices) {
        push_u16(b, 0);
        push_u16(b, 1);
        push_u16(b, 2);
    }
    return b;
}

std::string tri_json(const std::string& buffer_uri, bool with_normals, bool with_indices,
                     int prim_mode /* 4 = TRIANGLES, 1 = LINES */) {
    // Layout: pos[36] nrm?[36] uv[24] idx?[6]
    const u32 nrm_len = with_normals ? 36 : 0;
    const u32 idx_len = with_indices ? 6 : 0;
    const u32 total_len = 36 + nrm_len + 24 + idx_len;
    const u32 uv_off = 36 + nrm_len;
    const u32 idx_off = uv_off + 24;
    std::string attrs = "\"POSITION\":0";
    int next_acc = 1;
    int nrm_acc = -1, uv_acc = -1;
    if (with_normals) {
        nrm_acc = next_acc++;
        attrs += ",\"NORMAL\":" + std::to_string(nrm_acc);
    }
    uv_acc = next_acc++;
    attrs += ",\"TEXCOORD_0\":" + std::to_string(uv_acc);
    int idx_acc = -1;
    if (with_indices) idx_acc = next_acc++;

    std::string accessors = "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}";
    if (with_normals) {
        accessors +=
            ",{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}";
    }
    accessors += ",{\"bufferView\":" + std::to_string(with_normals ? 2 : 1) +
                 ",\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}";
    if (with_indices) {
        accessors += ",{\"bufferView\":" + std::to_string(with_normals ? 3 : 2) +
                     ",\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}";
    }
    std::string views = "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}";
    if (with_normals) views += ",{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36}";
    views += ",{\"buffer\":0,\"byteOffset\":" + std::to_string(uv_off) + ",\"byteLength\":24}";
    if (with_indices) {
        views += ",{\"buffer\":0,\"byteOffset\":" + std::to_string(idx_off) + ",\"byteLength\":6}";
    }

    std::string prim = "{\"attributes\":{" + attrs + "}";
    if (with_indices) prim += ",\"indices\":" + std::to_string(idx_acc);
    prim += ",\"material\":0,\"mode\":" + std::to_string(prim_mode) + "}";

    return "{\"asset\":{\"version\":\"2.0\"},"
           "\"buffers\":[{\"byteLength\":" +
           std::to_string(total_len) + ",\"uri\":\"" + buffer_uri + "\"}],"
           "\"bufferViews\":[" +
           views + "]," + "\"accessors\":[" + accessors + "]," +
           "\"materials\":[{\"name\":\"TestMat\",\"pbrMetallicRoughness\":{"
           "\"baseColorFactor\":[1.0,0.5,0.25,1.0],\"metallicFactor\":0.2,"
           "\"roughnessFactor\":0.8}}]," +
           "\"meshes\":[{\"name\":\"TriMesh\",\"primitives\":[" + prim + "]}]," +
           "\"nodes\":[{\"name\":\"TriNode\",\"translation\":[5.0,0.0,0.0],\"mesh\":0}],"
           "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
}

GltfImportResult import_embedded(bool with_normals, bool with_indices, int mode = 4) {
    auto buf = make_tri_buffer(with_normals, with_indices);
    std::string json = tri_json("data:application/octet-stream;base64," + base64_encode(buf),
                                with_normals, with_indices, mode);
    return import_gltf_memory(json.data(), json.size(), "test");
}

} // namespace

// ---------------------------------------------------------------------------
// Positive: full triangle
// ---------------------------------------------------------------------------

NF_TEST(gltf_import_indexed_triangle) {
    GltfImportResult r = import_embedded(true, true);
    NF_CHECK(r.ok);
    NF_CHECK(r.error.empty());
    NF_CHECK(r.meshes.size() == 1);
    NF_CHECK(r.primitives_skipped == 0);

    const MeshAsset& m = *r.meshes[0];
    NF_CHECK(m.vertices.size() == 3);
    NF_CHECK(m.indices.size() == 3);
    NF_CHECK(m.indices[0] == 0 && m.indices[1] == 1 && m.indices[2] == 2);
    NF_CHECK_NEAR(m.vertices[1].position[0], 1.0f, 1e-6f);
    NF_CHECK_NEAR(m.vertices[2].position[1], 1.0f, 1e-6f);
    NF_CHECK_NEAR(m.vertices[0].normal[2], 1.0f, 1e-6f);
    NF_CHECK_NEAR(m.vertices[1].uv0[0], 1.0f, 1e-6f);
    NF_CHECK_NEAR(m.vertices[2].uv0[1], 1.0f, 1e-6f);

    NF_CHECK(m.submeshes.size() == 1);
    NF_CHECK(m.submeshes[0].index_count == 3);
    NF_CHECK(m.submeshes[0].vertex_count == 3);
    NF_CHECK(m.submeshes[0].material_slot == 0);

    NF_CHECK_NEAR(m.bounds.min_x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(m.bounds.max_x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(m.bounds.max_y, 1.0f, 1e-6f);
    NF_CHECK_NEAR(m.sphere.cx, 0.5f, 1e-6f);
    NF_CHECK_NEAR(m.sphere.cy, 0.5f, 1e-6f);
    NF_CHECK_NEAR(m.sphere.radius, 0.70710678f, 1e-5f);

    NF_CHECK(r.materials.size() == 1);
    NF_CHECK(r.materials[0].name == "TestMat");
    NF_CHECK_NEAR(r.materials[0].base_color[0], 1.0f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].base_color[1], 0.5f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].base_color[2], 0.25f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].metallic, 0.2f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].roughness, 0.8f, 1e-6f);

    NF_CHECK(r.nodes.size() == 1);
    NF_CHECK(r.nodes[0].name == "TriNode");
    NF_CHECK_NEAR(r.nodes[0].translation[0], 5.0f, 1e-6f);
    NF_CHECK(r.nodes[0].mesh_index == 0);
    NF_CHECK(r.nodes[0].parent_index == -1);
}

NF_TEST(gltf_missing_normals_are_computed_smooth) {
    GltfImportResult r = import_embedded(false, true);
    NF_CHECK(r.ok);
    NF_CHECK(r.meshes.size() == 1);
    // Winding (0,0,0),(1,0,0),(0,1,0) faces +Z.
    for (const auto& v : r.meshes[0]->vertices) {
        NF_CHECK_NEAR(v.normal[0], 0.0f, 1e-5f);
        NF_CHECK_NEAR(v.normal[1], 0.0f, 1e-5f);
        NF_CHECK_NEAR(v.normal[2], 1.0f, 1e-5f);
    }
}

NF_TEST(gltf_non_indexed_triangle) {
    GltfImportResult r = import_embedded(true, false);
    NF_CHECK(r.ok);
    NF_CHECK(r.meshes.size() == 1);
    NF_CHECK(r.meshes[0]->vertices.size() == 3);
    NF_CHECK(r.meshes[0]->indices.size() == 3);
    NF_CHECK(r.meshes[0]->indices[2] == 2);
}

// ---------------------------------------------------------------------------
// Skips and failures
// ---------------------------------------------------------------------------

NF_TEST(gltf_lines_primitive_is_skipped_loudly) {
    GltfImportResult r = import_embedded(true, true, 1); // LINES
    NF_CHECK(r.ok); // the document is fine; the primitive is not our kind
    NF_CHECK(r.meshes.empty());
    NF_CHECK(r.primitives_skipped == 1);
}

NF_TEST(gltf_rejects_invalid_documents) {
    GltfImportResult e1 = import_gltf_memory(nullptr, 0);
    NF_CHECK(!e1.ok);

    const char not_json[] = "hello, this is not gltf";
    GltfImportResult e2 = import_gltf_memory(not_json, sizeof(not_json));
    NF_CHECK(!e2.ok);
    NF_CHECK(!e2.error.empty());

    // Malformed JSON cannot parse.
    const char bad[] = "{\"asset\": {\"version\": \"2.0\"";
    GltfImportResult e3 = import_gltf_memory(bad, sizeof(bad));
    NF_CHECK(!e3.ok);
    NF_CHECK(!e3.error.empty());

    // Valid JSON that is not a glTF asset: parses as an empty document.
    const char empty[] = "{\"foo\": 42}";
    GltfImportResult e3b = import_gltf_memory(empty, sizeof(empty));
    NF_CHECK(e3b.ok);
    NF_CHECK(e3b.meshes.empty());

    GltfImportResult e4 = import_gltf_file("does/not/exist_12345.gltf");
    NF_CHECK(!e4.ok);
    NF_CHECK(!e4.error.empty());
}

NF_TEST(gltf_external_bin_roundtrip) {
    auto buf = make_tri_buffer(true, true);
    std::string json = tri_json("test_tri.bin", true, true, 4);
    const auto dir = std::filesystem::temp_directory_path() / "nf_gltf_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    {
        std::FILE* f = nullptr;
#if defined(_MSC_VER)
        fopen_s(&f, (dir / "test_tri.bin").string().c_str(), "wb");
#else
        f = std::fopen((dir / "test_tri.bin").string().c_str(), "wb");
#endif
        NF_CHECK(f != nullptr);
        if (f) {
            std::fwrite(buf.data(), 1, buf.size(), f);
            std::fclose(f);
        }
    }
    {
        std::FILE* f = nullptr;
#if defined(_MSC_VER)
        fopen_s(&f, (dir / "test_tri.gltf").string().c_str(), "wb");
#else
        f = std::fopen((dir / "test_tri.gltf").string().c_str(), "wb");
#endif
        NF_CHECK(f != nullptr);
        if (f) {
            std::fwrite(json.data(), 1, json.size(), f);
            std::fclose(f);
        }
    }
    GltfImportResult r = import_gltf_file((dir / "test_tri.gltf").string());
    std::filesystem::remove_all(dir, ec);
    NF_CHECK(r.ok);
    NF_CHECK(r.meshes.size() == 1);
    NF_CHECK(r.meshes[0]->vertices.size() == 3);
    NF_CHECK_NEAR(r.meshes[0]->vertices[1].position[0], 1.0f, 1e-6f);
}
