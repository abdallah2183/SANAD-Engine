// AssetTests — glTF 2.0 import: embedded + external buffers, attributes,
// materials, nodes, and failure paths.
//
// Hermetic by design: every document is synthesized in memory (tiny base64
// encoder + JSON template below), so no binary fixtures. External-buffer
// loading is covered by a temp-dir round-trip.

#include <NF/Assets/GltfImport.hpp>
#include <NF/Test/TestFramework.hpp>
#include <NF/Test/TestPaths.hpp>

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
    // glTF front faces are CCW; SANAD's rasterizer uses FrontFace::CW. The
    // importer converts the convention by swapping the last two indices —
    // positions, normals and UVs stay exactly as the file authored them.
    NF_CHECK(m.indices[0] == 0 && m.indices[1] == 2 && m.indices[2] == 1);
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
    // Non-indexed primitives go through the same CCW -> CW conversion.
    NF_CHECK(r.meshes[0]->indices[0] == 0);
    NF_CHECK(r.meshes[0]->indices[1] == 2);
    NF_CHECK(r.meshes[0]->indices[2] == 1);
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
    const auto dir = nf::test::temp_root() / "nf_gltf_test";
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

// ---------------------------------------------------------------------------
// Emissive handling.
//
// glTF 2.0: emission = emissiveFactor * emissiveTexture, scaled by
// KHR_materials_emissive_strength. emissiveFactor defaults to [0,0,0], so a
// document that never mentions emission emits nothing at all — no matter which
// textures it binds or what strength it declares. Downstream, the engine reads
// "colour 0,0,0 with a non-zero strength" as "no colour given" and substitutes
// the base colour (PBRMaterialParams::pack), which is why the resolved strength
// — not just the resolved colour — has to carry the "no emission" fact.
// ---------------------------------------------------------------------------

namespace {

// The same triangle document as tri_json(), with the whole material block
// replaced, so a test can state exactly the material a glTF author wrote
// instead of copying the geometry boilerplate again.
std::string tri_json_with_material(const std::string& material_json) {
    auto buf = make_tri_buffer(true, true);
    std::string json =
        tri_json("data:application/octet-stream;base64," + base64_encode(buf), true, true, 4);
    const std::string original =
        "\"materials\":[{\"name\":\"TestMat\",\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[1.0,0.5,0.25,1.0],\"metallicFactor\":0.2,"
        "\"roughnessFactor\":0.8}}]";
    const usize at = json.find(original);
    // A miss leaves the canned material in place: the test's own assertions then
    // fail on real values instead of on a broken document.
    if (at != std::string::npos) {
        json.replace(at, original.size(), "\"materials\":[" + material_json + "]");
        // cgltf only surfaces material extensions declared in extensionsUsed.
        if (material_json.find("KHR_materials_emissive_strength") != std::string::npos) {
            const std::string head = "{\"asset\":{\"version\":\"2.0\"},";
            const usize h = json.find(head);
            if (h != std::string::npos) {
                json.insert(h + head.size(),
                            "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],");
            }
        }
    }
    return json;
}

GltfImportResult import_material(const std::string& material_json) {
    const std::string json = tri_json_with_material(material_json);
    return import_gltf_memory(json.data(), json.size(), "test");
}

} // namespace

NF_TEST(gltf_non_emissive_material_resolves_to_no_emission) {
    // What the facade test asset's limestone, timber and iron declare: a base
    // colour and nothing about emission.
    GltfImportResult r = import_material(
        "{\"name\":\"Limestone\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.62,0.58,0.5,1.0],"
        "\"metallicFactor\":0.0,\"roughnessFactor\":0.85}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    const GltfMaterialInfo& m = r.materials[0];
    NF_CHECK(m.name == "Limestone");
    NF_CHECK_NEAR(m.emissive[0], 0.0f, 1e-6f);
    NF_CHECK_NEAR(m.emissive[1], 0.0f, 1e-6f);
    NF_CHECK_NEAR(m.emissive[2], 0.0f, 1e-6f);
    // Zero strength is what "does not glow" means once packed.
    NF_CHECK_NEAR(m.emissive_strength, 0.0f, 1e-6f);
}

NF_TEST(gltf_zero_factor_with_strength_extension_still_emits_nothing) {
    // The factor is the gate: scaling zero by four is still zero.
    GltfImportResult r = import_material(
        "{\"name\":\"ZeroFactorStrength\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":4.0}}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    NF_CHECK_NEAR(r.materials[0].emissive[0], 0.0f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].emissive_strength, 0.0f, 1e-6f);
}

NF_TEST(gltf_genuine_emitter_keeps_spec_default_strength) {
    // No KHR_materials_emissive_strength: the spec default of 1 must survive.
    GltfImportResult r = import_material(
        "{\"name\":\"Lamp\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"emissiveFactor\":[1.0,0.45,0.1]}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    NF_CHECK_NEAR(r.materials[0].emissive[0], 1.0f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].emissive[1], 0.45f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].emissive[2], 0.1f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].emissive_strength, 1.0f, 1e-6f);
}

NF_TEST(gltf_emissive_strength_extension_scales_genuine_emitter) {
    GltfImportResult r = import_material(
        "{\"name\":\"LampDim\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"emissiveFactor\":[0.2,0.5,1.0],"
        "\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":0.5}}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    NF_CHECK_NEAR(r.materials[0].emissive[0], 0.2f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].emissive[1], 0.5f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].emissive[2], 1.0f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].emissive_strength, 0.5f, 1e-6f);
}

NF_TEST(gltf_single_emissive_channel_counts_as_declared) {
    // glTF clamps the factor to [0,1]; it never zeroes a channel, so one
    // non-zero channel is a real declaration of emission.
    GltfImportResult r = import_material(
        "{\"name\":\"OneChannel\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"emissiveFactor\":[0,0,0.25]}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    NF_CHECK_NEAR(r.materials[0].emissive[2], 0.25f, 1e-6f);
    NF_CHECK_NEAR(r.materials[0].emissive_strength, 1.0f, 1e-6f);
}

NF_TEST(gltf_facade_stone_timber_iron_are_non_emissive) {
    // The three material families that lit up white in the Phase 02B facade
    // review. Different albedos, metallicities and roughnesses, but all three
    // are ordinary PBR surfaces that never mention emission.
    GltfImportResult r = import_material(
        "{\"name\":\"Facade | Limestone\",\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.62,0.58,0.50,1.0],\"metallicFactor\":0.0,\"roughnessFactor\":0.85}},"
        "{\"name\":\"Facade | Aged Timber\",\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.31,0.19,0.11,1.0],\"metallicFactor\":0.0,\"roughnessFactor\":0.70}},"
        "{\"name\":\"Facade | Aged Iron\",\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.18,0.19,0.20,1.0],\"metallicFactor\":0.85,\"roughnessFactor\":0.45}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 3);
    if (r.materials.size() != 3) return;
    NF_CHECK(r.materials[0].name == "Facade | Limestone");
    NF_CHECK(r.materials[1].name == "Facade | Aged Timber");
    NF_CHECK(r.materials[2].name == "Facade | Aged Iron");
    for (const GltfMaterialInfo& m : r.materials) {
        for (int c = 0; c < 3; ++c) {
            NF_CHECK_NEAR(m.emissive[c], 0.0f, 1e-6f);
        }
        NF_CHECK_NEAR(m.emissive_strength, 0.0f, 1e-6f);
    }
}

namespace {

// tri_json() has no image slots. Inject one 1x1 PNG as a data URI so a test
// can bind a REAL emissiveTexture without shipping a binary fixture.
GltfImportResult import_material_with_texture(const std::string& material_json) {
    std::string json = tri_json_with_material(material_json);
    const std::string slots =
        "\"images\":[{\"name\":\"EmissivePattern\",\"uri\":\"data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mP4//8/"
        "AAX+Av4zEpUUAAAAAElFTkSuQmCC\"}],"
        "\"textures\":[{\"source\":0}],\"samplers\":[{}],";
    const usize at = json.find("\"meshes\":");
    if (at != std::string::npos) {
        json.insert(at, slots);
    }
    return import_gltf_memory(json.data(), json.size(), "test");
}

} // namespace

NF_TEST(gltf_zero_factor_with_emissive_texture_emits_nothing) {
    // glTF multiplies the texture by emissiveFactor: zero factor x any map = 0.
    // Binding the map must not re-enable the default strength.
    GltfImportResult r = import_material_with_texture(
        "{\"name\":\"ZeroFactorTexture\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"emissiveFactor\":[0,0,0],\"emissiveTexture\":{\"index\":0}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].emissive_image >= 0); // the map really was carried
    for (int c = 0; c < 3; ++c) {
        NF_CHECK_NEAR(r.materials[0].emissive[c], 0.0f, 1e-6f);
    }
    NF_CHECK_NEAR(r.materials[0].emissive_strength, 0.0f, 1e-6f);
}

NF_TEST(gltf_genuine_emitter_with_emissive_texture_keeps_strength) {
    // The mirror case: a real emitter with a map must NOT be turned off.
    GltfImportResult r = import_material_with_texture(
        "{\"name\":\"LampWithMap\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"emissiveFactor\":[1,1,1],\"emissiveTexture\":{\"index\":0}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].emissive_image >= 0);
    for (int c = 0; c < 3; ++c) {
        NF_CHECK_NEAR(r.materials[0].emissive[c], 1.0f, 1e-6f);
    }
    NF_CHECK_NEAR(r.materials[0].emissive_strength, 1.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// glTF facing conventions (Phase 02C.1 follow-up): CCW -> engine CW, and the
// `doubleSided` material flag. These are what make an arbitrary GLB render its
// authored colours instead of its unlit interior.
// ---------------------------------------------------------------------------

NF_TEST(gltf_double_sided_is_carried_and_normals_survive_winding_conversion) {
    GltfImportResult r = import_material(
        "{\"name\":\"Leaf\",\"doubleSided\":true,"
        "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.8,0.1,1.0]}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].double_sided);
    NF_CHECK(r.meshes.size() == 1);
    if (r.meshes.empty()) return;
    const MeshAsset& m = *r.meshes[0];
    // Convention conversion swaps indices, never vertex attributes.
    NF_CHECK(m.indices.size() == 3);
    NF_CHECK(m.indices[0] == 0 && m.indices[1] == 2 && m.indices[2] == 1);
    for (const auto& v : m.vertices) {
        NF_CHECK_NEAR(v.normal[2], 1.0f, 1e-5f); // authored +Z, not inverted
    }
    NF_CHECK_NEAR(m.vertices[1].position[0], 1.0f, 1e-6f);
}

NF_TEST(gltf_single_sided_material_stays_single_sided) {
    // glTF defaults doubleSided to false; engine-authored materials must keep
    // back-face culling so the change cannot silently make every mesh 2-sided.
    GltfImportResult r = import_material(
        "{\"name\":\"Wall\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.5,0.5,1.0]}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(!r.materials[0].double_sided);
}

// ---------------------------------------------------------------------------
// glTF `normalTexture.scale` (Phase 02C.2): the normal-map strength the source
// authored. Dropping it made every map render at strength 1 regardless of what
// Blender exported, so a deliberately flattened or reinforced detail read wrong.
// ---------------------------------------------------------------------------

NF_TEST(gltf_normal_texture_scale_is_carried) {
    GltfImportResult r = import_material_with_texture(
        "{\"name\":\"Stone\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"normalTexture\":{\"index\":0,\"scale\":2.0}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].normal_image >= 0); // the map really was carried
    NF_CHECK_NEAR(r.materials[0].normal_scale, 2.0f, 1e-6f);
}

NF_TEST(gltf_normal_texture_scale_zero_is_not_mistaken_for_absent) {
    // Zero is a legal, meaningful value — it flattens the map to the geometric
    // normal. A guard like `if (scale) info.normal_scale = scale;` would
    // silently promote it back to 1 and re-enable a detail the author removed.
    GltfImportResult r = import_material_with_texture(
        "{\"name\":\"Flat\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"normalTexture\":{\"index\":0,\"scale\":0.0}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].normal_image >= 0);
    NF_CHECK_NEAR(r.materials[0].normal_scale, 0.0f, 1e-6f);
}

NF_TEST(gltf_normal_texture_without_scale_defaults_to_one) {
    // The glTF 2.0 spec default. Assets that omit the key — every asset
    // exported before this was read — must keep rendering exactly as before.
    GltfImportResult r = import_material_with_texture(
        "{\"name\":\"Stone\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"normalTexture\":{\"index\":0}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].normal_image >= 0);
    NF_CHECK_NEAR(r.materials[0].normal_scale, 1.0f, 1e-6f);
}

NF_TEST(gltf_material_without_normal_texture_keeps_scale_default) {
    // No normal map at all: the payload default must still be the glTF default,
    // so a material that later gains a map in the editor starts at strength 1.
    GltfImportResult r = import_material(
        "{\"name\":\"Plain\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.5,0.5,1.0]}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].normal_image < 0);
    NF_CHECK_NEAR(r.materials[0].normal_scale, 1.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// glTF `occlusionTexture.strength` (Phase 02C.3): the AO blend the source
// authored. Dropping it made every occlusion map render at full strength, so a
// deliberately softened (or disabled) AO band read far darker than Blender.
// ---------------------------------------------------------------------------

NF_TEST(gltf_occlusion_texture_strength_is_carried) {
    GltfImportResult r = import_material_with_texture(
        "{\"name\":\"Stone\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"occlusionTexture\":{\"index\":0,\"strength\":0.5}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].occlusion_image >= 0); // the map really was carried
    NF_CHECK_NEAR(r.materials[0].occlusion_strength, 0.5f, 1e-6f);
}

NF_TEST(gltf_occlusion_texture_strength_zero_is_not_mistaken_for_absent) {
    // Zero is a legal, meaningful value — it disables the map entirely. A guard
    // like `if (strength) info.occlusion_strength = strength;` would silently
    // promote it back to 1 and re-darken an AO band the author removed.
    GltfImportResult r = import_material_with_texture(
        "{\"name\":\"Open\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"occlusionTexture\":{\"index\":0,\"strength\":0.0}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].occlusion_image >= 0);
    NF_CHECK_NEAR(r.materials[0].occlusion_strength, 0.0f, 1e-6f);
}

NF_TEST(gltf_occlusion_texture_without_strength_defaults_to_one) {
    // The glTF 2.0 spec default. Assets that omit the key must keep rendering
    // exactly as before (full-strength occlusion).
    GltfImportResult r = import_material_with_texture(
        "{\"name\":\"Stone\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"occlusionTexture\":{\"index\":0}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].occlusion_image >= 0);
    NF_CHECK_NEAR(r.materials[0].occlusion_strength, 1.0f, 1e-6f);
}

NF_TEST(gltf_material_without_occlusion_texture_keeps_strength_default) {
    // No occlusion map at all: the payload default must still be the glTF
    // default, so a material that later gains a map starts at strength 1.
    GltfImportResult r = import_material(
        "{\"name\":\"Plain\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.5,0.5,1.0]}}");
    NF_CHECK(r.ok);
    NF_CHECK(r.materials.size() == 1);
    if (r.materials.size() != 1) return;
    NF_CHECK(r.materials[0].occlusion_image < 0);
    NF_CHECK_NEAR(r.materials[0].occlusion_strength, 1.0f, 1e-6f);
}
