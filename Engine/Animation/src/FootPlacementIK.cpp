#include <NF/Animation/FootPlacementIK.hpp>

#include <algorithm>
#include <cmath>

namespace nf::animation {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDegToRad = kPi / 180.0f;

} // namespace

FootPlacementSolver::FootPlacementSolver(FootPlacementConfig config)
    : m_config(config) {}

void FootPlacementSolver::evaluate_foot(FootPlacementTarget& foot, const GroundHit& hit, f32 dt) {
    if (hit.has_ground) {
        const f32 ground_y = hit.point.y + m_config.foot_height_offset;
        foot.raw_target_height = std::clamp(
            ground_y - foot.animated_pos.y,
            -m_config.max_step_height,
            m_config.max_step_height
        );
    } else {
        foot.raw_target_height = 0.0f;
    }

    const f32 alpha = dt > 0.0f ? std::clamp(dt * m_config.interp_speed, 0.0f, 1.0f) : 1.0f;
    foot.height_offset = foot.height_offset + (foot.raw_target_height - foot.height_offset) * alpha;

    foot.target_pos = foot.animated_pos;
    foot.target_pos.y += foot.height_offset;

    // Ankle slope alignment
    Quat target_rot = Quat::identity();
    if (hit.has_ground && hit.normal.length_sq() > 1e-4f) {
        const Vec3 norm = hit.normal.normalized();
        const f32 cos_theta = std::clamp(norm.dot(Vec3::up), -1.0f, 1.0f);
        const f32 theta = std::acos(cos_theta);
        const f32 max_rad = m_config.max_ground_angle_deg * kDegToRad;
        const f32 clamped_angle = std::min(theta, max_rad);

        const Vec3 axis = Vec3::up.cross(norm);
        if (axis.length_sq() > 1e-6f && clamped_angle > 1e-4f) {
            target_rot = Quat::from_axis_angle(axis.normalized(), clamped_angle);
        }
    }

    foot.ankle_rotation_offset = Quat::slerp(foot.ankle_rotation_offset, target_rot, alpha);
}

void FootPlacementSolver::solve_biped(
    LegChain& left_leg,
    LegChain& right_leg,
    FootPlacementTarget& left_foot,
    FootPlacementTarget& right_foot,
    const GroundHit& left_hit,
    const GroundHit& right_hit,
    f32 dt,
    f32* out_pelvis_drop
) {
    evaluate_foot(left_foot, left_hit, dt);
    evaluate_foot(right_foot, right_hit, dt);

    f32 target_pelvis = 0.0f;
    if (m_config.enable_pelvis_drop) {
        const f32 lowest = std::min(left_foot.height_offset, right_foot.height_offset);
        if (lowest < 0.0f) {
            target_pelvis = lowest * m_config.pelvis_drop_scale;
        }
    }

    const f32 alpha = dt > 0.0f ? std::clamp(dt * m_config.interp_speed, 0.0f, 1.0f) : 1.0f;
    m_pelvis_drop = m_pelvis_drop + (target_pelvis - m_pelvis_drop) * alpha;

    if (out_pelvis_drop) {
        *out_pelvis_drop = m_pelvis_drop;
    }

    left_leg.hip_pos.y += m_pelvis_drop;
    left_leg.knee_pos.y += m_pelvis_drop;
    left_leg.ankle_pos.y += m_pelvis_drop;

    right_leg.hip_pos.y += m_pelvis_drop;
    right_leg.knee_pos.y += m_pelvis_drop;
    right_leg.ankle_pos.y += m_pelvis_drop;

    left_leg.ik_result = solve_two_bone_ik(
        left_leg.hip_pos,
        left_leg.knee_pos,
        left_leg.ankle_pos,
        left_foot.target_pos,
        left_leg.pole_hint
    );

    right_leg.ik_result = solve_two_bone_ik(
        right_leg.hip_pos,
        right_leg.knee_pos,
        right_leg.ankle_pos,
        right_foot.target_pos,
        right_leg.pole_hint
    );
}

void FootPlacementSolver::reset() noexcept {
    m_pelvis_drop = 0.0f;
}

} // namespace nf::animation
