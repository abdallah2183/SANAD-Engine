// NF/Animation/Retarget.cpp — name-mapped pose retargeting.

#include <NF/Animation/Retarget.hpp>

#include <cmath>

namespace nf::animation {

usize RetargetMap::matched_bone_count() const {
    usize n = 0;
    for (const int32_t s : source_of) {
        if (s >= 0) ++n;
    }
    return n;
}

RetargetMap build_retarget_map(const Skeleton& source, const Skeleton& target) {
    RetargetMap map;
    map.source_of.resize(target.bones.size(), -1);

    f32 ratio_sum = 0.0f;
    i32 ratio_count = 0;

    for (usize t = 0; t < target.bones.size(); ++t) {
        const int32_t s = source.find_bone(target.bones[t].name);
        if (s < 0) {
            continue;
        }
        map.source_of[t] = s;

        if (target.bones[t].parent < 0) {
            continue; // root length is not a rig proportion
        }
        const f32 src_len = source.bones[static_cast<usize>(s)].rest_translation.length();
        const f32 tgt_len = target.bones[t].rest_translation.length();
        if (src_len > 1e-6f) {
            ratio_sum += tgt_len / src_len;
            ++ratio_count;
        }
    }

    map.translation_scale = (ratio_count > 0) ? ratio_sum / static_cast<f32>(ratio_count) : 1.0f;
    return map;
}

void retarget_pose(const Skeleton& source,
                   const std::vector<LocalPose>& source_pose,
                   const RetargetMap& map,
                   const Skeleton& target,
                   std::vector<LocalPose>& out_target_pose) {
    const usize n = target.bones.size();
    out_target_pose.resize(n);

    // Unmatched bones (and any bone below a matched-but-missing-pose index)
    // start from rest; matched bones overwrite below.
    for (usize i = 0; i < n; ++i) {
        out_target_pose[i].translation = target.bones[i].rest_translation;
        out_target_pose[i].rotation = target.bones[i].rest_rotation;
        out_target_pose[i].scale = target.bones[i].rest_scale;
    }

    for (usize t = 0; t < n; ++t) {
        const int32_t s_idx = (t < map.source_of.size()) ? map.source_of[t] : -1;
        if (s_idx < 0 || static_cast<usize>(s_idx) >= source_pose.size()) {
            continue;
        }
        const usize s = static_cast<usize>(s_idx);
        const Bone& src_bone = source.bones[s];
        const Bone& tgt_bone = target.bones[t];
        const LocalPose& sp = source_pose[s];
        LocalPose& out = out_target_pose[t];

        // Rotation: the source's rest-relative offset reapplied on the
        // target's rest rotation.
        const Quat offset = src_bone.rest_rotation.inverse() * sp.rotation;
        out.rotation = (tgt_bone.rest_rotation * offset).normalized();

        if (tgt_bone.parent < 0) {
            // Root: transfer the full (entity-space) translation, scaled.
            out.translation = sp.translation * map.translation_scale;
        } else {
            // Non-root: keep the target's bone length, transfer the source's
            // animated stretch/compression as a ratio along the target bone.
            const f32 src_rest_len = src_bone.rest_translation.length();
            const f32 ratio = (src_rest_len > 1e-6f)
                                  ? sp.translation.length() / src_rest_len
                                  : 1.0f;
            out.translation = tgt_bone.rest_translation * ratio;
        }
    }
}

} // namespace nf::animation
