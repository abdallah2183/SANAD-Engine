// NF/Assets/GltfImport.cpp — glTF 2.0 -> MeshAsset conversion.

#include <NF/Assets/GltfImport.hpp>
#include <NF/Core/Logger.hpp>

#include <cgltf.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nf::assets {

namespace {

// Reads one vertex element (handles stride/offset, normalization and
// component conversion via cgltf). Returns false on out-of-range access.
bool read_attrib_floats(const cgltf_accessor* acc, usize vertex, float* out, usize count) {
    if (!acc) return false;
    if (vertex >= acc->count) return false;
    return cgltf_accessor_read_float(acc, vertex, out, count) != 0;
}

// Reads one index value regardless of component width.
bool read_index(const cgltf_accessor* acc, usize i, u32& out) {
    if (!acc || i >= acc->count) return false;
    out = static_cast<u32>(cgltf_accessor_read_index(acc, i));
    return true;
}

void compute_mesh_bounds(MeshAsset& mesh) {
    if (mesh.vertices.empty()) return;
    AssetAABB box;
    box.min_x = box.max_x = mesh.vertices[0].position[0];
    box.min_y = box.max_y = mesh.vertices[0].position[1];
    box.min_z = box.max_z = mesh.vertices[0].position[2];
    for (const auto& v : mesh.vertices) {
        if (v.position[0] < box.min_x) box.min_x = v.position[0];
        if (v.position[0] > box.max_x) box.max_x = v.position[0];
        if (v.position[1] < box.min_y) box.min_y = v.position[1];
        if (v.position[1] > box.max_y) box.max_y = v.position[1];
        if (v.position[2] < box.min_z) box.min_z = v.position[2];
        if (v.position[2] > box.max_z) box.max_z = v.position[2];
    }
    mesh.bounds = box;
    const float cx = (box.min_x + box.max_x) * 0.5f;
    const float cy = (box.min_y + box.max_y) * 0.5f;
    const float cz = (box.min_z + box.max_z) * 0.5f;
    float r2 = 0.0f;
    for (const auto& v : mesh.vertices) {
        const float dx = v.position[0] - cx;
        const float dy = v.position[1] - cy;
        const float dz = v.position[2] - cz;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) r2 = d2;
    }
    mesh.sphere = AssetSphere{cx, cy, cz, std::sqrt(r2)};
}

// Smooth (angle-averaged) normals for primitives that ship without NORMAL.
void compute_smooth_normals(std::vector<AssetVertex>& verts, const std::vector<u32>& indices,
                            usize base_vertex, usize index_start, usize index_count) {
    for (usize i = index_start; i + 2 < index_start + index_count; i += 3) {
        const u32 a = indices[i] + static_cast<u32>(base_vertex);
        const u32 b = indices[i + 1] + static_cast<u32>(base_vertex);
        const u32 c = indices[i + 2] + static_cast<u32>(base_vertex);
        if (a >= verts.size() || b >= verts.size() || c >= verts.size()) continue;
        const float* pa = verts[a].position;
        const float* pb = verts[b].position;
        const float* pc = verts[c].position;
        const float ux = pb[0] - pa[0], uy = pb[1] - pa[1], uz = pb[2] - pa[2];
        const float vx = pc[0] - pa[0], vy = pc[1] - pa[1], vz = pc[2] - pa[2];
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        verts[a].normal[0] += nx; verts[a].normal[1] += ny; verts[a].normal[2] += nz;
        verts[b].normal[0] += nx; verts[b].normal[1] += ny; verts[b].normal[2] += nz;
        verts[c].normal[0] += nx; verts[c].normal[1] += ny; verts[c].normal[2] += nz;
    }
    for (usize i = base_vertex; i < verts.size(); ++i) {
        float* n = verts[i].normal;
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-12f) {
            n[0] /= len; n[1] /= len; n[2] /= len;
        } else {
            n[0] = 0; n[1] = 0; n[2] = 1;
        }
    }
}

// Converts one glTF mesh (all triangle primitives merged, one submesh each).
// Returns false when nothing convertible was found (caller counts the skip).
bool convert_mesh(const cgltf_data* data, const cgltf_mesh& mesh, usize mesh_index,
                  const std::string& logical_path, MeshAsset& out, u32& skipped) {
    out.logical_path = logical_path + "#mesh" + std::to_string(mesh_index);
    out.format_version = 1;

    for (usize pi = 0; pi < mesh.primitives_count; ++pi) {
        const cgltf_primitive& prim = mesh.primitives[pi];
        if (prim.type != cgltf_primitive_type_triangles) {
            ++skipped;
            continue;
        }
        const cgltf_accessor* pos = nullptr;
        const cgltf_accessor* nrm = nullptr;
        const cgltf_accessor* uv = nullptr;
        const cgltf_accessor* tan = nullptr;
        for (usize ai = 0; ai < prim.attributes_count; ++ai) {
            const cgltf_attribute& attr = prim.attributes[ai];
            if (!attr.data) continue;
            switch (attr.type) {
                case cgltf_attribute_type_position: pos = attr.data; break;
                case cgltf_attribute_type_normal: nrm = attr.data; break;
                case cgltf_attribute_type_texcoord:
                    if (attr.index == 0) uv = attr.data;
                    break;
                case cgltf_attribute_type_tangent: tan = attr.data; break;
                default: break;
            }
        }
        if (!pos || pos->type != cgltf_type_vec3 || pos->count == 0) {
            ++skipped;
            continue;
        }
        if (pos->count > 100000000) { // absurd-input guard before widening casts
            ++skipped;
            continue;
        }

        const usize base_vertex = out.vertices.size();
        const usize base_index = out.indices.size();
        for (usize vi = 0; vi < pos->count; ++vi) {
            AssetVertex v;
            read_attrib_floats(pos, vi, v.position, 3);
            if (nrm && nrm->type == cgltf_type_vec3 && vi < nrm->count) {
                read_attrib_floats(nrm, vi, v.normal, 3);
            }
            if (uv && vi < uv->count) {
                const usize comps = cgltf_num_components(uv->type);
                float tmp[4] = {0, 0, 0, 0};
                const usize want = comps < 4 ? comps : 4;
                if (read_attrib_floats(uv, vi, tmp, want)) {
                    v.uv0[0] = tmp[0];
                    v.uv0[1] = want > 1 ? tmp[1] : 0.0f;
                }
            }
            if (tan && tan->type == cgltf_type_vec4 && vi < tan->count) {
                read_attrib_floats(tan, vi, v.tangent, 4);
            }
            out.vertices.push_back(v);
        }

        usize prim_index_count = 0;
        if (prim.indices) {
            if (prim.indices->count == 0 || prim.indices->count % 3 != 0) {
                // Roll back the vertices: a corrupt index list must not leave
                // a half-primitive behind.
                out.vertices.resize(base_vertex);
                ++skipped;
                continue;
            }
            for (usize ii = 0; ii < prim.indices->count; ++ii) {
                u32 idx = 0;
                if (!read_index(prim.indices, ii, idx) || idx >= pos->count) {
                    out.vertices.resize(base_vertex);
                    out.indices.resize(base_index);
                    prim_index_count = 0;
                    break;
                }
                out.indices.push_back(static_cast<uint32_t>(base_vertex) + idx);
                ++prim_index_count;
            }
            if (prim_index_count == 0) {
                ++skipped;
                continue;
            }
        } else {
            // Non-indexed: sequential triangles.
            if (pos->count % 3 != 0) {
                out.vertices.resize(base_vertex);
                ++skipped;
                continue;
            }
            for (usize ii = 0; ii < pos->count; ++ii) {
                out.indices.push_back(static_cast<uint32_t>(base_vertex + ii));
            }
            prim_index_count = pos->count;
        }

        if (!nrm) {
            compute_smooth_normals(out.vertices, out.indices, base_vertex, base_index,
                                   prim_index_count);
        }

        AssetSubMesh sub;
        sub.index_offset = static_cast<uint32_t>(base_index);
        sub.index_count = static_cast<uint32_t>(prim_index_count);
        sub.vertex_offset = static_cast<uint32_t>(base_vertex);
        sub.vertex_count = static_cast<uint32_t>(pos->count);
        if (prim.material) {
            // Material slot == index into the file's material list.
            sub.material_slot =
                static_cast<uint32_t>(prim.material - data->materials);
        }
        out.submeshes.push_back(sub);
    }

    if (out.submeshes.empty()) return false;
    if (mesh.name) out.logical_path = std::string(logical_path) + "#" + mesh.name;
    compute_mesh_bounds(out);
    return true;
}

GltfImportResult convert_parsed(cgltf_data* data, const std::string& logical_path) {
    GltfImportResult result;
    u32 skipped = 0;

    for (usize mi = 0; mi < data->materials_count; ++mi) {
        const cgltf_material& m = data->materials[mi];
        GltfMaterialInfo info;
        if (m.name) info.name = m.name;
        if (m.has_pbr_metallic_roughness) {
            for (int c = 0; c < 4; ++c) {
                info.base_color[c] = m.pbr_metallic_roughness.base_color_factor[c];
            }
            info.metallic = m.pbr_metallic_roughness.metallic_factor;
            info.roughness = m.pbr_metallic_roughness.roughness_factor;
        }
        result.materials.push_back(info);
    }

    for (usize mi = 0; mi < data->meshes_count; ++mi) {
        auto mesh = std::make_unique<MeshAsset>();
        if (convert_mesh(data, data->meshes[mi], mi, logical_path, *mesh, skipped)) {
            result.meshes.push_back(std::move(mesh));
        }
    }
    result.primitives_skipped = skipped;

    for (usize ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& n = data->nodes[ni];
        GltfNodeInfo info;
        if (n.name) info.name = n.name;
        if (n.has_translation) {
            info.translation[0] = n.translation[0];
            info.translation[1] = n.translation[1];
            info.translation[2] = n.translation[2];
        }
        if (n.has_rotation) {
            info.rotation[0] = n.rotation[0];
            info.rotation[1] = n.rotation[1];
            info.rotation[2] = n.rotation[2];
            info.rotation[3] = n.rotation[3];
        }
        if (n.has_scale) {
            info.scale[0] = n.scale[0];
            info.scale[1] = n.scale[1];
            info.scale[2] = n.scale[2];
        }
        if (n.mesh) {
            info.mesh_index = static_cast<int>(n.mesh - data->meshes);
        }
        if (n.parent) {
            info.parent_index = static_cast<int>(n.parent - data->nodes);
        }
        result.nodes.push_back(info);
    }

    result.ok = true;
    NF_LOG_INFO(LogCategory::Asset, "GltfImport: '{}' -> {} mesh(es), {} material(s), {} node(s), {} skipped",
                logical_path, result.meshes.size(), result.materials.size(),
                result.nodes.size(), skipped);
    return result;
}

} // namespace

GltfImportResult import_gltf_memory(const void* data, usize size, const std::string& logical_path) {
    GltfImportResult result;
    if (!data || size == 0) {
        result.error = "import_gltf_memory: empty input";
        return result;
    }
    cgltf_options options{};
    cgltf_data* parsed = nullptr;
    if (cgltf_parse(&options, data, size, &parsed) != cgltf_result_success || !parsed) {
        result.error = "not a valid glTF document";
        return result;
    }
    // Resolves embedded (base64) buffers; external .bin needs the file path
    // variant below, so a missing external buffer fails here, loudly.
    if (cgltf_load_buffers(&options, parsed, nullptr) != cgltf_result_success) {
        result.error = "glTF references external buffers; use import_gltf_file";
        cgltf_free(parsed);
        return result;
    }
    if (cgltf_validate(parsed) != cgltf_result_success) {
        result.error = "glTF document failed validation";
        cgltf_free(parsed);
        return result;
    }
    result = convert_parsed(parsed, logical_path);
    cgltf_free(parsed);
    return result;
}

GltfImportResult import_gltf_file(const std::string& path) {
    GltfImportResult result;
    if (path.empty()) {
        result.error = "import_gltf_file: empty path";
        return result;
    }
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        result.error = std::string("cannot open glTF file: ") + path;
        return result;
    }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        std::fclose(f);
        result.error = std::string("glTF file is empty: ") + path;
        return result;
    }
    std::vector<unsigned char> bytes(static_cast<usize>(len));
    const usize got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) {
        result.error = std::string("short read on glTF file: ") + path;
        return result;
    }

    cgltf_options options{};
    cgltf_data* parsed = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &parsed) != cgltf_result_success ||
        !parsed) {
        result.error = std::string("not a valid glTF document: ") + path;
        return result;
    }
    if (cgltf_load_buffers(&options, parsed, path.c_str()) != cgltf_result_success) {
        result.error = std::string("cannot load glTF buffers for: ") + path;
        cgltf_free(parsed);
        return result;
    }
    if (cgltf_validate(parsed) != cgltf_result_success) {
        result.error = std::string("glTF document failed validation: ") + path;
        cgltf_free(parsed);
        return result;
    }
    result = convert_parsed(parsed, path);
    if (!result.ok) result.error += std::string(" [") + path + "]";
    cgltf_free(parsed);
    return result;
}

} // namespace nf::assets
