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
// Skinned primitives fill `skin_out` (parallel to the mesh in the result);
// malformed JOINTS_0/WEIGHTS_0 pairs count in `skin_rejected` and leave the
// mesh static — never half-skinned.
bool convert_mesh(const cgltf_data* data, const cgltf_mesh& mesh, usize mesh_index,
                  const std::string& logical_path, MeshAsset& out, GltfMeshSkin& skin_out,
                  u32& skipped, u32& skin_rejected) {
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
        const cgltf_accessor* joints = nullptr;
        const cgltf_accessor* weights = nullptr;
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
                case cgltf_attribute_type_joints:
                    if (attr.index == 0) joints = attr.data;
                    break;
                case cgltf_attribute_type_weights:
                    if (attr.index == 0) weights = attr.data;
                    break;
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

        // Per-vertex skin binding: JOINTS_0 (raw u8/u16 joint indices) plus
        // WEIGHTS_0 (normalized u8/u16 or float — cgltf resolves both to
        // [0, 1] floats). Both must be VEC4 covering every vertex; anything
        // else rejects the binding loudly instead of mis-binding.
        if (joints || weights) {
            const bool shape_ok = joints && weights &&
                                  joints->type == cgltf_type_vec4 &&
                                  weights->type == cgltf_type_vec4 &&
                                  joints->count == pos->count &&
                                  weights->count == pos->count;
            if (!shape_ok || (skin_out.vertex_count != 0 &&
                              skin_out.vertex_count != base_vertex)) {
                ++skin_rejected;
            } else {
                bool bind_ok = true;
                std::vector<u16> j(static_cast<usize>(pos->count) * 4);
                std::vector<float> w(static_cast<usize>(pos->count) * 4);
                for (usize vi = 0; vi < pos->count && bind_ok; ++vi) {
                    float jr[4] = {0, 0, 0, 0};
                    float wr[4] = {0, 0, 0, 0};
                    if (!read_attrib_floats(joints, vi, jr, 4) ||
                        !read_attrib_floats(weights, vi, wr, 4)) {
                        bind_ok = false;
                        break;
                    }
                    float sum = 0.0f;
                    for (int c = 0; c < 4; ++c) {
                        if (jr[c] < 0.0f || jr[c] > 65535.0f ||
                            jr[c] != std::floor(jr[c])) {
                            bind_ok = false;
                            break;
                        }
                        sum += wr[c] > 0.0f ? wr[c] : 0.0f;
                    }
                    if (!bind_ok) break;
                    const float inv = sum > 1e-8f ? 1.0f / sum : 0.0f;
                    for (int c = 0; c < 4; ++c) {
                        j[vi * 4 + c] = static_cast<u16>(jr[c]);
                        w[vi * 4 + c] = (wr[c] > 0.0f ? wr[c] : 0.0f) * inv;
                    }
                }
                if (!bind_ok) {
                    ++skin_rejected;
                } else {
                    skin_out.vertex_count = static_cast<u32>(base_vertex + pos->count);
                    skin_out.joints.insert(skin_out.joints.end(), j.begin(), j.end());
                    skin_out.weights.insert(skin_out.weights.end(), w.begin(), w.end());
                }
            }
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

// Reads one animation sampler channel's input (times) and output (values).
// Returns false when the accessors are missing or empty.
bool read_channel_data(const cgltf_animation_sampler& sampler, GltfAnimationPath path,
                       std::vector<float>& out_times, std::vector<float>& out_values) {
    const usize comps = path == GltfAnimationPath::Rotation ? 4 : 3;
    const cgltf_accessor* in = sampler.input;
    const cgltf_accessor* out = sampler.output;
    if (!in || !out || in->count == 0 || out->count < in->count) return false;
    if (in->count > 100000000) return false;
    out_times.resize(in->count);
    out_values.resize(in->count * comps);
    for (usize i = 0; i < in->count; ++i) {
        float t = 0.0f;
        if (!cgltf_accessor_read_float(in, i, &t, 1)) return false;
        out_times[i] = t;
        float v[4] = {0, 0, 0, 0};
        if (!cgltf_accessor_read_float(out, i, v, comps)) return false;
        for (usize c = 0; c < comps; ++c) out_values[i * comps + c] = v[c];
    }
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
        GltfMeshSkin skin;
        if (convert_mesh(data, data->meshes[mi], mi, logical_path, *mesh, skin,
                         skipped, result.skin_bindings_rejected)) {
            result.meshes.push_back(std::move(mesh));
            result.mesh_skins.push_back(std::move(skin));
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
        if (n.skin) {
            info.skin_index = static_cast<int>(n.skin - data->skins);
        }
        if (n.parent) {
            info.parent_index = static_cast<int>(n.parent - data->nodes);
        }
        result.nodes.push_back(info);
    }

    // Skins: joints in glTF order, inverse bind matrices, and the root node —
    // from the file's `skeleton` when named, else the first joint whose parent
    // is outside the joint set, else -1 (reported, not guessed further).
    for (usize si = 0; si < data->skins_count; ++si) {
        const cgltf_skin& s = data->skins[si];
        GltfSkinInfo info;
        if (s.name) info.name = s.name;
        for (usize ji = 0; ji < s.joints_count; ++ji) {
            info.joint_nodes.push_back(static_cast<int>(s.joints[ji] - data->nodes));
        }
        if (s.skeleton) {
            info.root_node = static_cast<int>(s.skeleton - data->nodes);
        } else {
            for (int joint : info.joint_nodes) {
                if (joint < 0 || static_cast<usize>(joint) >= result.nodes.size()) continue;
                const int parent = result.nodes[static_cast<usize>(joint)].parent_index;
                bool parent_is_joint = false;
                for (int other : info.joint_nodes) {
                    if (other == parent) {
                        parent_is_joint = true;
                        break;
                    }
                }
                if (!parent_is_joint) {
                    info.root_node = joint;
                    break;
                }
            }
        }
        if (s.inverse_bind_matrices) {
            const cgltf_accessor* ibm = s.inverse_bind_matrices;
            if (ibm->count == s.joints_count && ibm->type == cgltf_type_mat4) {
                info.inverse_bind_matrices.resize(s.joints_count * 16);
                for (usize ji = 0; ji < s.joints_count; ++ji) {
                    cgltf_accessor_read_float(ibm, ji,
                                              info.inverse_bind_matrices.data() + ji * 16, 16);
                }
            }
        }
        result.skins.push_back(std::move(info));
    }

    // A mesh is skinned when the first node that references it binds a skin.
    // Static meshes keep skin_index == -1 — an explicit fact, never a
    // substituted default rig.
    for (usize mi = 0; mi < result.meshes.size(); ++mi) {
        for (const GltfNodeInfo& n : result.nodes) {
            if (n.mesh_index == static_cast<int>(mi) && n.skin_index >= 0) {
                result.mesh_skins[mi].skin_index = n.skin_index;
                break;
            }
        }
    }

    // Animations: LINEAR/STEP channels for translation/rotation/scale.
    // CUBICSPLINE and morph-weight channels are counted in
    // anim_channels_skipped — visible, never silently dropped.
    for (usize ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& a = data->animations[ai];
        GltfAnimationInfo info;
        if (a.name) info.name = a.name;
        for (usize ci = 0; ci < a.channels_count; ++ci) {
            const cgltf_animation_channel& ch = a.channels[ci];
            if (!ch.target_node || !ch.sampler || ch.target_path == cgltf_animation_path_type_weights) {
                ++result.anim_channels_skipped;
                continue;
            }
            if (ch.sampler->interpolation == cgltf_interpolation_type_cubic_spline) {
                ++result.anim_channels_skipped;
                continue;
            }
            GltfAnimationPath path;
            switch (ch.target_path) {
                case cgltf_animation_path_type_translation: path = GltfAnimationPath::Translation; break;
                case cgltf_animation_path_type_rotation: path = GltfAnimationPath::Rotation; break;
                case cgltf_animation_path_type_scale: path = GltfAnimationPath::Scale; break;
                default: ++result.anim_channels_skipped; continue;
            }
            GltfAnimationChannel channel;
            channel.node = static_cast<int>(ch.target_node - data->nodes);
            channel.path = path;
            if (!read_channel_data(*ch.sampler, path, channel.times, channel.values)) {
                ++result.anim_channels_skipped;
                continue;
            }
            for (float t : channel.times) {
                if (t > info.duration) info.duration = t;
            }
            info.channels.push_back(std::move(channel));
        }
        result.animations.push_back(std::move(info));
    }

    result.ok = true;
    NF_LOG_INFO(LogCategory::Asset,
                "GltfImport: '{}' -> {} mesh(es), {} material(s), {} node(s), {} skin(s), "
                "{} animation(s), {} skipped, {} anim channel(s) skipped",
                logical_path, result.meshes.size(), result.materials.size(),
                result.nodes.size(), result.skins.size(), result.animations.size(),
                skipped, result.anim_channels_skipped);
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
