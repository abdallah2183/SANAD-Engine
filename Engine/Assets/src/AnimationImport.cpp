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
