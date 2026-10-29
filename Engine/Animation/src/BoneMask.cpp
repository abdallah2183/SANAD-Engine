#include <NF/Animation/BoneMask.hpp>
#include <algorithm>

namespace nf::animation {

namespace {

inline Vec3 lerp_vec3(const Vec3& a, const Vec3& b, f32 t) {
    return a * (1.0f - t) + b * t;
}

} // namespace

BoneMask::BoneMask(size_t bone_count, f32 default_weight) {
    resize(bone_count, default_weight);
}

void BoneMask::resize(size_t bone_count, f32 default_weight) {
    m_weights.resize(bone_count, std::clamp(default_weight, 0.0f, 1.0f));
}

void BoneMask::set_weight(int32_t bone_index, f32 weight) {
    if (bone_index >= 0 && static_cast<size_t>(bone_index) < m_weights.size()) {
        m_weights[static_cast<size_t>(bone_index)] = std::clamp(weight, 0.0f, 1.0f);
    }
}

f32 BoneMask::get_weight(int32_t bone_index) const {
    if (bone_index >= 0 && static_cast<size_t>(bone_index) < m_weights.size()) {
        return m_weights[static_cast<size_t>(bone_index)];
    }
    return 0.0f;
}

void BoneMask::set_weight_recursive(const Skeleton& skel, int32_t root_bone_index, f32 weight) {
    if (root_bone_index < 0 || static_cast<size_t>(root_bone_index) >= skel.bones.size()) {
        return;
    }

    if (m_weights.size() < skel.bones.size()) {
        resize(skel.bones.size(), 0.0f);
    }

    std::vector<bool> is_descendant(skel.bones.size(), false);
    is_descendant[static_cast<size_t>(root_bone_index)] = true;
    set_weight(root_bone_index, weight);

    for (size_t i = static_cast<size_t>(root_bone_index) + 1; i < skel.bones.size(); ++i) {
        int32_t p = skel.bones[i].parent;
        if (p >= 0 && static_cast<size_t>(p) < is_descendant.size() && is_descendant[static_cast<size_t>(p)]) {
            is_descendant[i] = true;
            set_weight(static_cast<int32_t>(i), weight);
        }
    }
}

void BoneMask::fill(f32 weight) {
    f32 clamped = std::clamp(weight, 0.0f, 1.0f);
    std::fill(m_weights.begin(), m_weights.end(), clamped);
}

void BoneMask::invert() {
    for (auto& w : m_weights) {
        w = 1.0f - w;
    }
}

void blend_layered(const std::vector<LocalPose>& base,
                   const std::vector<LocalPose>& overlay,
                   const BoneMask& mask,
                   std::vector<LocalPose>& out_pose) {
    size_t count = std::min(base.size(), overlay.size());
    out_pose.resize(count);

    for (size_t i = 0; i < count; ++i) {
        f32 w = mask.get_weight(static_cast<int32_t>(i));
        if (w <= 0.0001f) {
            out_pose[i] = base[i];
        } else if (w >= 0.9999f) {
            out_pose[i] = overlay[i];
        } else {
            out_pose[i].translation = lerp_vec3(base[i].translation, overlay[i].translation, w);
            out_pose[i].rotation = Quat::slerp(base[i].rotation, overlay[i].rotation, w);
            out_pose[i].scale = lerp_vec3(base[i].scale, overlay[i].scale, w);
        }
    }
}

} // namespace nf::animation
