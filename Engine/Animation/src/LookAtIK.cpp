#include <NF/Animation/LookAtIK.hpp>

#include <algorithm>
#include <cmath>

namespace nf::animation {

namespace {

constexpr f32 k_pi = 3.14159265358979323846f;
constexpr f32 k_rad_to_deg = 180.0f / k_pi;
constexpr f32 k_deg_to_rad = k_pi / 180.0f;

} // namespace

LookAtSolver::LookAtSolver(LookAtConfig config)
    : m_config(std::move(config)) {}

void LookAtSolver::add_bone(std::string name, f32 weight, Vec3 local_forward, Vec3 local_up) {
    LookAtBone bone;
    bone.name = std::move(name);
    bone.weight = std::max(0.0f, weight);
    bone.local_forward = local_forward.normalized();
    bone.local_up = local_up.normalized();
    bone.current_offset = Quat::identity();
    m_bones.push_back(std::move(bone));
}

void LookAtSolver::clear_bones() noexcept {
    m_bones.clear();
}

LookAtBone* LookAtSolver::get_bone(usize index) {
    if (index >= m_bones.size()) {
        return nullptr;
    }
    return &m_bones[index];
}

const LookAtBone* LookAtSolver::get_bone(usize index) const {
    if (index >= m_bones.size()) {
        return nullptr;
    }
    return &m_bones[index];
}

void LookAtSolver::reset() noexcept {
    m_current_rot = Quat::identity();
    m_result = LookAtResult{};
    for (auto& bone : m_bones) {
        bone.current_offset = Quat::identity();
    }
}

LookAtResult LookAtSolver::update(
    const Vec3& character_pos,
    const Vec3& character_facing,
    const Vec3& target_world_pos,
    f32 dt
) {
    Vec3 to_target = target_world_pos - character_pos;
    f32 dist_sq = to_target.dot(to_target);
    if (dist_sq < 1e-6f) {
        return m_result;
    }

    f32 inv_dist = 1.0f / std::sqrt(dist_sq);
    Vec3 aim_dir = to_target * inv_dist;

    Vec3 F = character_facing.normalized();
    Vec3 world_up = Vec3{0.0f, 1.0f, 0.0f};
    if (std::abs(F.dot(world_up)) > 0.99f) {
        world_up = Vec3{0.0f, 0.0f, 1.0f};
    }

    Vec3 R = world_up.cross(F).normalized();
    Vec3 U = F.cross(R).normalized();

    f32 fwd_comp = aim_dir.dot(F);
    f32 right_comp = aim_dir.dot(R);
    f32 up_comp = aim_dir.dot(U);

    f32 target_yaw_rad = std::atan2(right_comp, fwd_comp);
    f32 horiz_len = std::sqrt(right_comp * right_comp + fwd_comp * fwd_comp);
    f32 target_pitch_rad = std::atan2(up_comp, horiz_len);

    f32 target_yaw_deg = target_yaw_rad * k_rad_to_deg;
    f32 target_pitch_deg = target_pitch_rad * k_rad_to_deg;

    m_result.target_yaw_deg = target_yaw_deg;
    m_result.target_pitch_deg = target_pitch_deg;

    f32 clamped_yaw_deg = std::clamp(target_yaw_deg, m_config.limits.yaw_min_deg, m_config.limits.yaw_max_deg);
    f32 clamped_pitch_deg = std::clamp(target_pitch_deg, m_config.limits.pitch_min_deg, m_config.limits.pitch_max_deg);

    m_result.clamped = (clamped_yaw_deg != target_yaw_deg) || (clamped_pitch_deg != target_pitch_deg);

    f32 deviation_deg = std::sqrt(clamped_yaw_deg * clamped_yaw_deg + clamped_pitch_deg * clamped_pitch_deg);
    m_result.in_deadzone = deviation_deg < m_config.limits.deadzone_angle_deg;

    if (m_result.in_deadzone) {
        clamped_yaw_deg = 0.0f;
        clamped_pitch_deg = 0.0f;
    }

    Quat q_yaw = Quat::from_axis_angle(U, clamped_yaw_deg * k_deg_to_rad);
    Quat q_pitch = Quat::from_axis_angle(R, -clamped_pitch_deg * k_deg_to_rad);
    Quat target_rot = (q_yaw * q_pitch).normalized();

    if (dt > 0.0f) {
        f32 factor = 1.0f - std::exp(-m_config.smoothing_speed * dt);
        factor = std::clamp(factor, 0.0f, 1.0f);
        m_current_rot = Quat::slerp(m_current_rot, target_rot, factor).normalized();
    } else {
        m_current_rot = target_rot;
    }

    m_result.current_look_dir = m_current_rot.rotate(F).normalized();

    f32 cur_fwd = m_result.current_look_dir.dot(F);
    f32 cur_right = m_result.current_look_dir.dot(R);
    f32 cur_up = m_result.current_look_dir.dot(U);
    f32 cur_horiz = std::sqrt(cur_right * cur_right + cur_fwd * cur_fwd);

    m_result.current_yaw_deg = std::atan2(cur_right, cur_fwd) * k_rad_to_deg;
    m_result.current_pitch_deg = std::atan2(cur_up, cur_horiz) * k_rad_to_deg;

    f32 total_weight = 0.0f;
    for (const auto& bone : m_bones) {
        total_weight += bone.weight;
    }

    for (auto& bone : m_bones) {
        f32 frac = (total_weight > 1e-4f) ? (bone.weight / total_weight) : 0.0f;
        f32 effective_t = frac * std::clamp(m_config.global_weight, 0.0f, 1.0f);
        bone.current_offset = Quat::slerp(Quat::identity(), m_current_rot, effective_t).normalized();
    }

    return m_result;
}

} // namespace nf::animation
