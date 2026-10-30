#include <NF/Animation/RagdollBlend.hpp>

#include <algorithm>
#include <cmath>

namespace nf::animation {

namespace {

Vec3 lerp_vec3(const Vec3& a, const Vec3& b, f32 t) {
    return Vec3{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

f32 smoothstep(f32 edge0, f32 edge1, f32 x) {
    f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

RagdollBlendController::RagdollBlendController(usize bone_count) {
    resize(bone_count);
}

void RagdollBlendController::resize(usize bone_count) {
    m_ragdoll_pose.resize(bone_count);
    m_animated_pose.resize(bone_count);
    m_bone_weights.assign(bone_count, 1.0f);
}

void RagdollBlendController::set_bone_weight(usize bone_idx, f32 weight) {
    if (bone_idx < m_bone_weights.size()) {
        m_bone_weights[bone_idx] = std::clamp(weight, 0.0f, 1.0f);
    }
}

f32 RagdollBlendController::bone_weight(usize bone_idx) const {
    if (bone_idx < m_bone_weights.size()) {
        return m_bone_weights[bone_idx];
    }
    return 1.0f;
}

void RagdollBlendController::start_ragdoll(f32 transition_duration) {
    f32 dur = (transition_duration >= 0.0f) ? transition_duration : m_config.blend_to_ragdoll_time;
    if (dur <= 1e-4f) {
        m_state = RagdollState::Ragdoll;
        m_global_weight = 1.0f;
        m_transition_time = 0.0f;
        m_transition_duration = 0.0f;
    } else {
        m_state = RagdollState::BlendingToRagdoll;
        m_transition_time = 0.0f;
        m_transition_duration = dur;
    }
}

void RagdollBlendController::start_get_up(f32 transition_duration) {
    f32 dur = (transition_duration >= 0.0f) ? transition_duration : m_config.blend_to_anim_time;
    if (dur <= 1e-4f) {
        m_state = RagdollState::Kinematic;
        m_global_weight = 0.0f;
        m_transition_time = 0.0f;
        m_transition_duration = 0.0f;
    } else {
        m_state = RagdollState::BlendingToAnimation;
        m_transition_time = 0.0f;
        m_transition_duration = dur;
    }
}

void RagdollBlendController::set_kinematic() noexcept {
    m_state = RagdollState::Kinematic;
    m_global_weight = 0.0f;
    m_transition_time = 0.0f;
    m_transition_duration = 0.0f;
}

void RagdollBlendController::set_ragdoll_pose(std::span<const LocalPose> poses) {
    if (m_ragdoll_pose.size() != poses.size()) {
        m_ragdoll_pose.resize(poses.size());
        if (m_bone_weights.size() != poses.size()) {
            m_bone_weights.assign(poses.size(), 1.0f);
        }
    }
    std::copy(poses.begin(), poses.end(), m_ragdoll_pose.begin());
}

void RagdollBlendController::set_animated_pose(std::span<const LocalPose> poses) {
    if (m_animated_pose.size() != poses.size()) {
        m_animated_pose.resize(poses.size());
        if (m_bone_weights.size() != poses.size()) {
            m_bone_weights.assign(poses.size(), 1.0f);
        }
    }
    std::copy(poses.begin(), poses.end(), m_animated_pose.begin());
}

void RagdollBlendController::update(f32 dt) {
    if (dt <= 0.0f) {
        return;
    }

    if (m_state == RagdollState::BlendingToRagdoll) {
        m_transition_time += dt;
        f32 alpha = smoothstep(0.0f, m_transition_duration, m_transition_time);
        m_global_weight = alpha;
        if (m_transition_time >= m_transition_duration) {
            m_state = RagdollState::Ragdoll;
            m_global_weight = 1.0f;
        }
    } else if (m_state == RagdollState::BlendingToAnimation) {
        m_transition_time += dt;
        f32 alpha = smoothstep(0.0f, m_transition_duration, m_transition_time);
        m_global_weight = 1.0f - alpha;
        if (m_transition_time >= m_transition_duration) {
            m_state = RagdollState::Kinematic;
            m_global_weight = 0.0f;
        }
    }
}

void RagdollBlendController::evaluate(std::vector<LocalPose>& out_pose) const {
    usize count = std::min(m_ragdoll_pose.size(), m_animated_pose.size());
    if (out_pose.size() != count) {
        out_pose.resize(count);
    }

    if (m_global_weight <= 1e-4f) {
        std::copy_n(m_animated_pose.begin(), count, out_pose.begin());
        return;
    }

    if (m_global_weight >= 1.0f - 1e-4f) {
        for (usize i = 0; i < count; ++i) {
            f32 w = (i < m_bone_weights.size()) ? m_bone_weights[i] : 1.0f;
            if (w >= 1.0f - 1e-4f) {
                out_pose[i] = m_ragdoll_pose[i];
            } else if (w <= 1e-4f) {
                out_pose[i] = m_animated_pose[i];
            } else {
                out_pose[i].translation = lerp_vec3(m_animated_pose[i].translation, m_ragdoll_pose[i].translation, w);
                out_pose[i].rotation = Quat::slerp(m_animated_pose[i].rotation, m_ragdoll_pose[i].rotation, w);
                out_pose[i].scale = lerp_vec3(m_animated_pose[i].scale, m_ragdoll_pose[i].scale, w);
            }
        }
        return;
    }

    for (usize i = 0; i < count; ++i) {
        f32 bone_w = (i < m_bone_weights.size()) ? m_bone_weights[i] : 1.0f;
        f32 effective_w = m_global_weight * bone_w;

        if (effective_w <= 1e-4f) {
            out_pose[i] = m_animated_pose[i];
        } else if (effective_w >= 1.0f - 1e-4f) {
            out_pose[i] = m_ragdoll_pose[i];
        } else {
            out_pose[i].translation = lerp_vec3(m_animated_pose[i].translation, m_ragdoll_pose[i].translation, effective_w);
            out_pose[i].rotation = Quat::slerp(m_animated_pose[i].rotation, m_ragdoll_pose[i].rotation, effective_w);
            out_pose[i].scale = lerp_vec3(m_animated_pose[i].scale, m_ragdoll_pose[i].scale, effective_w);
        }
    }
}

GroundOrientation RagdollBlendController::detect_ground_orientation(
    const Quat& chest_world_rot,
    const Vec3& world_up
) {
    Vec3 chest_forward = chest_world_rot.rotate(Vec3{0.0f, 0.0f, 1.0f});
    Vec3 chest_right   = chest_world_rot.rotate(Vec3{1.0f, 0.0f, 0.0f});

    f32 forward_dot = chest_forward.dot(world_up.normalized());
    if (forward_dot > 0.5f) {
        return GroundOrientation::FaceUp;
    }
    if (forward_dot < -0.5f) {
        return GroundOrientation::FaceDown;
    }

    f32 right_dot = chest_right.dot(world_up.normalized());
    if (right_dot > 0.0f) {
        return GroundOrientation::SideLeft;
    }
    return GroundOrientation::SideRight;
}

Vec3 RagdollBlendController::compute_root_alignment_offset(
    const Vec3& ragdoll_pelvis_pos,
    const Vec3& anim_root_pos
) noexcept {
    // Project difference onto ground plane (XZ)
    return Vec3{
        ragdoll_pelvis_pos.x - anim_root_pos.x,
        0.0f,
        ragdoll_pelvis_pos.z - anim_root_pos.z
    };
}

} // namespace nf::animation
