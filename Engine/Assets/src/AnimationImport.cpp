// NF/Assets/AnimationImport.cpp — glTF skin/animation → runtime animation
// types. See the header for the contract.

#include <NF/Assets/AnimationImport.hpp>

#include <algorithm>
#include <cmath>

namespace nf::assets {

namespace {

// Value of one curve at `time`, clamped to its key range (glTF LINEAR/STEP
// behaviour for the endpoints; interior keys are linearly interpolated, which
// is exact when the query time hits a key).
float curve_scalar(const std::vector<float>& times, const std::vector<float>& values,
                   usize comps, usize comp, float time, float fallback) {
    if (times.empty()) return fallback;
    usize lo = 0;
    usize hi = times.size() - 1;
    if (time <= times[lo]) return values[lo * comps + comp];
    if (time >= times[hi]) return values[hi * comps + comp];
    while (lo + 1 < hi) {
        const usize mid = (lo + hi) / 2;
        if (times[mid] <= time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const float span = times[hi] - times[lo];
    const float t = span > 1e-8f ? (time - times[lo]) / span : 0.0f;
    const float a = values[lo * comps + comp];
    const float b = values[hi * comps + comp];
    return a + (b - a) * t;
}

nf::Quat curve_quat(const std::vector<float>& times, const std::vector<float>& values,
                    float time, const nf::Quat& fallback) {
    if (times.empty()) return fallback;
    usize lo = 0;
    usize hi = times.size() - 1;
    if (time <= times[lo]) {
        return nf::Quat{values[0], values[1], values[2], values[3]};
    }
    if (time >= times[hi]) {
        return nf::Quat{values[hi * 4], values[hi * 4 + 1], values[hi * 4 + 2],
                        values[hi * 4 + 3]};
    }
    while (lo + 1 < hi) {
        const usize mid = (lo + hi) / 2;
        if (times[mid] <= time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const nf::Quat a{values[lo * 4], values[lo * 4 + 1], values[lo * 4 + 2],
                     values[lo * 4 + 3]};
    const nf::Quat b{values[hi * 4], values[hi * 4 + 1], values[hi * 4 + 2],
                     values[hi * 4 + 3]};
    const float span = times[hi] - times[lo];
    const float t = span > 1e-8f ? (time - times[lo]) / span : 0.0f;
    return nf::Quat::slerp(a, b, t);
}

struct BoneCurve {
    std::vector<float> times;
    std::vector<float> values;
    usize comps = 3;
};

} // namespace

CharacterImportValidation validate_character_import(const GltfImportResult& result) {
    CharacterImportValidation validation;
    if (!result.ok) {
        validation.error = result.error.empty() ? "import failed" : result.error;
        return validation;
    }
    if (result.meshes.empty()) {
        validation.error = "character requires at least one triangle mesh";
        return validation;
    }
    if (result.primitives_skipped != 0) {
        validation.error = "character contains unusable/non-triangle primitives (" +
                           std::to_string(result.primitives_skipped) + " skipped)";
        return validation;
    }
    if (result.anim_channels_skipped != 0) {
        validation.error = "character contains unsupported animation channels (" +
                           std::to_string(result.anim_channels_skipped) + " skipped)";
        return validation;
    }
    if (result.skin_bindings_rejected != 0) {
        validation.error = "character has rejected skin bindings (" +
                           std::to_string(result.skin_bindings_rejected) + ")";
        return validation;
    }
    if (result.materials.empty()) {
        validation.error = "character requires material data";
        return validation;
    }
    for (const GltfMaterialInfo& material : result.materials) {
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(material.base_color[c])) {
                validation.error = "character material '" + material.name +
                                   "' has a non-finite base color";
                return validation;
            }
        }
        if (!std::isfinite(material.metallic) || !std::isfinite(material.roughness)) {
            validation.error = "character material '" + material.name +
                               "' has non-finite metallic/roughness";
            return validation;
        }
    }

    int selected_skin = -1;
    for (usize mi = 0; mi < result.mesh_skins.size(); ++mi) {
        const GltfMeshSkin& mesh_skin = result.mesh_skins[mi];
        if (mesh_skin.skin_index < 0) continue;
        if (mi >= result.meshes.size()) {
            validation.error = "mesh skin has no matching mesh";
            return validation;
        }
        const int skin_index = mesh_skin.skin_index;
        if (skin_index < 0 || static_cast<usize>(skin_index) >= result.skins.size()) {
            validation.error = "mesh references an out-of-range skin index";
            return validation;
        }
        if (selected_skin >= 0 && selected_skin != skin_index) {
            validation.error = "character meshes bind more than one skin";
            return validation;
        }
        selected_skin = skin_index;

        const GltfSkinInfo& skin = result.skins[static_cast<usize>(skin_index)];
        const usize vertex_count = result.meshes[mi]->vertices.size();
        if (skin.joint_nodes.empty() || mesh_skin.vertex_count != vertex_count ||
            mesh_skin.joints.size() != vertex_count * 4 ||
            mesh_skin.weights.size() != vertex_count * 4) {
            validation.error = "character skin binding is incomplete for mesh " +
                               std::to_string(mi);
            return validation;
        }
        for (usize v = 0; v < vertex_count; ++v) {
            float sum = 0.0f;
            for (usize influence = 0; influence < 4; ++influence) {
                const usize offset = v * 4 + influence;
                const float weight = mesh_skin.weights[offset];
                const u16 joint = mesh_skin.joints[offset];
                if (!std::isfinite(weight) || weight < 0.0f || joint >= skin.joint_nodes.size()) {
                    validation.error = "character skin binding has an invalid joint or weight";
                    return validation;
                }
                sum += weight;
            }
            if (!std::isfinite(sum) || std::fabs(sum - 1.0f) > 1e-4f) {
                validation.error = "character skin weights are not normalized";
                return validation;
            }
        }
    }
    if (selected_skin < 0) {
        validation.error = "character requires a valid bound skin";
        return validation;
    }

    nf::animation::Skeleton skeleton;
    std::string conversion_error;
    if (!make_skeleton(result, static_cast<usize>(selected_skin), skeleton, conversion_error)) {
        validation.error = conversion_error;
        return validation;
    }
    const std::vector<int> node_to_bone =
        joint_node_to_bone(result, static_cast<usize>(selected_skin));
    for (const GltfAnimationInfo& animation : result.animations) {
        if (animation.channels.empty()) {
            validation.error = "character animation '" + animation.name +
                               "' has no imported channels";
            return validation;
        }
        nf::animation::AnimationClip clip;
        if (!make_clip(result, animation, skeleton, node_to_bone, clip, conversion_error)) {
            validation.error = "character animation '" + animation.name +
                               "' cannot target the bound skin: " + conversion_error;
            return validation;
        }
        ++validation.nonempty_clips;
    }
    if (validation.nonempty_clips < 2) {
        validation.error = "character requires at least two nonempty animation clips (found " +
                           std::to_string(validation.nonempty_clips) + ")";
        return validation;
    }

    validation.skin_index = selected_skin;
    validation.ok = true;
    return validation;
}

std::vector<int> joint_node_to_bone(const GltfImportResult& result, usize skin_index) {
    std::vector<int> map;
    if (skin_index >= result.skins.size()) return map;
    map.assign(result.nodes.size(), -1);
    const GltfSkinInfo& skin = result.skins[skin_index];
    for (usize b = 0; b < skin.joint_nodes.size(); ++b) {
        const int node = skin.joint_nodes[b];
        if (node >= 0 && static_cast<usize>(node) < map.size()) {
            map[static_cast<usize>(node)] = static_cast<int>(b);
        }
    }
    return map;
}

bool make_skeleton(const GltfImportResult& result, usize skin_index,
                   nf::animation::Skeleton& out, std::string& out_error) {
    if (skin_index >= result.skins.size()) {
        out_error = "make_skeleton: skin index out of range";
        return false;
    }
    if (result.skins[skin_index].joint_nodes.empty()) {
        out_error = "make_skeleton: skin has no joints";
        return false;
    }
    const std::vector<int> bone_of_node = joint_node_to_bone(result, skin_index);
    const GltfSkinInfo& skin = result.skins[skin_index];

    out.bones.clear();
    out.bones.reserve(skin.joint_nodes.size());
    for (usize b = 0; b < skin.joint_nodes.size(); ++b) {
        const int node = skin.joint_nodes[b];
        if (node < 0 || static_cast<usize>(node) >= result.nodes.size()) {
            out_error = "make_skeleton: joint node index out of range (joint " +
                        std::to_string(b) + ")";
            return false;
        }
        const GltfNodeInfo& n = result.nodes[static_cast<usize>(node)];
        nf::animation::Bone bone;
        bone.name = n.name.empty() ? ("bone_" + std::to_string(b)) : n.name;
        const int parent_node = n.parent_index;
        if (parent_node >= 0 && static_cast<usize>(parent_node) < bone_of_node.size()) {
            bone.parent = bone_of_node[static_cast<usize>(parent_node)];
        } else {
            bone.parent = -1;
        }
        bone.rest_translation = nf::Vec3{n.translation[0], n.translation[1], n.translation[2]};
        bone.rest_rotation = nf::Quat{n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]};
        bone.rest_scale = nf::Vec3{n.scale[0], n.scale[1], n.scale[2]};
        // Hierarchical order is load-bearing (parents before children): a
        // joint whose parent is in the skin but appears later would break
        // compute_world_transforms. glTF skin joint lists from Blender are
        // in hierarchy order; anything else is rejected loudly.
        if (bone.parent > static_cast<int>(b)) {
            out_error = "make_skeleton: joint '" + bone.name +
                        "' appears before its parent in the skin joint list";
            return false;
        }
        out.bones.push_back(std::move(bone));
    }
    return true;
}

bool make_clip(const GltfImportResult& result, const GltfAnimationInfo& anim,
               const nf::animation::Skeleton& skeleton,
               const std::vector<int>& node_to_bone,
               nf::animation::AnimationClip& out, std::string& out_error) {
    if (skeleton.bones.empty()) {
        out_error = "make_clip: empty skeleton";
        return false;
    }
    out = nf::animation::AnimationClip{};
    out.name = anim.name;
    out.duration = anim.duration;
    out.looping = true;

    // Per-bone curves for each path component.
    std::vector<BoneCurve> curves(skeleton.bones.size() * 3);

    for (const GltfAnimationChannel& ch : anim.channels) {
        if (ch.node < 0 || static_cast<usize>(ch.node) >= node_to_bone.size()) {
            out_error = "make_clip: channel targets node index out of range";
            return false;
        }
        const int bone = node_to_bone[static_cast<usize>(ch.node)];
        if (bone < 0 || static_cast<usize>(bone) >= skeleton.bones.size()) {
            const std::string& node_name = (static_cast<usize>(ch.node) < result.nodes.size())
                                               ? result.nodes[static_cast<usize>(ch.node)].name
                                               : std::to_string(ch.node);
            out_error = "make_clip: channel targets node '" + node_name +
                        "' which is not a joint of the skin";
            return false;
        }
        BoneCurve& curve = curves[static_cast<usize>(bone) * 3 +
                                  static_cast<usize>(ch.path)];
        curve.times = ch.times;
        curve.values = ch.values;
        curve.comps = ch.path == GltfAnimationPath::Rotation ? 4 : 3;
    }

    for (usize bone = 0; bone < skeleton.bones.size(); ++bone) {
        const BoneCurve& tc = curves[bone * 3 + static_cast<usize>(GltfAnimationPath::Translation)];
        const BoneCurve& rc = curves[bone * 3 + static_cast<usize>(GltfAnimationPath::Rotation)];
        const BoneCurve& sc = curves[bone * 3 + static_cast<usize>(GltfAnimationPath::Scale)];
        if (tc.times.empty() && rc.times.empty() && sc.times.empty()) continue;

        // Union of the curves' key times, sorted + deduplicated (determinism
        // rule: sort before serializing/iterating).
        std::vector<float> times;
        times.reserve(tc.times.size() + rc.times.size() + sc.times.size());
        times.insert(times.end(), tc.times.begin(), tc.times.end());
        times.insert(times.end(), rc.times.begin(), rc.times.end());
        times.insert(times.end(), sc.times.begin(), sc.times.end());
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(),
                                [](float a, float b) { return std::fabs(a - b) < 1e-6f; }),
                    times.end());

        const nf::animation::Bone& rest = skeleton.bones[bone];
        nf::animation::AnimationTrack track;
        track.bone_index = static_cast<int>(bone);
        track.keyframes.reserve(times.size());
        for (float t : times) {
            nf::animation::Keyframe kf;
            kf.time = t;
            kf.translation = nf::Vec3{
                curve_scalar(tc.times, tc.values, 3, 0, t, rest.rest_translation.x),
                curve_scalar(tc.times, tc.values, 3, 1, t, rest.rest_translation.y),
                curve_scalar(tc.times, tc.values, 3, 2, t, rest.rest_translation.z),
            };
            kf.rotation = curve_quat(rc.times, rc.values, t, rest.rest_rotation);
            kf.scale = nf::Vec3{
                curve_scalar(sc.times, sc.values, 3, 0, t, rest.rest_scale.x),
                curve_scalar(sc.times, sc.values, 3, 1, t, rest.rest_scale.y),
                curve_scalar(sc.times, sc.values, 3, 2, t, rest.rest_scale.z),
            };
            track.keyframes.push_back(kf);
        }
        out.tracks.push_back(std::move(track));
    }

    if (out.duration <= 0.0f && !out.tracks.empty()) {
        out_error = "make_clip: animation '" + anim.name + "' has channels but zero duration";
        return false;
    }
    return true;
}

} // namespace nf::assets
