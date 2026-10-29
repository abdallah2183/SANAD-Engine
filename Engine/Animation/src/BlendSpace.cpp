#include <NF/Animation/BlendSpace.hpp>
#include <algorithm>
#include <cmath>

namespace nf::animation {

void BlendSpace1D::add_sample(const std::string& clip_name, f32 position) {
    m_samples.push_back({clip_name, position});
    std::sort(m_samples.begin(), m_samples.end(), [](const BlendSample1D& a, const BlendSample1D& b) {
        return a.position < b.position;
    });
}

void BlendSpace1D::get_blend_weights(f32 param, std::vector<std::pair<size_t, f32>>& out_weights) const {
    out_weights.clear();
    if (m_samples.empty()) {
        return;
    }

    if (m_samples.size() == 1 || param <= m_samples.front().position) {
        out_weights.push_back({0, 1.0f});
        return;
    }

    if (param >= m_samples.back().position) {
        out_weights.push_back({m_samples.size() - 1, 1.0f});
        return;
    }

    for (size_t i = 0; i + 1 < m_samples.size(); ++i) {
        if (param >= m_samples[i].position && param <= m_samples[i + 1].position) {
            f32 len = m_samples[i + 1].position - m_samples[i].position;
            f32 t = (len > 0.0001f) ? (param - m_samples[i].position) / len : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            if (1.0f - t > 0.0001f) {
                out_weights.push_back({i, 1.0f - t});
            }
            if (t > 0.0001f) {
                out_weights.push_back({i + 1, t});
            }
            return;
        }
    }
}

f32 BlendSpace1D::get_synced_duration(f32 param, const std::unordered_map<std::string, const AnimationClip*>& clips) const {
    std::vector<std::pair<size_t, f32>> weights;
    get_blend_weights(param, weights);
    if (weights.empty()) {
        return 1.0f;
    }

    f32 blended_duration = 0.0f;
    f32 total_weight = 0.0f;
    for (const auto& [idx, w] : weights) {
        auto it = clips.find(m_samples[idx].clip_name);
        if (it != clips.end() && it->second) {
            blended_duration += it->second->duration * w;
            total_weight += w;
        }
    }

    return (total_weight > 0.0001f) ? (blended_duration / total_weight) : 1.0f;
}

void BlendSpace1D::evaluate(f32 param,
                           f32 normalized_time,
                           const Skeleton& skel,
                           const std::unordered_map<std::string, const AnimationClip*>& clips,
                           std::vector<LocalPose>& out_pose) const {
    std::vector<std::pair<size_t, f32>> weights;
    get_blend_weights(param, weights);

    if (weights.empty()) {
        out_pose.resize(skel.bone_count());
        for (size_t i = 0; i < skel.bone_count(); ++i) {
            out_pose[i].translation = skel.bones[i].rest_translation;
            out_pose[i].rotation = skel.bones[i].rest_rotation;
            out_pose[i].scale = skel.bones[i].rest_scale;
        }
        return;
    }

    std::vector<std::vector<LocalPose>> sampled_poses;
    std::vector<const std::vector<LocalPose>*> pose_ptrs;
    std::vector<f32> blend_w;

    sampled_poses.resize(weights.size());
    for (size_t i = 0; i < weights.size(); ++i) {
        size_t sample_idx = weights[i].first;
        f32 w = weights[i].second;

        auto it = clips.find(m_samples[sample_idx].clip_name);
        if (it != clips.end() && it->second) {
            const AnimationClip* clip = it->second;
            f32 t = normalized_time * clip->duration;
            clip->sample(t, skel, sampled_poses[i]);
            pose_ptrs.push_back(&sampled_poses[i]);
            blend_w.push_back(w);
        }
    }

    if (!pose_ptrs.empty()) {
        blend_poses(pose_ptrs, blend_w, out_pose);
    } else {
        out_pose.resize(skel.bone_count());
        for (size_t i = 0; i < skel.bone_count(); ++i) {
            out_pose[i].translation = skel.bones[i].rest_translation;
            out_pose[i].rotation = skel.bones[i].rest_rotation;
            out_pose[i].scale = skel.bones[i].rest_scale;
        }
    }
}

void BlendSpace2D::add_sample(const std::string& clip_name, const Vec2& position) {
    m_samples.push_back({clip_name, position});
}

void BlendSpace2D::get_blend_weights(const Vec2& param, std::vector<std::pair<size_t, f32>>& out_weights) const {
    out_weights.clear();
    if (m_samples.empty()) {
        return;
    }

    if (m_samples.size() == 1) {
        out_weights.push_back({0, 1.0f});
        return;
    }

    // Check for exact / near-exact match
    for (size_t i = 0; i < m_samples.size(); ++i) {
        f32 dx = param.x - m_samples[i].position.x;
        f32 dy = param.y - m_samples[i].position.y;
        f32 dist_sq = dx * dx + dy * dy;
        if (dist_sq < 0.00001f) {
            out_weights.push_back({i, 1.0f});
            return;
        }
    }

    // Inverse Distance Weighting
    std::vector<f32> raw_weights(m_samples.size(), 0.0f);
    f32 sum_weights = 0.0f;

    for (size_t i = 0; i < m_samples.size(); ++i) {
        f32 dx = param.x - m_samples[i].position.x;
        f32 dy = param.y - m_samples[i].position.y;
        f32 dist_sq = dx * dx + dy * dy;
        f32 w = 1.0f / dist_sq;
        raw_weights[i] = w;
        sum_weights += w;
    }

    if (sum_weights > 0.00001f) {
        f32 inv_sum = 1.0f / sum_weights;
        for (size_t i = 0; i < m_samples.size(); ++i) {
            f32 norm_w = raw_weights[i] * inv_sum;
            if (norm_w > 0.01f) {
                out_weights.push_back({i, norm_w});
            }
        }

        // Re-normalize active weights
        f32 active_sum = 0.0f;
        for (const auto& p : out_weights) {
            active_sum += p.second;
        }
        if (active_sum > 0.0001f) {
            for (auto& p : out_weights) {
                p.second /= active_sum;
            }
        }
    } else {
        out_weights.push_back({0, 1.0f});
    }
}

f32 BlendSpace2D::get_synced_duration(const Vec2& param, const std::unordered_map<std::string, const AnimationClip*>& clips) const {
    std::vector<std::pair<size_t, f32>> weights;
    get_blend_weights(param, weights);
    if (weights.empty()) {
        return 1.0f;
    }

    f32 blended_duration = 0.0f;
    f32 total_weight = 0.0f;
    for (const auto& [idx, w] : weights) {
        auto it = clips.find(m_samples[idx].clip_name);
        if (it != clips.end() && it->second) {
            blended_duration += it->second->duration * w;
            total_weight += w;
        }
    }

    return (total_weight > 0.0001f) ? (blended_duration / total_weight) : 1.0f;
}

void BlendSpace2D::evaluate(const Vec2& param,
                           f32 normalized_time,
                           const Skeleton& skel,
                           const std::unordered_map<std::string, const AnimationClip*>& clips,
                           std::vector<LocalPose>& out_pose) const {
    std::vector<std::pair<size_t, f32>> weights;
    get_blend_weights(param, weights);

    if (weights.empty()) {
        out_pose.resize(skel.bone_count());
        for (size_t i = 0; i < skel.bone_count(); ++i) {
            out_pose[i].translation = skel.bones[i].rest_translation;
            out_pose[i].rotation = skel.bones[i].rest_rotation;
            out_pose[i].scale = skel.bones[i].rest_scale;
        }
        return;
    }

    std::vector<std::vector<LocalPose>> sampled_poses;
    std::vector<const std::vector<LocalPose>*> pose_ptrs;
    std::vector<f32> blend_w;

    sampled_poses.resize(weights.size());
    for (size_t i = 0; i < weights.size(); ++i) {
        size_t sample_idx = weights[i].first;
        f32 w = weights[i].second;

        auto it = clips.find(m_samples[sample_idx].clip_name);
        if (it != clips.end() && it->second) {
            const AnimationClip* clip = it->second;
            f32 t = normalized_time * clip->duration;
            clip->sample(t, skel, sampled_poses[i]);
            pose_ptrs.push_back(&sampled_poses[i]);
            blend_w.push_back(w);
        }
    }

    if (!pose_ptrs.empty()) {
        blend_poses(pose_ptrs, blend_w, out_pose);
    } else {
        out_pose.resize(skel.bone_count());
        for (size_t i = 0; i < skel.bone_count(); ++i) {
            out_pose[i].translation = skel.bones[i].rest_translation;
            out_pose[i].rotation = skel.bones[i].rest_rotation;
            out_pose[i].scale = skel.bones[i].rest_scale;
        }
    }
}

} // namespace nf::animation
