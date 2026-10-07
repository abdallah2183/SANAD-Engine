// NF/Assets/GltfImport.cpp — glTF 2.0 -> MeshAsset conversion.

#include <NF/Assets/GltfImport.hpp>
#include <NF/Core/Logger.hpp>

#include <cgltf.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace nf::assets {

namespace {

std::string lowercase_gltf_extension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool has_glb_magic(const std::vector<unsigned char>& bytes) {
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "glTF", 4) == 0;
}

bool has_gltf_json_prefix(const std::vector<unsigned char>& bytes) {
    usize i = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        i = 3;
    }
    while (i < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[i]))) ++i;
    return i < bytes.size() && bytes[i] == '{';
}

// --- image extraction helpers ----------------------------------------------

std::string lowercase_ascii(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

/// glTF uris are percent-encoded; a plain filename usually survives untouched,
/// but "Hero%20Base.png" would otherwise be looked for with the escape in it.
std::string percent_decode(const std::string& uri) {
    std::string out;
    out.reserve(uri.size());
    for (usize i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const auto hex_value = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex_value(uri[i + 1]);
            const int lo = hex_value(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(uri[i]);
    }
    return out;
}

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/// Decodes "data:image/png;base64,<payload>". Returns false for a data URI that
/// is not base64, or that carries no payload — the percent-encoded text form
/// exists in the spec but no image uses it.
bool decode_data_uri_image(const std::string& uri, std::vector<u8>& out) {
    const usize comma = uri.find(',');
    if (comma == std::string::npos) return false;
    if (lowercase_ascii(uri.substr(0, comma)).find(";base64") == std::string::npos) return false;

    out.clear();
    out.reserve((uri.size() - comma) / 4 * 3);
    u32 accumulator = 0;
    int bits = 0;
    for (usize i = comma + 1; i < uri.size(); ++i) {
        const char c = uri[i];
        if (c == '=') break; // padding: the payload is complete
        const int value = base64_value(c);
        if (value < 0) continue; // whitespace and newlines are legal padding
        accumulator = (accumulator << 6) | static_cast<u32>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((accumulator >> bits) & 0xFFu));
        }
    }
    return !out.empty();
}

/// Extension for a glTF mime type, or empty when it names something this engine
/// does not decode. Only a FALLBACK: the bytes are sniffed first.
std::string image_extension_for_mime(const char* mime) {
    if (mime == nullptr) return {};
    const std::string m = lowercase_ascii(mime);
    if (m == "image/png") return ".png";
    if (m == "image/jpeg" || m == "image/jpg") return ".jpg";
    if (m == "image/bmp") return ".bmp";
    if (m == "image/gif") return ".gif";
    if (m == "image/webp") return ".webp";
    if (m == "image/tga" || m == "image/x-tga") return ".tga";
    return {};
}

bool read_file_bytes_at(const std::string& path, std::vector<u8>& out, std::string& error) {
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        std::fclose(f);
        error = "empty file '" + path + "'";
        return false;
    }
    out.resize(static_cast<usize>(len));
    const usize got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        error = "short read on '" + path + "'";
        return false;
    }
    return true;
}

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

// Bounds and smooth normals used to be local here. They are now
// recompute_mesh_bounds() / compute_smooth_normals() in MeshImport.cpp, shared
// with the OBJ/STL/PLY readers so no two formats can disagree about what "the
// bounds" or "a smooth normal" means. Two things changed when the local copies
// went away, both fixes:
//   - submesh bounds are now filled in (they used to stay zero, while
//     MeshExport and Runtime::update already set them);
//   - the smooth-normal helper takes ABSOLUTE indices. The local version added
//     `base_vertex` to indices that already carried it (convert_mesh pushes
//     `base_vertex + idx`), so every primitive after the first computed its
//     normals from the wrong triangles.

// The importer associates a mesh with the first node that binds a skin. Find
// that skin's joint count before converting primitives so JOINTS_0 values can
// be checked against the real destination range, not just u16's storage range.
const cgltf_skin* bound_skin_for_mesh(const cgltf_data* data, const cgltf_mesh& mesh) {
    for (usize ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (node.mesh == &mesh && node.skin) {
            return node.skin;
        }
    }
    return nullptr;
}

// Converts one glTF mesh (all triangle primitives merged, one submesh each).
// Returns false when nothing convertible was found (caller counts the skip).
// Skin bindings are assembled transactionally for the whole mesh: if any
// convertible triangle primitive is unskinned or malformed, no partial binding
// is published and the rejection is counted.
bool convert_mesh(const cgltf_data* data, const cgltf_mesh& mesh, usize mesh_index,
                  const std::string& logical_path, MeshAsset& out, GltfMeshSkin& skin_out,
                  u32& skipped, u32& skin_rejected, const cgltf_skin* bound_skin) {
    out.logical_path = logical_path + "#mesh" + std::to_string(mesh_index);
    out.format_version = 1;

    std::vector<u16> mesh_joints;
    std::vector<float> mesh_weights;
    bool saw_skin_attributes = false;
    u32 malformed_skin_primitives = 0;
    u32 unskinned_primitives = 0;

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
        // [0, 1] floats). Stage this primitive's arrays; nothing is published
        // until the complete mesh has been validated below.
        std::vector<u16> primitive_joints;
        std::vector<float> primitive_weights;
        bool primitive_bind_ok = true;
        if (joints || weights) {
            const bool shape_ok = joints && weights &&
                                  joints->type == cgltf_type_vec4 &&
                                  weights->type == cgltf_type_vec4 &&
                                  joints->count == pos->count &&
                                  weights->count == pos->count;
            primitive_bind_ok = shape_ok;
            if (primitive_bind_ok) {
                primitive_joints.resize(static_cast<usize>(pos->count) * 4);
                primitive_weights.resize(static_cast<usize>(pos->count) * 4);
                for (usize vi = 0; vi < pos->count && primitive_bind_ok; ++vi) {
                    float jr[4] = {0, 0, 0, 0};
                    float wr[4] = {0, 0, 0, 0};
                    if (!read_attrib_floats(joints, vi, jr, 4) ||
                        !read_attrib_floats(weights, vi, wr, 4)) {
                        primitive_bind_ok = false;
                        break;
                    }
                    float sum = 0.0f;
                    for (int c = 0; c < 4; ++c) {
                        if (!std::isfinite(jr[c]) || !std::isfinite(wr[c]) ||
                            jr[c] < 0.0f || jr[c] > 65535.0f ||
                            jr[c] != std::floor(jr[c]) || wr[c] < 0.0f) {
                            primitive_bind_ok = false;
                            break;
                        }
                        if (bound_skin &&
                            static_cast<usize>(jr[c]) >= bound_skin->joints_count) {
                            primitive_bind_ok = false;
                            break;
                        }
                        sum += wr[c];
                    }
                    if (!primitive_bind_ok || !std::isfinite(sum) || sum <= 1e-8f) {
                        primitive_bind_ok = false;
                        break;
                    }
                    const float inv = 1.0f / sum;
                    for (int c = 0; c < 4; ++c) {
                        primitive_joints[vi * 4 + c] = static_cast<u16>(jr[c]);
                        primitive_weights[vi * 4 + c] = wr[c] * inv;
                    }
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
            // Absolute indices, matching how convert_mesh stores them.
            compute_smooth_normals(out.vertices, out.indices, base_vertex, base_index,
                                   prim_index_count);
        }

        if (joints || weights) {
            saw_skin_attributes = true;
            if (primitive_bind_ok) {
                mesh_joints.insert(mesh_joints.end(), primitive_joints.begin(),
                                   primitive_joints.end());
                mesh_weights.insert(mesh_weights.end(), primitive_weights.begin(),
                                    primitive_weights.end());
            } else {
                ++malformed_skin_primitives;
            }
        } else {
            ++unskinned_primitives;
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
    if (saw_skin_attributes) {
        const u32 rejected = malformed_skin_primitives + unskinned_primitives;
        if (rejected == 0 && mesh_joints.size() == out.vertices.size() * 4 &&
            mesh_weights.size() == out.vertices.size() * 4) {
            skin_out.vertex_count = static_cast<u32>(out.vertices.size());
            skin_out.joints = std::move(mesh_joints);
            skin_out.weights = std::move(mesh_weights);
        } else {
            skin_rejected += rejected != 0 ? rejected : 1u;
        }
    }
    if (mesh.name) out.logical_path = std::string(logical_path) + "#" + mesh.name;
    recompute_mesh_bounds(out);
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

    // --- images, before materials -------------------------------------------
    // A material's `albedo_image` indexes this list, so the list has to exist
    // first. Images are kept in their ORIGINAL encoded bytes: re-encoding would
    // lose data, and the bytes are what a texture asset needs on disk.
    std::vector<int> image_index_of(data->images_count, -1);
    const std::filesystem::path base_dir = std::filesystem::path(logical_path).parent_path();
    for (usize ii = 0; ii < data->images_count; ++ii) {
        const cgltf_image& img = data->images[ii];
        MeshImportImage out;
        out.name = img.name ? img.name : ("image" + std::to_string(ii));
        bool extracted = false;

        if (img.buffer_view != nullptr && img.buffer_view->buffer != nullptr &&
            img.buffer_view->buffer->data != nullptr) {
            // Embedded: a GLB BIN chunk, or a base64 buffer the loader resolved.
            const cgltf_buffer* buffer = img.buffer_view->buffer;
            const usize offset = img.buffer_view->offset;
            const usize length = img.buffer_view->size;
            if (offset <= buffer->size && length <= buffer->size - offset) {
                const u8* base = static_cast<const u8*>(buffer->data);
                out.bytes.assign(base + offset, base + offset + length);
                out.source = "bufferView " + std::to_string(ii);
                extracted = !out.bytes.empty();
            }
        } else if (img.uri != nullptr) {
            const std::string uri = percent_decode(img.uri);
            const std::string lower = lowercase_ascii(uri);
            if (lower.rfind("data:", 0) == 0) {
                // A self-contained .gltf may inline the image as a data URI.
                if (!decode_data_uri_image(uri, out.bytes)) out.bytes.clear();
                out.source = "data URI";
                extracted = !out.bytes.empty();
            } else {
                // External file beside the .gltf/.glb. Only the file-based entry
                // point can resolve this; a memory import reports the loss.
                std::vector<u8> file_bytes;
                std::string read_error;
                const std::string file_path = (base_dir / uri).string();
                if (read_file_bytes_at(file_path, file_bytes, read_error)) {
                    out.bytes = std::move(file_bytes);
                    out.source = "uri " + uri;
                    extracted = !out.bytes.empty();
                }
            }
        }

        if (!extracted) {
            ++result.images_skipped;
            result.warnings.push_back("image '" + out.name +
                                      "' could not be read out of the source and was skipped");
            continue;
        }
        const char* sniffed = image_extension_for_bytes(out.bytes);
        std::string fallback;
        if (img.uri != nullptr) {
            fallback = lowercase_ascii(std::filesystem::path(percent_decode(img.uri)).extension().string());
        } else if (img.mime_type != nullptr) {
            fallback = image_extension_for_mime(img.mime_type);
        }
        out.extension = sniffed[0] != '\0' ? std::string(sniffed) : fallback;
        image_index_of[ii] = static_cast<int>(result.images.size());
        result.images.push_back(std::move(out));
    }

    for (usize mi = 0; mi < data->materials_count; ++mi) {
        const cgltf_material& m = data->materials[mi];
        GltfMaterialInfo info;
        if (m.name) info.name = m.name;
        // One image-index resolver for every map slot below: a texture's
        // cgltf image pointer becomes this result's image index, or the slot
        // stays -1. Shared because the translation (and its guards) is
        // identical for all five slots.
        auto carry_map = [&](const cgltf_texture* tex, int& slot) {
            if (tex == nullptr || tex->image == nullptr) return;
            const usize image_index = static_cast<usize>(tex->image - data->images);
            if (image_index < image_index_of.size() && image_index_of[image_index] >= 0) {
                slot = image_index_of[image_index];
            }
        };
        if (m.has_pbr_metallic_roughness) {
            for (int c = 0; c < 4; ++c) {
                info.base_color[c] = m.pbr_metallic_roughness.base_color_factor[c];
            }
            info.metallic = m.pbr_metallic_roughness.metallic_factor;
            info.roughness = m.pbr_metallic_roughness.roughness_factor;

            carry_map(m.pbr_metallic_roughness.base_color_texture.texture,
                      info.albedo_image);
            carry_map(m.pbr_metallic_roughness.metallic_roughness_texture.texture,
                      info.metallic_roughness_image);
        }
        // Material-level slots, independent of the PBR block: a material can
        // legally carry a normal map without has_pbr_metallic_roughness, so
        // these resolve outside the if above.
        carry_map(m.normal_texture.texture, info.normal_image);
        carry_map(m.occlusion_texture.texture, info.occlusion_image);
        carry_map(m.emissive_texture.texture, info.emissive_image);
        for (int c = 0; c < 3; ++c) {
            info.emissive[c] = m.emissive_factor[c];
        }
        if (m.has_emissive_strength) {
            info.emissive_strength = m.emissive_strength.emissive_strength;
        }

        // Everything else the material declares, named so the loss is visible
        // instead of implied. PBR maps are carried above (not dropped); what
        // remains here genuinely has no slot in this pipeline.
        if (m.has_pbr_specular_glossiness) {
            info.dropped.push_back("specular_glossiness (no equivalent in this pipeline)");
        }
        if (m.has_clearcoat) info.dropped.push_back("clearcoat (no equivalent in this pipeline)");
        if (m.has_transmission) info.dropped.push_back("transmission (no equivalent in this pipeline)");
        if (m.has_sheen) info.dropped.push_back("sheen (no equivalent in this pipeline)");
        if (m.has_iridescence) info.dropped.push_back("iridescence (no equivalent in this pipeline)");
        if (m.alpha_mode != cgltf_alpha_mode_opaque) {
            info.dropped.push_back(m.alpha_mode == cgltf_alpha_mode_mask
                                       ? "alpha mode MASK (imported as blended alpha)"
                                       : "alpha mode BLEND (imported as blended alpha)");
        }
        result.materials.push_back(std::move(info));
    }

    for (usize mi = 0; mi < data->meshes_count; ++mi) {
        auto mesh = std::make_unique<MeshAsset>();
        GltfMeshSkin skin;
        if (convert_mesh(data, data->meshes[mi], mi, logical_path, *mesh, skin,
                         skipped, result.skin_bindings_rejected,
                         bound_skin_for_mesh(data, data->meshes[mi]))) {
            result.meshes.push_back(std::move(mesh));
            result.mesh_skins.push_back(std::move(skin));
            // The name the source gave this mesh, so an importer that has to
            // invent a file name can prefer the author's word over an index.
            result.mesh_names.push_back(data->meshes[mi].name
                                            ? std::string(data->meshes[mi].name)
                                            : "mesh" + std::to_string(mi));
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

    // Animations: LINEAR channels for translation/rotation/scale. STEP and
    // CUBICSPLINE are explicitly rejected and counted: converting STEP to the
    // engine's linear/slerp sampler would silently change the motion.
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
            if (ch.sampler->interpolation != cgltf_interpolation_type_linear) {
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
    // The bytes decide, not the caller: a memory import that holds a GLB
    // container says so, so a report can name the container it actually read.
    // Checked straight off the pointer — copying the buffer to look at four
    // bytes would double a model's peak memory for nothing.
    const bool glb_container = size >= 4 && std::memcmp(data, "glTF", 4) == 0;
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
    // Set AFTER the conversion: convert_parsed() returns a whole fresh result,
    // so anything written before this line would be overwritten by it.
    result.format = glb_container ? MeshImportFormat::Glb : MeshImportFormat::Gltf;
    cgltf_free(parsed);
    return result;
}

GltfImportResult import_gltf_file(const std::string& path) {
    GltfImportResult result;
    if (path.empty()) {
        result.error = "import_gltf_file: empty path";
        return result;
    }
    const std::string extension = lowercase_gltf_extension(path);
    if (extension != ".gltf" && extension != ".glb") {
        // Still refused here — this entry point reads glTF and nothing else.
        // The other formats have their own readers; import_mesh_file() (see
        // MeshImport.hpp) dispatches to all of them, and is what a caller who
        // does not know the format ahead of time should use.
        result.error = "unsupported model extension '" + extension +
                       "' (this entry point reads .gltf/.glb only; use import_mesh_file for "
                       "OBJ/STL/PLY/NFMesh)";
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

    const bool glb_content = has_glb_magic(bytes);
    const bool gltf_json_content = has_gltf_json_prefix(bytes);
    if ((extension == ".glb" && !glb_content) ||
        (extension == ".gltf" && !gltf_json_content)) {
        result.error = "extension/content mismatch for '" + path + "': expected " +
                       extension + " but detected " +
                       (glb_content ? "glTF binary (GLB)" :
                        gltf_json_content ? "glTF JSON" : "neither glTF JSON nor GLB");
        return result;
    }
    // Which container the bytes actually are. Assigned to `result` only AFTER
    // convert_parsed(), because that call returns a whole fresh result and
    // would overwrite anything set here.
    const bool glb_container = glb_content;

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
    result.format = glb_container ? MeshImportFormat::Glb : MeshImportFormat::Gltf;
    cgltf_free(parsed);
    return result;
}

} // namespace nf::assets
