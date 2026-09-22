// AssetTests — glTF character round trip (G2 acceptance).
//
// Synthesizes, fully in memory, the exact shape of document the shipped
// Blender add-on (Templates/Blender/nf_gltf_export.py) produces for a
// game-ready character: one skinned mesh (JOINTS_0 u16 + WEIGHTS_0 normalized
// u16), a 4-joint skin with inverse bind matrices, two sampled animation
// clips, and a Principled material. Asserts the skeleton, bindings, clips,
// and runtime playback through nf::animation — plus the loud-failure path
// (static documents report no skin, unsupported channels are counted, never
// silently substituted).
//
// Blender itself is not a test dependency: the fixture is the document the
// add-on emits (same attributes, component types, and sampling style).

#include <NF/Assets/AnimationImport.hpp>
#include <NF/Assets/GltfImport.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::assets;

namespace {

// ---------------------------------------------------------------------------
// Tiny glTF document builder (embedded base64 buffer)
// ---------------------------------------------------------------------------

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
        out.push_back(kDigits[(triple >> 6) & 63]);
        out.push_back(i + 2 < in.size() ? kDigits[triple & 63] : '=');
    }
    return out;
}

// Little-endian writers into a raw view segment (the buffer is x64-only, as
// is the whole engine).
void push_f32(u8* p, float v) {
    u32 u = 0;
    std::memcpy(&u, &v, 4);
    p[0] = static_cast<u8>(u & 0xFF);
    p[1] = static_cast<u8>((u >> 8) & 0xFF);
    p[2] = static_cast<u8>((u >> 16) & 0xFF);
    p[3] = static_cast<u8>((u >> 24) & 0xFF);
}
void push_u16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v & 0xFF);
    p[1] = static_cast<u8>((v >> 8) & 0xFF);
}

struct GltfDoc {
    std::vector<u8> buffer;
    std::string json;
    std::string views;
    std::string accessors;
    int next_view = 0;
    int next_acc = 0;
    bool first_view = true;
    bool first_acc = true;

    template <typename Push>
    int add_view(usize byte_length, Push push) {
        const usize offset = buffer.size();
        buffer.resize(offset + byte_length);
        // Views are appended 4-byte aligned; every segment below is a
        // multiple of 4, so this never pads.
        push(buffer.data() + offset);
        if (!first_view) views += ",";
        first_view = false;
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                 ",\"byteLength\":" + std::to_string(byte_length) + "}";
        return next_view++;
    }

    int add_accessor(int view, u32 component_type, u32 count, const char* type,
                     bool normalized = false) {
        if (!first_acc) accessors += ",";
        first_acc = false;
        accessors += "{\"bufferView\":" + std::to_string(view) +
                     ",\"componentType\":" + std::to_string(component_type) +
                     ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" +
                     (normalized ? ",\"normalized\":true" : "") + "}";
        return next_acc++;
    }
};

struct CharacterFixture {
    std::string json;

    // Skinned quad + 4-joint rig + 2 clips (+ one splined clip for the skip
    // counter) + 1 material. `with_weights=false` builds the malformed
    // JOINTS_0-only variant for the rejection test.
    static CharacterFixture make(bool with_weights = true, bool with_splined_clip = true) {
        GltfDoc d;

        // Mesh: 4 verts, 6 indices (two triangles).
        const int pos_view = d.add_view(48, [](u8* p) {
            const float v[12] = {-0.5f, 0, 0, 0.5f, 0, 0, 0.5f, 1, 0, -0.5f, 1, 0};
            for (int i = 0; i < 12; ++i) push_f32(p + i * 4, v[i]);
        });
        const int nrm_view = d.add_view(48, [](u8* p) {
            for (int i = 0; i < 12; ++i) push_f32(p + i * 4, i % 3 == 2 ? 1.0f : 0.0f);
        });
        const int joint_view = d.add_view(32, [](u8* p) {
            const u16 j[16] = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0};
            for (int i = 0; i < 16; ++i) push_u16(p + i * 2, j[i]);
        });
        int weight_view = -1;
        if (with_weights) {
            weight_view = d.add_view(32, [](u8* p) {
                // Blender-style: normalized u16. v0 = (1,0,0,0), v1..v3 split
                // evenly between two joints.
                const u16 w[16] = {65535, 0, 0, 0, 32768, 32768, 0, 0,
                                   32768, 0, 32768, 0, 0, 32768, 32768, 0};
                for (int i = 0; i < 16; ++i) push_u16(p + i * 2, w[i]);
            });
        }
        const int index_view = d.add_view(12, [](u8* p) {
            const u16 idx[6] = {0, 1, 2, 0, 2, 3};
            for (int i = 0; i < 6; ++i) push_u16(p + i * 2, idx[i]);
        });

        // Inverse bind matrices: identity for all 4 joints.
        const int ibm_view = d.add_view(256, [](u8* p) {
            for (int j = 0; j < 4; ++j) {
                const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                for (int i = 0; i < 16; ++i) push_f32(p + (j * 16 + i) * 4, m[i]);
            }
        });

        // Idle: Hips translation bob (3 keys) + Spine rotation (2 keys).
        const int idle_hips_t_view = d.add_view(12, [](u8* p) {
            push_f32(p, 0.0f);
            push_f32(p + 4, 0.5f);
            push_f32(p + 8, 1.0f);
        });
        const int idle_hips_v_view = d.add_view(36, [](u8* p) {
            const float v[9] = {0, 1, 0, 0, 1.1f, 0, 0, 1, 0};
            for (int i = 0; i < 9; ++i) push_f32(p + i * 4, v[i]);
        });
        const int idle_spine_t_view = d.add_view(8, [](u8* p) {
            push_f32(p, 0.0f);
            push_f32(p + 4, 1.0f);
        });
        const float k45z[4] = {0, 0, 0.70710678f, 0.70710678f};
        const int idle_spine_v_view = d.add_view(32, [&](u8* p) {
            const float q0[4] = {0, 0, 0, 1};
            for (int i = 0; i < 4; ++i) push_f32(p + i * 4, q0[i]);
            for (int i = 0; i < 4; ++i) push_f32(p + 16 + i * 4, k45z[i]);
        });

        // Wave: ArmL rotation (3 keys) + ArmR translation (1 key).
        const int wave_arml_t_view = d.add_view(12, [](u8* p) {
            push_f32(p, 0.0f);
            push_f32(p + 4, 0.5f);
            push_f32(p + 8, 1.0f);
        });
        const float k45z_half[4] = {0, 0, 0.38268343f, 0.92387953f};
        const int wave_arml_v_view = d.add_view(48, [&](u8* p) {
            const float q0[4] = {0, 0, 0, 1};
            for (int i = 0; i < 4; ++i) push_f32(p + i * 4, q0[i]);
            for (int i = 0; i < 4; ++i) push_f32(p + 16 + i * 4, k45z_half[i]);
            for (int i = 0; i < 4; ++i) push_f32(p + 32 + i * 4, q0[i]);
        });
        const int wave_armr_t_view = d.add_view(4, [](u8* p) { push_f32(p, 0.0f); });
        const int wave_armr_v_view = d.add_view(12, [](u8* p) {
            const float v[3] = {0, 0.4f, 0};
            for (int i = 0; i < 3; ++i) push_f32(p + i * 4, v[i]);
        });

        // Accessors.
        const int pos_acc = d.add_accessor(pos_view, 5126, 4, "VEC3");
        const int nrm_acc = d.add_accessor(nrm_view, 5126, 4, "VEC3");
        const int joint_acc = d.add_accessor(joint_view, 5123, 4, "VEC4");
        int weight_acc = -1;
        if (with_weights) weight_acc = d.add_accessor(weight_view, 5123, 4, "VEC4", true);
        const int index_acc = d.add_accessor(index_view, 5123, 6, "SCALAR");
        const int ibm_acc = d.add_accessor(ibm_view, 5126, 4, "MAT4");

        const int idle_hips_t_acc = d.add_accessor(idle_hips_t_view, 5126, 3, "SCALAR");
        const int idle_hips_v_acc = d.add_accessor(idle_hips_v_view, 5126, 3, "VEC3");
        const int idle_spine_t_acc = d.add_accessor(idle_spine_t_view, 5126, 2, "SCALAR");
        const int idle_spine_v_acc = d.add_accessor(idle_spine_v_view, 5126, 2, "VEC4");
        const int wave_arml_t_acc = d.add_accessor(wave_arml_t_view, 5126, 3, "SCALAR");
        const int wave_arml_v_acc = d.add_accessor(wave_arml_v_view, 5126, 3, "VEC4");
        const int wave_armr_t_acc = d.add_accessor(wave_armr_t_view, 5126, 1, "SCALAR");
        const int wave_armr_v_acc = d.add_accessor(wave_armr_v_view, 5126, 1, "VEC3");

        std::string attrs = "\"POSITION\":" + std::to_string(pos_acc) +
                            ",\"NORMAL\":" + std::to_string(nrm_acc) +
                            ",\"JOINTS_0\":" + std::to_string(joint_acc);
        if (with_weights) attrs += ",\"WEIGHTS_0\":" + std::to_string(weight_acc);

        std::string splined;
        if (with_splined_clip) {
            // A CUBICSPLINE sampler on Hips translation: unsupported, must be
            // counted in anim_channels_skipped, never dropped silently.
            const int spl_t_view = d.add_view(8, [](u8* p) {
                push_f32(p, 0.0f);
                push_f32(p + 4, 1.0f);
            });
            const int spl_v_view = d.add_view(72, [](u8* p) {
                for (int i = 0; i < 18; ++i) push_f32(p + i * 4, 0.0f);
            });
            const int spl_t_acc = d.add_accessor(spl_t_view, 5126, 2, "SCALAR");
            // glTF CUBICSPLINE stores in-tangent/value/out-tangent per key, so
            // the output accessor must hold 3 values per input key. cgltf's
            // validator enforces exactly that.
            const int spl_v_acc = d.add_accessor(spl_v_view, 5126, 6, "VEC3");
            // Views/accs were appended after the ones captured above — the
            // JSON arrays are strings, so ordering is already correct.
            splined =
                ",{\"name\":\"Splined\",\"samplers\":"
                "[{\"interpolation\":\"CUBICSPLINE\",\"input\":" +
                std::to_string(spl_t_acc) + ",\"output\":" + std::to_string(spl_v_acc) +
                "}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,"
                "\"path\":\"translation\"}}]}";
        }

        d.json =
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"buffers\":[{\"byteLength\":" + std::to_string(d.buffer.size()) +
            ",\"uri\":\"data:application/octet-stream;base64," +
            base64_encode(d.buffer) + "\"}],"
            "\"bufferViews\":[" + d.views + "],"
            "\"accessors\":[" + d.accessors + "],"
            "\"materials\":[{\"name\":\"HeroMat\",\"pbrMetallicRoughness\":{"
            "\"baseColorFactor\":[0.8,0.4,0.2,1.0],\"metallicFactor\":0.1,"
            "\"roughnessFactor\":0.7}}],"
            "\"meshes\":[{\"name\":\"HeroMesh\",\"primitives\":[{"
            "\"attributes\":{" + attrs + "},\"indices\":" + std::to_string(index_acc) +
            ",\"material\":0,\"mode\":4}]}],"
            "\"skins\":[{\"name\":\"HeroRig\",\"skeleton\":0,\"joints\":[0,1,2,3],"
            "\"inverseBindMatrices\":" + std::to_string(ibm_acc) + "}],"
            "\"nodes\":["
            "{\"name\":\"Hips\",\"translation\":[0,1,0],\"mesh\":0,\"skin\":0,\"children\":[1]},"
            "{\"name\":\"Spine\",\"translation\":[0,0.5,0],\"children\":[2,3]},"
            "{\"name\":\"ArmL\",\"translation\":[0.3,0.2,0]},"
            "{\"name\":\"ArmR\",\"translation\":[-0.3,0.2,0]}],"
            "\"animations\":["
            "{\"name\":\"Idle\",\"samplers\":["
            "{\"input\":" + std::to_string(idle_hips_t_acc) + ",\"output\":" +
            std::to_string(idle_hips_v_acc) + "},"
            "{\"input\":" + std::to_string(idle_spine_t_acc) + ",\"output\":" +
            std::to_string(idle_spine_v_acc) + "}],"
            "\"channels\":["
            "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
            "{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"rotation\"}}]},"
            "{\"name\":\"Wave\",\"samplers\":["
            "{\"input\":" + std::to_string(wave_arml_t_acc) + ",\"output\":" +
            std::to_string(wave_arml_v_acc) + "},"
            "{\"input\":" + std::to_string(wave_armr_t_acc) + ",\"output\":" +
            std::to_string(wave_armr_v_acc) + "}],"
            "\"channels\":["
            "{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}},"
            "{\"sampler\":1,\"target\":{\"node\":3,\"path\":\"translation\"}}]}" +
            splined + "],"
            "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
        return CharacterFixture{d.json};
    }
};

GltfImportResult import_fixture(const CharacterFixture& f) {
    GltfImportResult r = import_gltf_memory(f.json.data(), f.json.size(), "character.glb");
    // An import failure with no reason is the opacity this track exists to
    // remove; print it so a red run is diagnosable from the log alone.
    if (!r.ok) std::cout << "[fixture import failed] " << r.error << "\n";
    return r;
}

// 90 degrees about Z, xyzw.
bool quat_near(const nf::Quat& q, float z, float w, float tol) {
    return std::fabs(q.x) < tol && std::fabs(q.y) < tol &&
           std::fabs(q.z - z) < tol && std::fabs(q.w - w) < tol;
}

} // namespace

// ---------------------------------------------------------------------------
// The acceptance round trip
// ---------------------------------------------------------------------------

NF_TEST(gltf_character_imports_skin_clips_material) {
    GltfImportResult r = import_fixture(CharacterFixture::make());
    NF_CHECK(r.ok);
    NF_CHECK(r.error.empty());
    NF_CHECK(r.meshes.size() == 1);
    NF_CHECK(r.primitives_skipped == 0);

    // Skin: joints in rig order, root, identity inverse binds.
    NF_CHECK(r.skins.size() == 1);
    NF_CHECK(r.skins[0].name == "HeroRig");
    NF_CHECK(r.skins[0].joint_nodes.size() == 4);
    NF_CHECK(r.skins[0].joint_nodes[0] == 0 && r.skins[0].joint_nodes[3] == 3);
    NF_CHECK(r.skins[0].root_node == 0);
    NF_CHECK(r.skins[0].inverse_bind_matrices.size() == 64);
    NF_CHECK_NEAR(r.skins[0].inverse_bind_matrices[0], 1.0f, 1e-6f);
    NF_CHECK_NEAR(r.skins[0].inverse_bind_matrices[15], 1.0f, 1e-6f);

    // Node binding + per-vertex skin binding.
    NF_CHECK(r.nodes.size() == 4);
    NF_CHECK(r.nodes[0].name == "Hips");
    NF_CHECK(r.nodes[0].skin_index == 0);
    NF_CHECK(r.nodes[0].mesh_index == 0);
    NF_CHECK(r.mesh_skins.size() == 1);
    NF_CHECK(r.mesh_skins[0].skin_index == 0);
    NF_CHECK(r.mesh_skins[0].vertex_count == 4);
    NF_CHECK(r.mesh_skins[0].joints.size() == 16);
    NF_CHECK(r.mesh_skins[0].joints[0] == 0 && r.mesh_skins[0].joints[4] == 1);
    NF_CHECK(r.mesh_skins[0].joints[12] == 2);
    // Weights read from normalized u16 and renormalized to sum 1.
    const auto& w = r.mesh_skins[0].weights;
    NF_CHECK(w.size() == 16);
    NF_CHECK_NEAR(w[0], 1.0f, 1e-5f);
    NF_CHECK_NEAR(w[4], 0.5f, 1e-5f);
    NF_CHECK_NEAR(w[5], 0.5f, 1e-5f);
    NF_CHECK_NEAR(w[6] + w[7] + w[6] * 0.0f, 0.0f, 1e-5f); // v2 upper half zero
    NF_CHECK_NEAR(w[8] + w[10], 1.0f, 1e-5f);

    // Material.
    NF_CHECK(r.materials.size() == 1);
    NF_CHECK(r.materials[0].name == "HeroMat");
    NF_CHECK_NEAR(r.materials[0].base_color[0], 0.8f, 1e-6f);

    // Clips: two real ones + the splined one (imported, zero channels).
    NF_CHECK(r.animations.size() == 3);
    NF_CHECK(r.animations[0].name == "Idle");
    NF_CHECK_NEAR(r.animations[0].duration, 1.0f, 1e-6f);
    NF_CHECK(r.animations[0].channels.size() == 2);
    NF_CHECK(r.animations[1].name == "Wave");
    NF_CHECK(r.animations[1].channels.size() == 2);
    NF_CHECK(r.animations[2].name == "Splined");
    NF_CHECK(r.animations[2].channels.empty());
    NF_CHECK(r.anim_channels_skipped == 1); // counted, not silent
    NF_CHECK(r.skin_bindings_rejected == 0);
}

NF_TEST(gltf_character_skeleton_matches_rig) {
    GltfImportResult r = import_fixture(CharacterFixture::make());
    NF_CHECK(r.ok);

    nf::animation::Skeleton skel;
    std::string err;
    NF_CHECK(make_skeleton(r, 0, skel, err));
    NF_CHECK(skel.bone_count() == 4);

    NF_CHECK(skel.bones[0].name == "Hips");
    NF_CHECK(skel.bones[1].name == "Spine");
    NF_CHECK(skel.bones[2].name == "ArmL");
    NF_CHECK(skel.bones[3].name == "ArmR");
    NF_CHECK(skel.bones[0].parent == -1);
    NF_CHECK(skel.bones[1].parent == 0);
    NF_CHECK(skel.bones[2].parent == 1);
    NF_CHECK(skel.bones[3].parent == 1);

    // Rest pose = the joint nodes' local TRS.
    NF_CHECK_NEAR(skel.bones[0].rest_translation.y, 1.0f, 1e-6f);
    NF_CHECK_NEAR(skel.bones[1].rest_translation.y, 0.5f, 1e-6f);
    NF_CHECK_NEAR(skel.bones[2].rest_translation.x, 0.3f, 1e-6f);
    NF_CHECK_NEAR(skel.bones[3].rest_translation.x, -0.3f, 1e-6f);
    NF_CHECK(quat_near(skel.bones[0].rest_rotation, 0.0f, 1.0f, 1e-6f));
    NF_CHECK_NEAR(skel.bones[0].rest_scale.x, 1.0f, 1e-6f);

    // Node -> bone map covers the rig; other nodes (none here) would be -1.
    const std::vector<int> map = joint_node_to_bone(r, 0);
    NF_CHECK(map.size() == r.nodes.size());
    NF_CHECK(map[0] == 0 && map[1] == 1 && map[2] == 2 && map[3] == 3);

    // Out-of-range skin index fails loudly.
    nf::animation::Skeleton empty;
    NF_CHECK(!make_skeleton(r, 1, empty, err));
    NF_CHECK(!err.empty());
}

NF_TEST(gltf_character_clips_play_through_runtime_api) {
    GltfImportResult r = import_fixture(CharacterFixture::make());
    NF_CHECK(r.ok);

    nf::animation::Skeleton skel;
    std::string err;
    NF_CHECK(make_skeleton(r, 0, skel, err));
    const std::vector<int> map = joint_node_to_bone(r, 0);

    // Idle: Hips bob + Spine twist.
    nf::animation::AnimationClip idle;
    NF_CHECK(make_clip(r, r.animations[0], skel, map, idle, err));
    NF_CHECK(err.empty());
    NF_CHECK(idle.name == "Idle");
    NF_CHECK_NEAR(idle.duration, 1.0f, 1e-6f);
    NF_CHECK(idle.tracks.size() == 2);

    std::vector<nf::animation::LocalPose> pose;
    idle.sample(0.0f, skel, pose);
    NF_CHECK(pose.size() == 4);
    NF_CHECK_NEAR(pose[0].translation.y, 1.0f, 1e-5f); // rest at t=0
    idle.sample(0.5f, skel, pose);
    NF_CHECK_NEAR(pose[0].translation.y, 1.1f, 1e-5f); // bobbed
    // Spine: halfway between identity and 90 deg Z = 45 deg Z.
    NF_CHECK(quat_near(pose[1].rotation, 0.38268343f, 0.92387953f, 1e-4f));
    // Bones without curves keep their rest pose (partial-channel merge).
    idle.sample(0.5f, skel, pose);
    NF_CHECK_NEAR(pose[2].translation.x, 0.3f, 1e-5f);

    // Wave: ArmL swing + single-key ArmR translation.
    nf::animation::AnimationClip wave;
    NF_CHECK(make_clip(r, r.animations[1], skel, map, wave, err));
    NF_CHECK(wave.tracks.size() == 2);
    wave.sample(0.5f, skel, pose);
    NF_CHECK(quat_near(pose[2].rotation, 0.38268343f, 0.92387953f, 1e-4f));
    wave.sample(0.25f, skel, pose);
    // Halfway into the swing.
    NF_CHECK(quat_near(pose[2].rotation, 0.19509032f, 0.98078528f, 1e-4f));
    // ArmR has one translation key: value applies for the whole clip, and the
    // rotation stays at rest — a track never clobbers unanimated components.
    wave.sample(0.7f, skel, pose);
    NF_CHECK_NEAR(pose[3].translation.y, 0.4f, 1e-5f);
    NF_CHECK(quat_near(pose[3].rotation, 0.0f, 1.0f, 1e-5f));

    // The world-pose pipeline the runtime uses consumes these directly.
    // Mat4 is row-vector (v * M): the translation lives in row 3.
    std::vector<nf::animation::WorldPose> world;
    idle.sample(0.5f, skel, pose);
    nf::animation::compute_world_transforms(skel, pose, world);
    NF_CHECK(world.size() == 4);
    NF_CHECK_NEAR(world[0].transform.m[3][1], 1.1f, 1e-4f);
}

NF_TEST(gltf_static_document_reports_no_skin_no_substitution) {
    // A document with no skin and no animations must say so explicitly —
    // never a default skeleton, never a fake clip.
    GltfDoc d;
    const int pos_view = d.add_view(36, [](u8* p) {
        const float v[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        for (int i = 0; i < 9; ++i) push_f32(p + i * 4, v[i]);
    });
    const int idx_view = d.add_view(6, [](u8* p) {
        push_u16(p, 0);
        push_u16(p + 2, 1);
        push_u16(p + 4, 2);
    });
    const int pos_acc = d.add_accessor(pos_view, 5126, 3, "VEC3");
    const int idx_acc = d.add_accessor(idx_view, 5123, 3, "SCALAR");
    d.json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":" + std::to_string(d.buffer.size()) +
        ",\"uri\":\"data:application/octet-stream;base64," +
        base64_encode(d.buffer) + "\"}],"
        "\"bufferViews\":[" + d.views + "],"
        "\"accessors\":[" + d.accessors + "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" +
        std::to_string(pos_acc) + "},\"indices\":" + std::to_string(idx_acc) +
        ",\"mode\":4}]}],"
        "\"nodes\":[{\"name\":\"Prop\",\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";

    GltfImportResult r = import_gltf_memory(d.json.data(), d.json.size(), "prop.glb");
    NF_CHECK(r.ok);
    NF_CHECK(r.skins.empty());
    NF_CHECK(r.animations.empty());
    NF_CHECK(r.mesh_skins.size() == 1);
    NF_CHECK(r.mesh_skins[0].skin_index == -1);
    NF_CHECK(r.mesh_skins[0].joints.empty());
    NF_CHECK(r.mesh_skins[0].weights.empty());

    nf::animation::Skeleton skel;
    std::string err;
    NF_CHECK(!make_skeleton(r, 0, skel, err)); // no skin -> no skeleton, loudly
    NF_CHECK(!err.empty());
}

NF_TEST(gltf_character_malformed_weights_rejected_loudly) {
    // JOINTS_0 without WEIGHTS_0: the primitive is imported unskinned and the
    // rejection is counted — never half-bound, never silent.
    CharacterFixture f = CharacterFixture::make(/*with_weights=*/false, /*with_splined_clip=*/false);
    GltfImportResult r = import_fixture(f);
    NF_CHECK(r.ok);
    NF_CHECK(r.meshes.size() == 1);
    NF_CHECK(r.skin_bindings_rejected == 1);
    NF_CHECK(r.mesh_skins[0].skin_index == 0); // the node still binds the skin
    NF_CHECK(r.mesh_skins[0].vertex_count == 0);
    NF_CHECK(r.mesh_skins[0].joints.empty());
}

NF_TEST(gltf_clip_channels_outside_skin_fail_loudly) {
    GltfImportResult r = import_fixture(CharacterFixture::make());
    NF_CHECK(r.ok);
    nf::animation::Skeleton skel;
    std::string err;
    NF_CHECK(make_skeleton(r, 0, skel, err));

    // A map with no joints: every channel targets a non-joint node.
    const std::vector<int> broken_map(r.nodes.size(), -1);
    nf::animation::AnimationClip clip;
    NF_CHECK(!make_clip(r, r.animations[0], skel, broken_map, clip, err));
    NF_CHECK(err.find("Hips") != std::string::npos); // names the offending node
}
